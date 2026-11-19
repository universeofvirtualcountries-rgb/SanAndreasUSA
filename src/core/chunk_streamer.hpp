// Keeps a 3x3 / 5x5 chunk grid alive around the player.
//
// Responsibilities:
//  * decide which chunks to load / unload from the player's global position,
//  * drive model requests + object spawns through IEngineBridge,
//  * retry props whose model was still streaming (never silently drop them),
//  * keep EnEx door markers alive and answer "is the player on a door".
#pragma once

#include "chunk_generator.hpp"
#include "engine_bridge.hpp"
#include "types.hpp"

#include <cstdint>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

namespace usa1to1 {
namespace core {

struct StreamedObject {
    ObjectHandle handle;
    int modelId = -1;
    bool interior = false;
};

struct ActiveChunk {
    ChunkCoord coord;
    uint64_t seed = 0;
    std::vector<StreamedObject> objects;
    std::vector<ChunkProp> pending; // props waiting for their model to load
    std::vector<uint64_t> markerIds;
    enum class ModelState { Pending, Loaded, Failed };
    std::unordered_map<int, ModelState> models; // accepted requests only
    double requestedAt = 0.0;
};

struct DoorMarkerInfo {
    ChunkDoor door;
    ChunkCoord coord;
    Vec3f localPos; // updated on every floating-origin shift
};

class ChunkStreamer {
  public:
    struct Config {
        double chunkSize = ChunkGenerator::kChunkSize;
        int radius = 2;             // 2 => 5x5, 1 => 3x3
        int maxChunksPerUpdate = 4; // avoid a 25-chunk hitch on spawn
        int maxSpawnsPerUpdate = 48;
        bool spawnMarkers = true;
    };

    ChunkStreamer(IEngineBridge &bridge, const ChunkGenerator &generator, Config config);

    // playerGlobal: player position on the 1:1 plane.
    // origin: current floating-origin offset (local = global - origin).
    int Update(const Vec2d &playerGlobal, const Vec2d &origin);

    // Floating origin moved by `delta` (local space). Free-standing markers are
    // re-placed by the adapter; we only need to update our cached positions.
    void OnOriginShift(const Vec3f &delta);

    void Shutdown();

    int ActiveChunkCount() const { return static_cast<int>(m_active.size()); }
    size_t ObjectCount() const;
    size_t MarkerCount() const { return m_doorMarkers.size(); }
    size_t PendingCount() const;
    const std::map<std::pair<int64_t, int64_t>, ActiveChunk> &Active() const { return m_active; }

    struct DoorHit {
        bool valid = false;
        ChunkCoord coord;
        ChunkDoor door{};
        uint64_t markerId = 0;
        float distance = 1e9f;
        Vec3f doorLocalPos; // local position of the door marker (stand point)
    };
    DoorHit FindDoorAt(const Vec3f &localPlayer, float maxDistance = 1.6f) const;

    bool IsFullyLoadedAround(const Vec2d &playerGlobal) const;
    const Config &GetConfig() const { return m_config; }

  private:
    void UnloadChunk(std::pair<int64_t, int64_t> key);
    void BuildChunk(const ChunkContent &content, const Vec2d &origin);
    bool SpawnPendingProps(ActiveChunk &chunk, const Vec2d &origin, int &budget);

    IEngineBridge &m_bridge;
    const ChunkGenerator &m_generator;
    Config m_config;
    std::map<std::pair<int64_t, int64_t>, ActiveChunk> m_active;
    std::unordered_map<uint64_t, DoorMarkerInfo> m_doorMarkers;
    size_t m_requested = 0, m_loaded = 0, m_failed = 0, m_spawnFailed = 0;
    bool m_statsDirty = false;
    double m_nextStatsLog = 0.0;
};

} // namespace core
} // namespace usa1to1
