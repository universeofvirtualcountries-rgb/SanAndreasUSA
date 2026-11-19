// world_delta.json - only the player's changes are persisted.
//
// The 1:1 world itself is a pure function of global coordinates and the model
// catalogue, so it must never be saved. What does get saved: where the player
// is on the plane, which cash registers are already emptied, which houses were
// bought, and how far each JSON mission got.
#pragma once

#include "json.hpp"
#include "types.hpp"

#include <cstdint>
#include <map>
#include <string>

namespace usa1to1 {
namespace core {

struct DoorState {
    bool robbed = false;
    bool owned = false;
    int64_t visits = 0;
};

struct MissionState {
    std::string status; // "available" | "active" | "completed" | "failed"
    int step = 0;
};

class WorldDelta {
  public:
    static constexpr int kVersion = 1;

    // Player position on the 1:1 plane (metres, double).
    double playerGlobalX = 0.0;
    double playerGlobalY = 0.0;
    float playerZ = 0.0f;
    bool hasPlayerPos = false;

    int64_t money = 0;

    std::map<uint64_t, DoorState> doors;            // key: door seed
    std::map<std::string, MissionState> missions;   // key: mission id

    // --- persistence ---------------------------------------------------------
    Json ToJson() const;
    bool FromJson(const Json &root, std::string *error = nullptr);

    bool SaveToFile(const std::string &path, std::string *error = nullptr) const;
    bool LoadFromFile(const std::string &path, std::string *error = nullptr);
    static WorldDelta Load(const std::string &path);

    // --- gameplay helpers ----------------------------------------------------
    DoorState &Door(uint64_t doorSeed);
    const DoorState *FindDoor(uint64_t doorSeed) const;
    bool RobDoor(uint64_t doorSeed, int64_t cashReward);
    bool BuyHouse(uint64_t doorSeed, int64_t price);
    MissionState &Mission(const std::string &id);
    const MissionState *FindMission(const std::string &id) const;
    bool SetMissionStatus(const std::string &id, const std::string &status, int step);

    void SetPlayerPosition(double gx, double gy, float z) {
        playerGlobalX = gx;
        playerGlobalY = gy;
        playerZ = z;
        hasPlayerPos = true;
    }
};

} // namespace core
} // namespace usa1to1
