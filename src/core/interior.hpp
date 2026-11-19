// Deterministic interiors behind every yellow EnEx marker.
//
// Each door gets a 64-bit seed derived from its global position. That seed
// drives a connected 3..7 room layout built from standard game assets, so the
// SECOND time you walk into the same door you get the exact same apartment -
// same rooms, same furniture, same cash register.
//
// interiors live in a pocket at z >= 1000 (kInteriorBaseZ), one pocket per
// area id, which keeps them out of the main world's floating origin entirely.
#pragma once

#include "engine_bridge.hpp"
#include "model_catalog.hpp"
#include "types.hpp"
#include "world_delta.hpp"

#include <cstdint>
#include <vector>

namespace usa1to1 {
namespace core {

struct InteriorRoom {
    int cellX = 0;
    int cellY = 0;
    float centerX = 0.0f;
    float centerY = 0.0f;
    float width = 0.0f;
    float depth = 0.0f;
    bool isExitRoom = false;
};

struct InteriorProp {
    int modelId = -1;
    Vec3f localPos;
    float headingRad = 0.0f;
    PropTag tag = PropTag::Unknown;
};

struct InteriorLayout {
    uint64_t doorSeed = 0;
    uint64_t interiorSeed = 0;
    int areaId = 0;
    float baseZ = kInteriorBaseZ;
    std::vector<InteriorRoom> rooms;
    std::vector<InteriorProp> props;
    Vec3f playerSpawn;
    Vec3f exitMarker;
    Vec3f registerPos;
    bool hasRegister = false;
    std::vector<int> requiredModels;

    size_t RoomCount() const { return rooms.size(); }
    bool Empty() const { return rooms.empty(); }
};

class InteriorGenerator {
  public:
    static constexpr float kCellSize = 14.0f;
    static constexpr int kMaxAreaId = 17;

    explicit InteriorGenerator(const ModelCatalog &catalog);

    InteriorLayout Generate(uint64_t doorSeed, int areaId, float baseZ) const;

    static int AreaIdForSeed(uint64_t doorSeed);
    static float PocketZForArea(int areaId);

    // True when every room is reachable from the exit room through shared walls.
    static bool IsConnected(const InteriorLayout &layout);

  private:
    ModelCatalog m_catalog;
};

class InteriorManager {
  public:
    enum class Event { None, Entered, Exited, Robbed };

    InteriorManager(IEngineBridge &bridge, const InteriorGenerator &generator);

    // Called when the player steps on a door marker.
    bool Enter(uint64_t doorSeed, const Vec3f &doorLocalPos, WorldDelta &delta);

    // Tear down the pocket and put the player back in front of the door.
    void Exit(WorldDelta &delta);

    // Frame update while inside: exit marker proximity + cash register robbery.
    Event Update(const Vec3f &localPlayer, WorldDelta &delta);

    bool IsInside() const { return m_inside; }
    int CurrentArea() const { return m_current.areaId; }
    uint64_t CurrentDoorSeed() const { return m_current.doorSeed; }
    const InteriorLayout &Current() const { return m_current; }
    size_t ObjectCount() const { return m_objects.size(); }
    uint64_t ExitMarkerId() const { return m_exitMarkerId; }

    int64_t TakeRobberyReward() {
        const int64_t r = m_lastReward;
        m_lastReward = 0;
        return r;
    }

    static int64_t RewardForDoor(uint64_t doorSeed);

  private:
    void Teardown();
    void SpawnPendingProps();

    IEngineBridge &m_bridge;
    const InteriorGenerator &m_generator;
    bool m_inside = false;
    InteriorLayout m_current;
    std::vector<ObjectHandle> m_objects;
    std::vector<int> m_models;
    std::vector<size_t> m_pendingProps;
    double m_spawnDeadline = 0.0;
    uint64_t m_exitMarkerId = 0;
    Vec3f m_returnPos;
    int64_t m_lastReward = 0;
};

} // namespace core
} // namespace usa1to1
