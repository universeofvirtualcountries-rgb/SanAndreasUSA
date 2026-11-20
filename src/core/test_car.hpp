#pragma once

#include "engine_bridge.hpp"
#include <optional>

namespace usa1to1::core {

// One outstanding request, owned by the game tick. Never construct in the
// button callback or the same tick that synchronously services the request.
class TestCarRequest {
  public:
    static constexpr int kModel = 560; // Sultan; no arbitrary native model IDs
    enum class Result { Idle, Waiting, Requested, Spawned, Failed, TimedOut };

    void Enqueue() { m_pending = true; }
    bool Pending() const { return m_pending; }

    Result Tick(IEngineBridge &bridge, const std::optional<Vec3f> &position) {
        if (!m_pending)
            return Result::Idle;
        const double now = bridge.NowSeconds();
        if (!m_deadline)
            m_deadline = now + 8.0;
        if (now >= *m_deadline) {
            Cancel(bridge);
            return Result::TimedOut;
        }
        if (!position)
            return Result::Waiting;
        if (!m_requested) {
            if (!bridge.RequestTestCarModel(kModel)) {
                Cancel(bridge);
                return Result::Failed;
            }
            m_requested = true;
            if (!bridge.IsModelLoaded(kModel))
                bridge.FlushModelStreaming();
            return Result::Requested;
        }
        if (!bridge.IsModelLoaded(kModel))
            return Result::Waiting;
        const bool spawned = bridge.SpawnTestCar(kModel, *position);
        Cancel(bridge);
        return spawned ? Result::Spawned : Result::Failed;
    }

    void Cancel(IEngineBridge &bridge) {
        if (m_requested)
            bridge.ReleaseModel(kModel);
        m_pending = m_requested = false;
        m_deadline.reset();
    }

  private:
    bool m_pending = false;
    bool m_requested = false;
    std::optional<double> m_deadline;
};

} // namespace usa1to1::core
