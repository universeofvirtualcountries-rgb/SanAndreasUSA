#include "chunk_streamer.hpp"

#include "hash.hpp"
#include "log.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace usa1to1 {
namespace core {

ChunkStreamer::ChunkStreamer(IEngineBridge &bridge, const ChunkGenerator &generator, Config config)
    : m_bridge(bridge), m_generator(generator), m_config(config) {
    if (m_config.radius < 0)
        m_config.radius = 0;
    if (m_config.radius > 3)
        m_config.radius = 3; // 7x7 would already violate the +/-1000 local window
    if (m_config.maxChunksPerUpdate < 1)
        m_config.maxChunksPerUpdate = 1;
    if (m_config.maxSpawnsPerUpdate < 1)
        m_config.maxSpawnsPerUpdate = 1;
}

size_t ChunkStreamer::ObjectCount() const {
    size_t n = 0;
    for (const auto &kv : m_active)
        n += kv.second.objects.size();
    return n;
}

size_t ChunkStreamer::PendingCount() const {
    size_t n = 0;
    for (const auto &kv : m_active)
        n += kv.second.pending.size();
    return n;
}

bool ChunkStreamer::IsFullyLoadedAround(const Vec2d &playerGlobal) const {
    const ChunkCoord centre = ChunkGenerator::ChunkOf(playerGlobal);
    for (int dy = -m_config.radius; dy <= m_config.radius; ++dy) {
        for (int dx = -m_config.radius; dx <= m_config.radius; ++dx) {
            const auto key = std::make_pair(centre.x + dx, centre.y + dy);
            if (m_active.find(key) == m_active.end())
                return false;
        }
    }
    return true;
}

int ChunkStreamer::Update(const Vec2d &playerGlobal, const Vec2d &origin) {
    const ChunkCoord centre = ChunkGenerator::ChunkOf(playerGlobal);

    // 1) Unload chunks that left the grid.
    for (auto it = m_active.begin(); it != m_active.end();) {
        const int64_t ddx = it->first.first - centre.x;
        const int64_t ddy = it->first.second - centre.y;
        if (std::llabs(ddx) > m_config.radius || std::llabs(ddy) > m_config.radius) {
            const auto key = it->first;
            ++it;
            UnloadChunk(key);
        } else {
            ++it;
        }
    }

    // 2) Add missing chunks, closest first, throttled.
    std::vector<std::pair<int, std::pair<int64_t, int64_t>>> missing;
    for (int dy = -m_config.radius; dy <= m_config.radius; ++dy) {
        for (int dx = -m_config.radius; dx <= m_config.radius; ++dx) {
            const auto key = std::make_pair(centre.x + dx, centre.y + dy);
            if (m_active.find(key) == m_active.end())
                missing.emplace_back(dx * dx + dy * dy, key);
        }
    }
    std::sort(missing.begin(), missing.end(), [](const auto &a, const auto &b) { return a.first < b.first; });

    int added = 0;
    for (const auto &entry : missing) {
        if (added >= m_config.maxChunksPerUpdate)
            break;
        const auto key = entry.second;
        BuildChunk(m_generator.Generate(key.first, key.second), origin);
        ++added;
    }

    // 3) Retry props that were waiting for their models, with a spawn budget so
    //    a freshly streamed neighbourhood does not drop the frame rate.
    int budget = m_config.maxSpawnsPerUpdate;
    for (auto &kv : m_active) {
        if (budget <= 0)
            break;
        SpawnPendingProps(kv.second, origin, budget);
    }

    const double now = m_bridge.NowSeconds();
    if (m_statsDirty && now >= m_nextStatsLog) {
        USA_LOG_I("ChunkStreamer: requested=%zu loaded=%zu failed=%zu spawn_failed=%zu pending_props=%zu objects=%zu",
                  m_requested, m_loaded, m_failed, m_spawnFailed, PendingCount(), ObjectCount());
        m_statsDirty = false;
        m_nextStatsLog = now + 2.0;
    }
    if (added > 0)
        USA_LOG_D("ChunkStreamer: +%d chunks, active=%d objects=%zu pending=%zu markers=%zu", added,
                  ActiveChunkCount(), ObjectCount(), PendingCount(), MarkerCount());
    return added;
}

void ChunkStreamer::BuildChunk(const ChunkContent &content, const Vec2d &origin) {
    ActiveChunk chunk;
    chunk.coord = content.coord;
    chunk.seed = content.seed;

    chunk.requestedAt = m_bridge.NowSeconds();
    for (int modelId : content.requiredModels) {
        if (modelId < 0)
            continue;
        ++m_requested;
        m_statsDirty = true;
        if (m_bridge.RequestModel(modelId, false))
            chunk.models.emplace(modelId, ActiveChunk::ModelState::Pending);
        else
            ++m_failed;
    }

    chunk.pending = content.props;

    if (m_config.spawnMarkers) {
        for (const auto &door : content.doors) {
            const uint64_t markerId = MarkerIdFromDoorSeed(door.seed);
            const Vec3f local{static_cast<float>(content.originGlobal.x - origin.x) + door.localPos.x,
                              static_cast<float>(content.originGlobal.y - origin.y) + door.localPos.y,
                              door.localPos.z + 0.2f};
            m_bridge.PlaceMarker(markerId, local, 1.0f, false);
            chunk.markerIds.push_back(markerId);
            m_doorMarkers[markerId] = DoorMarkerInfo{door, content.coord, local};
        }
    }

    const auto key = std::make_pair(content.coord.x, content.coord.y);
    m_active.emplace(key, std::move(chunk));
}

bool ChunkStreamer::SpawnPendingProps(ActiveChunk &chunk, const Vec2d &origin, int &budget) {
    if (chunk.pending.empty())
        return false;

    const Vec2d chunkOrigin = ChunkGenerator::ChunkOriginGlobal(chunk.coord.x, chunk.coord.y);
    bool spawnedAny = false;
    const double now = m_bridge.NowSeconds();
    for (auto it = chunk.pending.begin(); it != chunk.pending.end();) {
        if (budget <= 0)
            break;
        const auto state = chunk.models.find(it->modelId);
        using State = ActiveChunk::ModelState;
        if (state == chunk.models.end() || state->second == State::Failed) {
            it = chunk.pending.erase(it);
            continue;
        }
        if (!m_bridge.IsModelLoaded(it->modelId)) {
            if (now - chunk.requestedAt >= 10.0) {
                state->second = State::Failed;
                ++m_failed;
                m_statsDirty = true;
                it = chunk.pending.erase(it);
            } else {
                ++it;
            }
            continue;
        }
        if (state->second == State::Pending) {
            state->second = State::Loaded;
            ++m_loaded;
            m_statsDirty = true;
        }

        SpawnParams params;
        params.modelId = it->modelId;
        params.pos = {static_cast<float>(chunkOrigin.x - origin.x) + it->localPos.x,
                      static_cast<float>(chunkOrigin.y - origin.y) + it->localPos.y, it->localPos.z};
        params.headingRad = it->headingRad;
        params.dynamic = false;
        params.interior = false;
        params.collision = true;
        params.ownerTag = chunk.seed;

        StreamedObject obj;
        obj.modelId = it->modelId;
        obj.handle = m_bridge.CreateObject(params);
        --budget;
        if (obj.handle) {
            chunk.objects.push_back(obj);
            spawnedAny = true;
        } else if (!m_bridge.IsModelLoaded(it->modelId) && now - chunk.requestedAt < 10.0) {
            // A model can lose its live resources during creation. Leave it
            // queued and let the regular readiness check retry on a later tick.
            ++it;
            continue;
        } else {
            ++m_spawnFailed;
            m_statsDirty = true;
        }
        it = chunk.pending.erase(it);
    }
    return spawnedAny;
}

void ChunkStreamer::UnloadChunk(std::pair<int64_t, int64_t> key) {
    auto it = m_active.find(key);
    if (it == m_active.end())
        return;

    for (const auto &obj : it->second.objects)
        m_bridge.DestroyObject(obj.handle);
    for (uint64_t markerId : it->second.markerIds) {
        m_bridge.RemoveMarker(markerId);
        m_doorMarkers.erase(markerId);
    }
    for (const auto &entry : it->second.models)
        m_bridge.ReleaseModel(entry.first);

    m_active.erase(it);
}

void ChunkStreamer::OnOriginShift(const Vec3f &delta) {
    // Keep cached door positions in sync (the adapter re-places the real
    // C3dMarkers itself in ShiftStreamedEntities).
    for (auto &kv : m_doorMarkers)
        kv.second.localPos += delta;
}

ChunkStreamer::DoorHit ChunkStreamer::FindDoorAt(const Vec3f &localPlayer, float maxDistance) const {
    DoorHit best;
    best.distance = maxDistance;
    for (const auto &kv : m_doorMarkers) {
        const Vec3f &p = kv.second.localPos;
        const float dx = p.x - localPlayer.x;
        const float dy = p.y - localPlayer.y;
        const float dz = p.z - localPlayer.z;
        if (dz > 3.0f || dz < -3.0f)
            continue; // different floor / different world pocket
        const float d = std::sqrt(dx * dx + dy * dy);
        if (d <= best.distance) {
            best.valid = true;
            best.distance = d;
            best.door = kv.second.door;
            best.coord = kv.second.coord;
            best.markerId = kv.first;
            best.doorLocalPos = p;
        }
    }
    return best;
}

void ChunkStreamer::Shutdown() {
    std::vector<std::pair<int64_t, int64_t>> keys;
    keys.reserve(m_active.size());
    for (const auto &kv : m_active)
        keys.push_back(kv.first);
    for (const auto &k : keys)
        UnloadChunk(k);
    m_active.clear();
    m_doorMarkers.clear();
}

} // namespace core
} // namespace usa1to1
