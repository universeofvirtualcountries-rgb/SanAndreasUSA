// "Тестовый полигон 1:1": an endless straight road under the wheels plus a
// yellow marker every 200 m in GLOBAL coordinates. Walk/drive 10 km and you get
// ~50 floating-origin shifts; the markers prove the shifts stay consistent with
// the global grid instead of drifting.
#pragma once

#include "engine_bridge.hpp"
#include "heightfield.hpp"
#include "model_catalog.hpp"
#include "types.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace usa1to1 {
namespace core {

struct PolygonTile {
    ObjectHandle handle;
    int64_t index = 0; // tile index along the road
};

class TestPolygon {
  public:
    struct Config {
        double tileLength = 20.0;
        double laneWidth = 14.0;
        int aheadTiles = 10;
        int behindTiles = 2;
        double markerSpacing = 200.0;
        float generationZ = HeightField::kDefaultGenerationZ;
        int markerAhead = 2; // markers around the player, in each direction
    };

    TestPolygon(IEngineBridge &bridge, const ModelCatalog &catalog);
    TestPolygon(IEngineBridge &bridge, const ModelCatalog &catalog, Config config);

    // Starts the polygon at the player's current global position, heading north.
    bool Enable(const Vec2d &playerGlobal, const Vec2d &origin);
    void Disable();
    bool Active() const { return m_active; }

    // Called every frame while active. Recycles tiles behind the player and
    // spawns new ones ahead; creates/removes the 200 m markers.
    void Update(const Vec2d &playerGlobal, const Vec2d &origin);

    void OnOriginShift(const Vec2d &origin);

    size_t TileCount() const { return m_tiles.size(); }
    size_t MarkerCount() const { return m_markers.size(); }
    bool WaitingForSupport() const { return m_holdAt.has_value(); }
    bool HasVisuals() const { return !m_tiles.empty(); }
    int RoadModel() const { return m_roadModel; }
    int64_t TileIndexAt(double globalY) const;

  private:
    void Clear();
    void HoldPlayer(const Vec2d &origin);
    void RestorePlayerHeight(const Vec2d &origin, bool recenter);

    IEngineBridge &m_bridge;
    ModelCatalog m_catalog;
    Config m_config;
    bool m_active = false;
    int m_roadModel = -1;
    bool m_requested = false;
    float m_tileHeading = 0.0f;
    double m_tileLength = 20.0;
    float m_tileHalfWidth = 0.0f;
    std::optional<Vec2d> m_holdAt;
    Vec3f m_tileOffset;
    double m_nextSpawnAttempt = 0.0;
    double m_nextRecoveryLog = 0.0;
    double m_roadX = 0.0;      // global X of the road centreline
    double m_startY = 0.0;     // global Y where the polygon started
    std::map<int64_t, PolygonTile> m_tiles;
    std::map<int64_t, uint64_t> m_markers; // marker index -> marker id
};

} // namespace core
} // namespace usa1to1
