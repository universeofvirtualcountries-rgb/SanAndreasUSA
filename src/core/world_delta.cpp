#include "world_delta.hpp"

#include "log.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace usa1to1 {
namespace core {

Json WorldDelta::ToJson() const {
    Json root = Json::Object();
    root.Set("version", kVersion);
    if (hasPlayerPos) {
        Json player = Json::Object();
        player.Set("global_x", playerGlobalX);
        player.Set("global_y", playerGlobalY);
        player.Set("z", static_cast<double>(playerZ));
        root.Set("player", player);
    }
    root.Set("money", money);

    if (!doors.empty()) {
        Json doorObj = Json::Object();
        for (const auto &kv : doors) {
            Json d = Json::Object();
            if (kv.second.robbed)
                d.Set("robbed", true);
            if (kv.second.owned)
                d.Set("owned", true);
            if (kv.second.visits)
                d.Set("visits", kv.second.visits);
            // Compact: skip entries that carry no information at all.
            if (d.Size() == 0)
                continue;
            char key[32];
            std::snprintf(key, sizeof(key), "%llu", static_cast<unsigned long long>(kv.first));
            doorObj.Set(key, d);
        }
        if (doorObj.Size() > 0)
            root.Set("doors", doorObj);
    }

    if (!missions.empty()) {
        Json m = Json::Object();
        for (const auto &kv : missions) {
            if (kv.second.status.empty() && kv.second.step == 0)
                continue;
            Json entry = Json::Object();
            entry.Set("status", kv.second.status);
            entry.Set("step", kv.second.step);
            m.Set(kv.first, entry);
        }
        if (m.Size() > 0)
            root.Set("missions", m);
    }
    return root;
}

bool WorldDelta::FromJson(const Json &root, std::string *error) {
    try {
        if (!root.IsObject()) {
            if (error)
                *error = "world delta: root is not an object";
            return false;
        }
        doors.clear();
        missions.clear();
        money = 0;
        hasPlayerPos = false;
        playerGlobalX = playerGlobalY = 0.0;
        playerZ = 0.0f;

        money = root.GetInt("money", 0);
        if (const Json *player = root.Find("player")) {
            playerGlobalX = player->GetDouble("global_x", 0.0);
            playerGlobalY = player->GetDouble("global_y", 0.0);
            playerZ = static_cast<float>(player->GetDouble("z", 0.0));
            hasPlayerPos = true;
        }
        if (const Json *doorObj = root.Find("doors")) {
            for (const auto &kv : doorObj->GetMembers()) {
                DoorState st;
                st.robbed = kv.second.GetBool("robbed", false);
                st.owned = kv.second.GetBool("owned", false);
                st.visits = kv.second.GetInt("visits", 0);
                const uint64_t key = std::strtoull(kv.first.c_str(), nullptr, 10);
                doors[key] = st;
            }
        }
        if (const Json *m = root.Find("missions")) {
            for (const auto &kv : m->GetMembers()) {
                MissionState st;
                st.status = kv.second.GetString("status", "");
                st.step = static_cast<int>(kv.second.GetInt("step", 0));
                missions[kv.first] = st;
            }
        }
        return true;
    } catch (const std::exception &e) {
        if (error)
            *error = e.what();
        return false;
    }
}

bool WorldDelta::SaveToFile(const std::string &path, std::string *error) const {
    namespace fs = std::filesystem;
    try {
        const fs::path p(path);
        if (p.has_parent_path())
            fs::create_directories(p.parent_path());
        const fs::path tmp = p.string() + ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) {
                if (error)
                    *error = "cannot write " + tmp.string();
                return false;
            }
            out << ToJson().Dump(true) << '\n';
            if (!out.good()) {
                if (error)
                    *error = "write failed " + tmp.string();
                return false;
            }
        }
        // Atomic-ish replace, so a crash mid-save never eats the old delta.
        std::error_code ec;
        fs::rename(tmp, p, ec);
        if (ec) {
            fs::remove(p, ec);
            ec.clear();
            fs::rename(tmp, p, ec);
            if (ec) {
                if (error)
                    *error = "rename failed: " + ec.message();
                return false;
            }
        }
        return true;
    } catch (const std::exception &e) {
        if (error)
            *error = e.what();
        return false;
    }
}

bool WorldDelta::LoadFromFile(const std::string &path, std::string *error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error)
            *error = "cannot open " + path;
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    Json root;
    if (!Json::TryParse(ss.str(), root, error))
        return false;
    return FromJson(root, error);
}

WorldDelta WorldDelta::Load(const std::string &path) {
    WorldDelta d;
    std::string err;
    if (d.LoadFromFile(path, &err))
        USA_LOG_I("WorldDelta loaded: %zu doors, %zu missions", d.doors.size(), d.missions.size());
    else
        USA_LOG_I("WorldDelta: fresh start (%s)", err.c_str());
    return d;
}

DoorState &WorldDelta::Door(uint64_t doorSeed) { return doors[doorSeed]; }

const DoorState *WorldDelta::FindDoor(uint64_t doorSeed) const {
    auto it = doors.find(doorSeed);
    return it == doors.end() ? nullptr : &it->second;
}

bool WorldDelta::RobDoor(uint64_t doorSeed, int64_t cashReward) {
    DoorState &d = doors[doorSeed];
    if (d.robbed)
        return false;
    d.robbed = true;
    money += cashReward;
    return true;
}

bool WorldDelta::BuyHouse(uint64_t doorSeed, int64_t price) {
    DoorState &d = doors[doorSeed];
    if (d.owned || money < price)
        return false;
    d.owned = true;
    money -= price;
    return true;
}

MissionState &WorldDelta::Mission(const std::string &id) { return missions[id]; }

const MissionState *WorldDelta::FindMission(const std::string &id) const {
    auto it = missions.find(id);
    return it == missions.end() ? nullptr : &it->second;
}

bool WorldDelta::SetMissionStatus(const std::string &id, const std::string &status, int step) {
    MissionState &m = missions[id];
    m.status = status;
    m.step = step;
    return true;
}

} // namespace core
} // namespace usa1to1
