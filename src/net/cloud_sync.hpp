// CloudSync: pulls fresh JSON missions + manifest from the GitHub repo.
//
// Network access goes through IHttpTransport. On Android that is a thin JNI
// bridge over java.net.HttpURLConnection (no OpenSSL, no libcurl, ~0 bytes of
// extra .so weight); on the host test suite it's an in-memory fake.
#pragma once

#include "core/json.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace usa1to1 {
namespace net {

using Json = core::Json;

class IHttpTransport {
  public:
    virtual ~IHttpTransport() = default;
    // Whole-body GET into `out`. Returns false on transport/HTTP error.
    virtual bool Get(const std::string &url, std::string &out, int timeoutSec) = 0;
    // Streams a URL into a local file (creates parent dirs). Returns false on error.
    virtual bool Download(const std::string &url, const std::string &localPath, int timeoutSec) = 0;
};

struct RemoteEntry {
    std::string id;
    std::string file;    // relative path inside assets/
    std::string url;
    int version = 0;
    int64_t size = 0;
    std::string hash;    // FNV-1a 64 hex of the file content
};

struct RemoteManifest {
    int version = 0;
    std::vector<RemoteEntry> missions;

    bool Parse(const std::string &text, std::string *error = nullptr);
};

// FNV-1a 64 rendered as 16 lowercase hex chars - cheap, deterministic, and
// plenty for "did this file change" checks.
std::string HashHex(const std::string &data);
std::string HashFileHex(const std::string &path, std::string *error = nullptr);

class CloudSync {
  public:
    enum class State { Idle, Checking, Downloading, UpToDate, NothingToDo, Error };

    struct Result {
        State state = State::NothingToDo;
        int updated = 0;
        int failed = 0;
        std::string error;
        std::vector<std::string> updatedIds;
    };

    // missionsDir: where mission JSONs live. statePath: local version cache.
    CloudSync(IHttpTransport &transport, std::string missionsDir, std::string statePath);

    void SetManifestUrl(const std::string &url) { m_manifestUrl = url; }
    void SetMissionsBaseUrl(const std::string &url) { m_baseUrl = url; }
    const std::string &ManifestUrl() const { return m_manifestUrl; }

    // Blocking; used by tests and by the "Sync now" button.
    Result SyncNow();

    // Fire-and-forget check from the game loop; Poll() applies the outcome.
    void RequestAsync();
    void Poll();
    bool Busy() const { return m_busy.load(); }

    State CurrentState() const { return m_state; }
    std::string LastError() const { return m_lastError; }
    int LastUpdatedCount() const { return m_lastUpdated; }
    size_t LastUpdatedIdsCount() const;

    // Called on the game thread when a poll finds freshly downloaded missions.
    void SetUpdatedCallback(std::function<void(const std::vector<std::string> &)> cb) {
        m_callback = std::move(cb);
    }

    // Local state file helpers (public for tests).
    Json LoadState() const;
    void SaveState(const Json &state) const;

  private:
    Result RunOnce();

    IHttpTransport &m_transport;
    std::string m_missionsDir;
    std::string m_statePath;
    std::string m_manifestUrl;
    std::string m_baseUrl;

    std::atomic<bool> m_busy{false};
    std::thread m_thread;
    std::mutex m_resultMutex;
    Result m_pending;
    bool m_hasPending = false;

    State m_state = State::Idle;
    std::string m_lastError;
    int m_lastUpdated = 0;
    std::vector<std::string> m_lastIds;
    std::function<void(const std::vector<std::string> &)> m_callback;
};

} // namespace net
} // namespace usa1to1
