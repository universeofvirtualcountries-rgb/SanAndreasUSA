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
    if (m_config.tileLength < 5.0)
        m_config.tileLength = 20.0;
    if (m_config.markerSpacing < 50.0)
        m_config.markerSpacing = 200.0;
}

int64_t TestPolygon::TileIndexAt(double globalY) const {
    return static_cast<int64_t>(std::floor((globalY - m_startY) / m_config.tileLength));
}

bool TestPolygon::Enable(const Vec2d &playerGlobal, const Vec2d &origin) {
    Disable();
    m_roadX = playerGlobal.x;
    m_startY = playerGlobal.y;
    m_roadModel = m_bridge.ProbeRoadModel();
    if (m_roadModel < 0) {
        const ModelProto *road = m_catalog.PickRoad();
        if (road)
            m_roadModel = road->id;
    }
    if (m_roadModel < 0) {
        USA_LOG_W("TestPolygon: no road model found in the game image - markers only (no slabs)");
    } else {
        m_bridge.RequestModel(m_roadModel, false);
        m_bridge.FlushModelStreaming();
    }

    m_active = true;
    Update(playerGlobal, origin);
    USA_LOG_I("TestPolygon: enabled at global (%.1f, %.1f), model=%d", playerGlobal.x, playerGlobal.y, m_roadModel);
    return true;
}

void TestPolygon::Clear() {
    for (auto &kv : m_tiles)
        m_bridge.DestroyObject(kv.second.handle);
    m_tiles.clear();
    for (auto &kv : m_markers)
        m_bridge.RemoveMarker(kv.second);
    m_markers.clear();
    if (m_roadModel >= 0)
        m_bridge.ReleaseModel(m_roadModel);
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

    if (m_roadModel >= 0 && m_bridge.IsModelLoaded(m_roadModel)) {
        for (int64_t i = first; i <= last; ++i) {
            if (m_tiles.count(i))
                continue;
            const double gy = m_startY + (static_cast<double>(i) + 0.5) * m_config.tileLength;
            const Vec3f local{static_cast<float>(m_roadX - origin.x), static_cast<float>(gy - origin.y),
                              HeightField::HeightAt(m_roadX, gy) + 0.05f};
            SpawnParams params;
            params.modelId = m_roadModel;
            params.pos = local;
            params.headingRad = 0.0f;
            params.dynamic = false;
            params.interior = false;
            params.collision = true;
            params.ownerTag = Mix64(static_cast<uint64_t>(i) ^ 0x9E3779B9ULL);
            PolygonTile tile;
            tile.index = i;
            tile.handle = m_bridge.CreateObject(params);
            if (tile.handle)
                m_tiles.emplace(i, tile);
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
                          HeightField::HeightAt(m_roadX, gy) + 0.3f};
        const uint64_t id = MarkerIdFromDoorSeed(Mix64(static_cast<uint64_t>(k)) ^ 0x9E1CE5ULL);
        m_bridge.PlaceMarker(id, local, 1.2f, false);
        m_markers.emplace(k, id);
    }
}

} // namespace core
} // namespace usa1to1
