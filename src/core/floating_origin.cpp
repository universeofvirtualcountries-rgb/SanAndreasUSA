#include "floating_origin.hpp"

#include "log.hpp"

#include <algorithm>
#include <cmath>

namespace usa1to1 {
namespace core {

FloatingOrigin::FloatingOrigin() : FloatingOrigin(Config()) {}

FloatingOrigin::FloatingOrigin(Config config) : m_config(config) {
    if (m_config.chunkSize <= 1.0)
        m_config.chunkSize = 200.0;
    if (m_config.gridRadius < 0)
        m_config.gridRadius = 0;
    if (m_config.gridRadius > 3)
        m_config.gridRadius = 3;
    if (m_config.localLimit <= m_config.chunkSize)
        m_config.localLimit = 1000.0;
    if (m_config.safetyMargin < 0.0)
        m_config.safetyMargin = 0.0;
}

Vec3f FloatingOrigin::ToLocal(const Vec2d &global) const {
    return {SafeLocal(global.x - m_offset.x), SafeLocal(global.y - m_offset.y), 0.0f};
}

Vec2d FloatingOrigin::ToGlobal(const Vec3f &local) const {
    return {m_offset.x + static_cast<double>(local.x), m_offset.y + static_cast<double>(local.y)};
}

double FloatingOrigin::MaxGridExtent() const {
    return (static_cast<double>(m_config.gridRadius) + 1.0) * m_config.chunkSize;
}

double FloatingOrigin::TriggerThreshold() const {
    const double t = m_config.localLimit - MaxGridExtent() - m_config.safetyMargin;
    return t > 0.0 ? t : m_config.chunkSize * 0.5;
}

double FloatingOrigin::RemainingBudget(double localCoord) const {
    const double r = TriggerThreshold() - std::fabs(localCoord);
    return r > 0.0 ? r : 0.0;
}

int64_t FloatingOrigin::ComputeShiftChunks(double localCoord, double chunkSize, double threshold) {
    if (chunkSize <= 0.0)
        return 0;
    if (std::fabs(localCoord) <= threshold)
        return 0;
    // Truncate toward zero: brings |local| below one chunk size, never overshoots.
    const double chunks = std::trunc(localCoord / chunkSize);
    return static_cast<int64_t>(chunks);
}

FloatingOrigin::ShiftResult FloatingOrigin::Update(IEngineBridge &bridge) {
    ShiftResult result;
    Vec3f localPlayer;
    if (!bridge.GetPlayerPosition(localPlayer))
        return result;

    // Interior pockets sit at z >= 1000 next to the world origin; the main world
    // must not be shifted while the player is inside one.
    if (IsInteriorZ(localPlayer.z))
        return result;

    const double threshold = TriggerThreshold();
    const int64_t cx = ComputeShiftChunks(static_cast<double>(localPlayer.x), m_config.chunkSize, threshold);
    const int64_t cy = ComputeShiftChunks(static_cast<double>(localPlayer.y), m_config.chunkSize, threshold);
    if (cx == 0 && cy == 0)
        return result;

    const Vec2d delta{-static_cast<double>(cx) * m_config.chunkSize,
                      -static_cast<double>(cy) * m_config.chunkSize};
    result = ApplyDelta(bridge, delta);
    result.chunksX = cx;
    result.chunksY = cy;
    return result;
}

FloatingOrigin::ShiftResult FloatingOrigin::TeleportPlayer(IEngineBridge &bridge, const Vec2d &globalTarget) {
    ShiftResult result;
    Vec3f localPlayer;
    if (!bridge.GetPlayerPosition(localPlayer))
        return result;

    // Cheapest and most stable form of a teleport: keep the player exactly where
    // they are in local space and move the origin under them. No entity gets
    // touched, so velocities, camera and collision state survive untouched.
    // The streamer notices the new global position and rebuilds the grid.
    m_offset = globalTarget - Vec2d(static_cast<double>(localPlayer.x), static_cast<double>(localPlayer.y));
    result.shifted = true;
    result.invariantHeld = true;
    ++m_totalShifts;
    m_lastShiftEntities = 0;
    USA_LOG_I("FloatingOrigin: teleport, offset=(%.1f, %.1f) local=(%.1f, %.1f)", m_offset.x, m_offset.y,
              localPlayer.x, localPlayer.y);
    return result;
}

FloatingOrigin::ShiftResult FloatingOrigin::ApplyDelta(IEngineBridge &bridge, const Vec2d &delta) {
    ShiftResult result;
    result.delta = {static_cast<float>(delta.x), static_cast<float>(delta.y), 0.0f};
    if (result.delta.x == 0.0f && result.delta.y == 0.0f)
        return result;

    // Order matters: the player first (physics reads it this frame), then the
    // vehicle the player may be sitting in, then the camera, then the world.
    int moved = 0;
    moved += std::max(0, bridge.ShiftPlayer(result.delta));
    moved += std::max(0, bridge.ShiftPlayerVehicle(result.delta));
    moved += std::max(0, bridge.ShiftCamera(result.delta));
    moved += std::max(0, bridge.ShiftStreamedEntities(result.delta, /*includeInterior=*/false));
    moved += std::max(0, bridge.ShiftWorldPeds(result.delta));
    moved += std::max(0, bridge.ShiftWorldVehicles(result.delta));

    m_offset.x -= delta.x;
    m_offset.y -= delta.y;
    ++m_totalShifts;
    m_lastShiftEntities = static_cast<size_t>(moved);

    result.shifted = true;
    result.entitiesMoved = moved;
    result.invariantHeld = true;

    Vec3f after;
    if (bridge.GetPlayerPosition(after)) {
        const double limit = m_config.localLimit - MaxGridExtent() - 1.0;
        if (std::fabs(after.x) > limit || std::fabs(after.y) > limit) {
            result.invariantHeld = false;
            USA_LOG_E("FloatingOrigin invariant BROKEN: local=(%.1f, %.1f) limit=%.1f", after.x, after.y, limit);
        }
    }

    USA_LOG_D("FloatingOrigin: shift (%.1f, %.1f) entities=%d offset=(%.1f, %.1f)", result.delta.x,
              result.delta.y, moved, m_offset.x, m_offset.y);
    return result;
}

} // namespace core
} // namespace usa1to1
