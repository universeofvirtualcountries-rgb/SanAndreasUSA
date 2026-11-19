#include "cloud_sync.hpp"

#include "core/hash.hpp"
#include "core/log.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace usa1to1 {
namespace net {
namespace fs = std::filesystem;
using core::Json;

std::string HashHex(const std::string &data) {
    static const char *kHex = "0123456789abcdef";
    const uint64_t h = core::Fnv1a64(data.data(), data.size());
    std::string out(16, '0');
    for (int i = 0; i < 16; ++i)
        out[15 - i] = kHex[(h >> (i * 4)) & 0xF];
    return out;
}

std::string HashFileHex(const std::string &path, std::string *error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error)
            *error = "cannot open " + path;
        return {};
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return HashHex(ss.str());
}

bool RemoteManifest::Parse(const std::string &text, std::string *error) {
    Json root;
    if (!Json::TryParse(text, root, error))
        return false;
    try {
        version = static_cast<int>(root.GetInt("version", 0));
        missions.clear();
        const Json *arr = root.Find("missions");
        if (arr && arr->IsArray()) {
            for (const auto &m : arr->GetArray()) {
                RemoteEntry e;
                e.id = m.GetString("id", "");
                e.file = m.GetString("file", "");
                e.url = m.GetString("url", "");
                e.version = static_cast<int>(m.GetInt("version", 0));
                e.size = m.GetInt("size", 0);
                e.hash = m.GetString("hash", "");
                if (e.id.empty() && !e.file.empty())
                    e.id = fs::path(e.file).stem().string();
                if (e.id.empty())
                    continue;
                missions.push_back(e);
            }
        }
        return true;
    } catch (const std::exception &e) {
        if (error)
            *error = e.what();
        return false;
    }
}

CloudSync::CloudSync(IHttpTransport &transport, std::string missionsDir, std::string statePath)
    : m_transport(transport), m_missionsDir(std::move(missionsDir)), m_statePath(std::move(statePath)) {}

size_t CloudSync::LastUpdatedIdsCount() const { return m_lastIds.size(); }

Json CloudSync::LoadState() const {
    std::ifstream in(m_statePath, std::ios::binary);
    if (!in)
        return Json::Object();
    std::stringstream ss;
    ss << in.rdbuf();
    Json root;
    if (!Json::TryParse(ss.str(), root) || !root.IsObject())
        return Json::Object();
    return root;
}

void CloudSync::SaveState(const Json &state) const {
    std::error_code ec;
    const fs::path p(m_statePath);
    if (p.has_parent_path())
        fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out)
        return;
    out << state.Dump(true) << '\n';
}

CloudSync::Result CloudSync::RunOnce() {
    Result result;
    m_state = State::Checking;

    if (m_manifestUrl.empty()) {
        result.state = State::Error;
        result.error = "manifest url is empty";
        m_state = State::Error;
        m_lastError = result.error;
        return result;
    }

    std::string manifestText;
    if (!m_transport.Get(m_manifestUrl, manifestText, 15)) {
        result.state = State::Error;
        result.error = "manifest download failed";
        m_state = State::Error;
        m_lastError = result.error;
        USA_LOG_W("CloudSync: %s (%s)", result.error.c_str(), m_manifestUrl.c_str());
        return result;
    }

    RemoteManifest manifest;
    std::string parseError;
    if (!manifest.Parse(manifestText, &parseError)) {
        result.state = State::Error;
        result.error = "manifest parse: " + parseError;
        m_state = State::Error;
        m_lastError = result.error;
        return result;
    }

    Json state = LoadState();
    Json missionsState = state.Find("missions") ? *state.Find("missions") : Json::Object();
    bool stateChanged = false;
    m_state = State::Downloading;

    std::error_code ec;
    fs::create_directories(m_missionsDir, ec);

    for (const auto &entry : manifest.missions) {
        std::string fileName = entry.file;
        if (fileName.empty())
            fileName = "missions/" + entry.id + ".json";
        const std::string baseName = fs::path(fileName).filename().string();
        const std::string localPath = (fs::path(m_missionsDir) / baseName).string();

        const Json &cached = missionsState[entry.id];
        const int cachedVersion = static_cast<int>(cached.GetInt("version", 0));
        const std::string cachedHash = cached.GetString("hash", "");

        std::string localHash;
        if (fs::exists(localPath, ec))
            localHash = HashFileHex(localPath, nullptr);

        const bool missing = localHash.empty();
        const bool changed = !missing && !entry.hash.empty() && localHash != entry.hash;
        const bool newer = !missing && entry.version > cachedVersion;

        if (!missing && !changed && !newer) {
            continue; // already current
        }

        std::string url = entry.url;
        if (url.empty()) {
            std::string base = m_baseUrl;
            if (!base.empty() && base.back() != '/')
                base += '/';
            url = base + baseName;
        }

        const std::string tmpPath = localPath + ".download";
        if (!m_transport.Download(url, tmpPath, 20)) {
            ++result.failed;
            USA_LOG_W("CloudSync: download failed %s", url.c_str());
            fs::remove(tmpPath, ec);
            continue;
        }

        const std::string newHash = HashFileHex(tmpPath, nullptr);
        if (!entry.hash.empty() && !newHash.empty() && newHash != entry.hash) {
            ++result.failed;
            USA_LOG_W("CloudSync: hash mismatch for %s (got %s want %s)", entry.id.c_str(),
                            newHash.c_str(), entry.hash.c_str());
            fs::remove(tmpPath, ec);
            continue;
        }

        fs::rename(tmpPath, localPath, ec);
        if (ec) {
            fs::remove(localPath, ec);
            ec.clear();
            fs::rename(tmpPath, localPath, ec);
            if (ec) {
                ++result.failed;
                continue;
            }
        }

        Json entryState = Json::Object();
        entryState.Set("version", entry.version);
        entryState.Set("hash", newHash.empty() ? entry.hash : newHash);
        missionsState.Set(entry.id, entryState);
        stateChanged = true;

        result.updatedIds.push_back(entry.id);
        ++result.updated;
        USA_LOG_I("CloudSync: updated %s (v%d)", entry.id.c_str(), entry.version);
    }

    if (stateChanged) {
        state.Set("version", 1);
        state.Set("missions", missionsState);
        SaveState(state);
    }

    result.state = result.failed > 0 ? State::Error : (result.updated > 0 ? State::UpToDate : State::NothingToDo);
    if (result.failed > 0)
        result.error = "some downloads failed";

    {
        std::lock_guard<std::mutex> lock(m_resultMutex);
        m_pending = result;
        m_hasPending = true;
    }
    m_state = result.state;
    m_lastError = result.error;
    m_lastUpdated = result.updated;
    m_lastIds = result.updatedIds;
    return result;
}

CloudSync::Result CloudSync::SyncNow() { return RunOnce(); }

void CloudSync::RequestAsync() {
    if (m_busy.exchange(true))
        return;
    if (m_thread.joinable())
        m_thread.join();
    m_thread = std::thread([this]() {
        RunOnce();
        m_busy.store(false);
    });
}

void CloudSync::Poll() {
    if (m_busy.load())
        return;
    if (m_thread.joinable())
        m_thread.join();

    Result result;
    {
        std::lock_guard<std::mutex> lock(m_resultMutex);
        if (!m_hasPending)
            return;
        result = m_pending;
        m_hasPending = false;
    }
    m_state = result.state;
    m_lastError = result.error;
    m_lastUpdated = result.updated;
    m_lastIds = result.updatedIds;
    if (!result.updatedIds.empty() && m_callback)
        m_callback(result.updatedIds);
}

} // namespace net
} // namespace usa1to1
