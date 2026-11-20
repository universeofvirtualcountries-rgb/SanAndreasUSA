#include "test_polygon.hpp"

#include "hash.hpp"
#include "heightfield.hpp"
#include "log.hpp"

#include <algorithm>
#include <cmath>

namespace usa1to1 {
namespace core {

TestPolygon::TestPolygon(IEngineBridge &bridge, const ModelCatalog &catalog) : TestPolygon(bridge, catalog, Config()) {}

TestPolygon::TestPolygon(IEngineBridge &bridge, const ModelCatalog &catalog, Config config)
    : m_bridge(bridge), m_catalog(catalog), m_config(config) {
    m_config.generationZ = HeightField::ClampGenerationZ(m_config.generationZ);
    if (m_config.tileLength < 5.0)
        m_config.tileLength = 20.0;
    if (m_config.markerSpacing < 50.0)
        m_config.markerSpacing = 200.0;
}

int64_t TestPolygon::TileIndexAt(double globalY) const {
    return static_cast<int64_t>(std::floor((globalY - m_startY) / m_tileLength));
}

bool TestPolygon::Enable(const Vec2d &playerGlobal, const Vec2d &origin) {
    Disable();
    m_roadX = playerGlobal.x;
    m_candidates.clear();
    if (const auto *road = m_catalog.PickRoad(m_bridge.ProbeRoadModel()))
        m_candidates.push_back(road->id);
    for (const int id : kRoadFallbackIds) {
        const auto *road = m_catalog.Find(id);
        if (road && IsFlatRoadCandidate(*road) && (m_candidates.empty() || id != m_candidates.front()))
            m_candidates.push_back(id);
    }
    m_candidate = 0;
    m_active = true;
    m_nextRecoveryLog = m_bridge.NowSeconds();
    SelectRoad(playerGlobal);
    RestorePlayerHeight(origin, false);
    UpdateTiles(playerGlobal, origin, false); // pin now; first Create is on a later tick
    return true;
}

std::string TestPolygon::StatusLine() const {
    return "Catalog: " + std::to_string(m_catalog.Size()) + " | RoadID: " + std::to_string(m_roadModel) +
           " | Loaded: " + (m_modelLoaded ? "yes" : "no") + " | Spawned: " + std::to_string(m_tiles.size()) + " | Err: " +
           (!m_active ? "polygon off" : m_roadError.empty() ? "none" : m_roadError);
}

void TestPolygon::SelectRoad(const Vec2d &anchor) {
    const auto *road = m_candidate < m_candidates.size() ? m_catalog.Find(m_candidates[m_candidate]) : nullptr;
    m_roadModel = road ? road->id : -1;
    m_tileLength = m_config.tileLength;
    m_tileHeading = 0.0f;
    m_tileHalfWidth = 0.0f;
    m_tileOffset = {};
    if (road) {
        // Use the longest collision axis along travel. Align the top of the
        // collision box to generationZ, even if the DFF pivot is offset.
        const bool rotate = road->colHalfExtents.x > road->colHalfExtents.y;
        m_tileHeading = rotate ? 1.57079632679f : 0.0f;
        m_tileLength = 1.98 * (rotate ? road->colHalfExtents.x : road->colHalfExtents.y);
        m_tileHalfWidth = rotate ? road->colHalfExtents.y : road->colHalfExtents.x;
        m_tileOffset = rotate ? Vec3f{-road->colOffset.y, road->colOffset.x, road->colOffset.z} : road->colOffset;
        m_tileOffset.z += road->colHalfExtents.z;
    }
    m_startY = anchor.y - m_tileLength * 0.5;
    m_modelLoaded = false;
    m_rejected = false;
    m_requested = road && m_bridge.RequestRoadModel(m_roadModel);
    if (m_requested) {
        m_bridge.FlushModelStreaming(); // once per candidate, outside the tile loop
        m_roadError = "waiting for road collision";
    } else if (const char *reason = road ? m_bridge.RoadSpawnError() : nullptr) {
        m_roadError = reason;
    } else {
        m_roadError = road ? "RequestModel rejected; Add not called" : "no verified road model";
    }
    const double now = m_bridge.NowSeconds();
    m_nextSpawnAttempt = now;
    m_fallbackAt = m_requested ? now + 4.0 : now;
    USA_LOG_I("TestPolygon: trying road model=%d (%zu/%zu)", m_roadModel, m_candidate + 1, m_candidates.size());
}

void TestPolygon::ClearRoad() {
    for (auto &kv : m_tiles)
        m_bridge.DestroyObject(kv.second.handle);
    m_tiles.clear();
    if (m_requested)
        m_bridge.ReleaseRoadModel(m_roadModel);
    m_requested = false;
    m_modelLoaded = false;
}

void TestPolygon::Clear() {
    ClearRoad();
    m_holdAt.reset();
    for (auto &kv : m_markers)
        m_bridge.RemoveMarker(kv.second);
    m_markers.clear();
}

void TestPolygon::Disable() {
    if (!m_active && m_tiles.empty() && m_markers.empty())
        return;
    Clear();
    m_active = false;
}

void TestPolygon::Update(const Vec2d &playerGlobal, const Vec2d &origin) {
    UpdateTiles(playerGlobal, origin, true);
}

void TestPolygon::UpdateTiles(const Vec2d &playerGlobal, const Vec2d &origin, bool allowSpawn) {
    if (!m_active)
        return;

    // --- endless slab strip along +Y ---------------------------------------
    const Vec2d target = m_holdAt.value_or(playerGlobal);
    const int64_t centre = TileIndexAt(target.y);
    const int64_t first = centre - m_config.behindTiles;
    const int64_t last = centre + m_config.aheadTiles;

    for (auto it = m_tiles.begin(); it != m_tiles.end();) {
        if (it->first < first || it->first > last) {
            m_bridge.DestroyObject(it->second.handle);
            it = m_tiles.erase(it);
        } else {
            ++it;
        }
    }

    const bool wasLoaded = m_modelLoaded;
    m_modelLoaded = m_requested && m_bridge.IsRoadModelLoaded(m_roadModel);
    if (m_requested && !m_modelLoaded && !m_rejected) {
        const char *reason = m_bridge.RoadSpawnError();
        m_roadError = reason ? reason : "model not loaded; Add not called";
        if (wasLoaded && m_bridge.RequestRoadModel(m_roadModel)) {
            // One re-request on a loaded -> unloaded edge; no per-tile flush.
            m_bridge.FlushModelStreaming();
            m_fallbackAt = m_bridge.NowSeconds() + 4.0;
        }
    }
    if (allowSpawn && m_modelLoaded && !m_rejected && m_bridge.NowSeconds() >= m_nextSpawnAttempt) {
        int failed = 0;
        // Prioritise the only slab that can release the hold.
        for (int64_t slot = first - 1; slot <= last; ++slot) {
            const int64_t i = slot < first ? centre : slot;
            if ((slot == centre) || m_tiles.count(i))
                continue;
            const double gy = m_startY + (static_cast<double>(i) + 0.5) * m_tileLength;
            const Vec3f local{static_cast<float>(m_roadX - origin.x) - m_tileOffset.x,
                              static_cast<float>(gy - origin.y) - m_tileOffset.y,
                              m_config.generationZ - m_tileOffset.z};
            SpawnParams params;
            params.modelId = m_roadModel;
            params.pos = local;
            params.headingRad = m_tileHeading;
            params.dynamic = false;
            params.interior = false;
            params.collision = true;
            params.roadSlab = true;
            params.ownerTag = Mix64(static_cast<uint64_t>(i) ^ 0x9E3779B9ULL);
            PolygonTile tile;
            tile.index = i;
            tile.handle = m_bridge.CreateObject(params);
            if (tile.handle)
                m_tiles.emplace(i, tile);
            else {
                ++failed;
                m_rejected = m_bridge.RoadCandidateRejected();
                if (i == centre || m_rejected) {
                    const char *reason = m_bridge.RoadSpawnError();
                    m_roadError = reason ? reason : "CreateObject failed; Add not confirmed";
                }
                if (m_rejected)
                    break;
            }
        }
        if (failed) {
            m_nextSpawnAttempt = m_bridge.NowSeconds() + 2.0;
            USA_LOG_W("TestPolygon: model=%d requested=1 loaded=1 failed=%d tile creation(s)", m_roadModel, failed);
        }
    }

    // --- yellow markers every 200 m, snapped to the GLOBAL grid -------------
    const int64_t markerIndex = static_cast<int64_t>(std::floor(target.y / m_config.markerSpacing));
    for (auto it = m_markers.begin(); it != m_markers.end();) {
        if (std::llabs(it->first - markerIndex) > m_config.markerAhead) {
            m_bridge.RemoveMarker(it->second);
            it = m_markers.erase(it);
        } else {
            ++it;
        }
    }
    for (int64_t k = markerIndex - m_config.markerAhead; k <= markerIndex + m_config.markerAhead; ++k) {
        if (m_markers.count(k))
            continue;
        const double gy = static_cast<double>(k) * m_config.markerSpacing;
        const Vec3f local{static_cast<float>(m_roadX - origin.x), static_cast<float>(gy - origin.y),
                          m_config.generationZ + 0.3f};
        const uint64_t id = MarkerIdFromDoorSeed(Mix64(static_cast<uint64_t>(k)) ^ 0x9E1CE5ULL);
        m_bridge.PlaceMarker(id, local, 1.2f, false);
        m_markers.emplace(k, id);
    }

    // A tile elsewhere in the strip cannot catch the player. Keep the
    // waiting anchor in global coordinates so it survives an origin shift.
    const auto supportingTile = m_tiles.find(centre);
    const bool collisionReady = supportingTile != m_tiles.end() &&
                                m_bridge.IsObjectCollisionReady(supportingTile->second.handle);
    if (!collisionReady && supportingTile != m_tiles.end()) {
        m_roadError = "Add done; supporting object lost its collider";
        m_bridge.DestroyObject(supportingTile->second.handle);
        m_tiles.erase(supportingTile);
    }
    const bool supportReady = collisionReady && std::abs(target.x - m_roadX) <= m_tileHalfWidth &&
                              m_bridge.HasRoadSupportAt(supportingTile->second.handle,
                                  {static_cast<float>(target.x - origin.x), static_cast<float>(target.y - origin.y),
                                   m_config.generationZ});
    if (!supportReady || (m_rejected && m_candidate + 1 < m_candidates.size())) {
        if (!m_rejected && collisionReady) {
            if (std::abs(target.x - m_roadX) > m_tileHalfWidth)
                m_roadError = "player outside slab bounds";
            else {
                const char *reason = m_bridge.RoadSpawnError();
                m_roadError = reason ? reason : "Add done; downward probe missed";
            }
        }
        if (!m_holdAt) {
            // On an actual collision miss (e.g. terrain with holes), retry from
            // the known tile centre instead of pinning outside the strip forever.
            m_holdAt = collisionReady || std::abs(target.x - m_roadX) > m_tileHalfWidth
                           ? Vec2d{m_roadX, m_startY + (static_cast<double>(centre) + 0.5) * m_tileLength}
                           : target;
            USA_LOG_I("TestPolygon: holding player until collision tile %lld is ready", static_cast<long long>(centre));
        }
        HoldPlayer(origin);
        if (allowSpawn && m_candidate + 1 < m_candidates.size() && (m_rejected || m_bridge.NowSeconds() >= m_fallbackAt)) {
            const std::string previous = m_roadError;
            USA_LOG_W("TestPolygon: retry with fallback after %s", StatusLine().c_str());
            ClearRoad();
            ++m_candidate;
            SelectRoad(*m_holdAt);
            if (m_requested)
                m_roadError = "fallback after " + previous;
        }
        return;
    }
    if (!m_rejected)
        m_roadError.clear();
    m_fallbackAt = m_bridge.NowSeconds() + 4.0;
    if (m_holdAt) {
        HoldPlayer(origin);
        m_holdAt.reset();
        USA_LOG_I("TestPolygon: supporting tile %lld ready; releasing hold", static_cast<long long>(centre));
        return;
    }
    Vec3f player;
    if (m_bridge.GetPlayerPosition(player) && player.z < m_config.generationZ - 5.0f)
        RestorePlayerHeight(origin, true);
}

std::optional<Vec3f> TestPolygon::TestCarPosition(const Vec2d &playerGlobal, const Vec2d &origin) {
    if (!m_active || m_holdAt)
        return std::nullopt;
    for (const double ahead : {7.0, -7.0}) {
        const double y = playerGlobal.y + ahead;
        const double dx = m_roadX - playerGlobal.x;
        if (std::abs(dx) > 10.0)
            return std::nullopt;
        const auto tile = m_tiles.find(TileIndexAt(y));
        if (tile == m_tiles.end() || !m_bridge.IsObjectCollisionReady(tile->second.handle))
            continue;
        const Vec3f surface{static_cast<float>(m_roadX - origin.x), static_cast<float>(y - origin.y), m_config.generationZ};
        if (m_bridge.HasRoadSupportAt(tile->second.handle, surface))
            return Vec3f{surface.x, surface.y, surface.z + 1.0f};
    }
    return std::nullopt;
}

void TestPolygon::HoldPlayer(const Vec2d &origin) {
    m_bridge.HoldPlayerPosition({static_cast<float>(m_holdAt->x - origin.x),
                                 static_cast<float>(m_holdAt->y - origin.y), m_config.generationZ + 1.5f});
}

void TestPolygon::OnOriginShift(const Vec2d &origin) {
    if (m_holdAt)
        HoldPlayer(origin);
    else if (m_active)
        RestorePlayerHeight(origin, false);
}

void TestPolygon::RestorePlayerHeight(const Vec2d &origin, bool recenter) {
    Vec3f player;
    if (!m_bridge.GetPlayerPosition(player))
        return;
    if (recenter) {
        if (m_bridge.NowSeconds() >= m_nextRecoveryLog) {
            USA_LOG_W("TestPolygon: fall recovery from z=%.1f to %.1f", player.z, m_config.generationZ + 2.0f);
            m_nextRecoveryLog = m_bridge.NowSeconds() + 2.0;
        }
        player.x = static_cast<float>(m_roadX - origin.x);
    }
    player.z = m_config.generationZ + 2.0f;
    m_bridge.RecoverPlayerPosition(player);
}

} // namespace core
} // namespace usa1to1
