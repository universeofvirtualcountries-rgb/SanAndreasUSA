// Floating origin for a 1:1 re-creation of the USA.
//
// Global coordinates are doubles in metres (millions of them). RenderWare and
// the whole game physics only ever see local coordinates, and the FULL active
// chunk grid is guaranteed to live inside [-1000, +1000] on both axes.
//
// The shift is quantised to whole chunks, which means:
//   * chunk keys (and therefore seeds and content) never change because of a
//     shift - the world does not "jump" its layout,
//   * after a shift the player sits inside the centre chunk, so the grid extent
//     is at most (radius + 1) * chunkSize + one chunk of residual.
#pragma once

#include "engine_bridge.hpp"
#include "types.hpp"

#include <cstdint>

namespace usa1to1 {
namespace core {

class FloatingOrigin {
  public:
    struct Config {
        double chunkSize = 200.0;
        int gridRadius = 2;        // 2 => 5x5 grid
        double localLimit = 1000.0; // hard engine limit for local coords
        double safetyMargin = 50.0; // stay a little further away from the cliff
    };

    struct ShiftResult {
        bool shifted = false;
        Vec3f delta;
        int64_t chunksX = 0;
        int64_t chunksY = 0;
        int entitiesMoved = 0;
        bool invariantHeld = true;
    };

    FloatingOrigin();
    explicit FloatingOrigin(Config config);

    const Config &GetConfig() const { return m_config; }
    const Vec2d &Offset() const { return m_offset; }
    void SetOffset(const Vec2d &offset) { m_offset = offset; }

    Vec3f ToLocal(const Vec2d &global) const;
    Vec2d ToGlobal(const Vec3f &local) const;

    // Largest local coordinate any chunk corner of the active grid can reach.
    double MaxGridExtent() const;

    // |local| above which a shift is triggered. Computed so the far chunk edge
    // can never leave localLimit: limit - gridExtent - margin.
    double TriggerThreshold() const;

    // Room left before the next shift, in metres (>= 0).
    double RemainingBudget(double localCoord) const;

    // Reads the player position, shifts the world if needed. Called every frame
    // from CGame::Process.
    ShiftResult Update(IEngineBridge &bridge);

    // Teleport support: move the player to an arbitrary global position by
    // shifting every entity, so physics keeps its velocities.
    ShiftResult TeleportPlayer(IEngineBridge &bridge, const Vec2d &globalTarget);

    // Pure decision helper (unit tested directly).
    static int64_t ComputeShiftChunks(double localCoord, double chunkSize, double threshold);

    size_t TotalShifts() const { return m_totalShifts; }
    size_t LastShiftEntities() const { return m_lastShiftEntities; }

  private:
    ShiftResult ApplyDelta(IEngineBridge &bridge, const Vec2d &delta);

    Config m_config;
    Vec2d m_offset;
    size_t m_totalShifts = 0;
    size_t m_lastShiftEntities = 0;
};

} // namespace core
} // namespace usa1to1
