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
    m_roadModel = m_bridge.ProbeRoadModel();
    if (m_roadModel < 0) {
        if (const auto *road = m_catalog.PickRoad())
            m_roadModel = road->id;
    }
    m_tileLength = m_config.tileLength;
    m_tileHeading = 0.0f;
    m_tileOffset = {};
    if (const auto *road = m_catalog.Find(m_roadModel); road && IsFlatRoadCandidate(*road)) {
        // Use the longest collision axis along travel. Align the top of the
        // collision box to generationZ, even if the DFF pivot is offset.
        const bool rotate = road->colHalfExtents.x > road->colHalfExtents.y;
        m_tileHeading = rotate ? 1.57079632679f : 0.0f;
        m_tileLength = 1.98 * (rotate ? road->colHalfExtents.x : road->colHalfExtents.y);
        m_tileOffset = rotate ? Vec3f{-road->colOffset.y, road->colOffset.x, road->colOffset.z} : road->colOffset;
        m_tileOffset.z += road->colHalfExtents.z;
    }
    m_startY = playerGlobal.y - m_tileLength * 0.5;
    m_requested = m_roadModel >= 0 && m_bridge.RequestModel(m_roadModel, false);
    if (m_requested)
        m_bridge.FlushModelStreaming();
    const bool loaded = m_requested && m_bridge.IsModelLoaded(m_roadModel);
    m_active = true;
    m_nextSpawnAttempt = m_nextRecoveryLog = m_bridge.NowSeconds();
    RestorePlayerHeight(origin, false);
    Update(playerGlobal, origin);
    USA_LOG_I("TestPolygon: enabled at (%.1f, %.1f, %.1f), model=%d requested=%d loaded=%d failed=%d tiles=%zu",
              playerGlobal.x, playerGlobal.y, m_config.generationZ, m_roadModel, m_requested, loaded,
              !loaded || m_tiles.empty(), m_tiles.size());
    if (!HasVisuals())
        USA_LOG_W("TestPolygon: markers only (no collision tiles created)");
    return true;
}

void TestPolygon::Clear() {
    for (auto &kv : m_tiles)
        m_bridge.DestroyObject(kv.second.handle);
    m_tiles.clear();
    for (auto &kv : m_markers)
        m_bridge.RemoveMarker(kv.second);
    m_markers.clear();
    if (m_requested)
        m_bridge.ReleaseModel(m_roadModel);
    m_requested = false;
}

void TestPolygon::Disable() {
    if (!m_active && m_tiles.empty() && m_markers.empty())
        return;
    Clear();
    m_active = false;
}

void TestPolygon::Update(const Vec2d &playerGlobal, const Vec2d &origin) {
    if (!m_active)
        return;

    // --- endless slab strip along +Y ---------------------------------------
    const int64_t centre = TileIndexAt(playerGlobal.y);
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

    if (m_requested && m_bridge.IsModelLoaded(m_roadModel) && m_bridge.NowSeconds() >= m_nextSpawnAttempt) {
        int failed = 0;
        for (int64_t i = first; i <= last; ++i) {
            if (m_tiles.count(i))
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
            params.ownerTag = Mix64(static_cast<uint64_t>(i) ^ 0x9E3779B9ULL);
            PolygonTile tile;
            tile.index = i;
            tile.handle = m_bridge.CreateObject(params);
            if (tile.handle)
                m_tiles.emplace(i, tile);
            else
                ++failed;
        }
        if (failed) {
            m_nextSpawnAttempt = m_bridge.NowSeconds() + 2.0;
            USA_LOG_W("TestPolygon: model=%d requested=1 loaded=1 failed=%d tile creation(s)", m_roadModel, failed);
        }
    }

    // --- yellow markers every 200 m, snapped to the GLOBAL grid -------------
    const int64_t markerIndex = static_cast<int64_t>(std::floor(playerGlobal.y / m_config.markerSpacing));
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

    Vec3f player;
    if (m_bridge.GetPlayerPosition(player) && player.z < m_config.generationZ - 5.0f)
        RestorePlayerHeight(origin, true);
}

void TestPolygon::OnOriginShift(const Vec2d &origin) {
    if (m_active)
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
