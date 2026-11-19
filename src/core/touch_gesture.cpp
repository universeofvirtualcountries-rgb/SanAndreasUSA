#include "touch_gesture.hpp"

#include <algorithm>
#include <cmath>

namespace usa1to1 {
namespace core {
namespace {
constexpr int kActionDown = 0;
constexpr int kActionUp = 1;
constexpr int kActionMove = 2;
constexpr int kActionCancel = 3;
constexpr int kActionPointerDown = 5;
constexpr int kActionPointerUp = 6;
} // namespace

TwoFingerSwipeDetector::TwoFingerSwipeDetector() : TwoFingerSwipeDetector(Config()) {}

TwoFingerSwipeDetector::TwoFingerSwipeDetector(Config config) : m_config(config) {
    if (m_config.minDistancePx <= 0.0f)
        m_config.minDistancePx = 180.0f;
    if (m_config.maxDurationSec <= 0.0)
        m_config.maxDurationSec = 0.8;
    if (m_config.cooldownSec < 0.0)
        m_config.cooldownSec = 0.0;
}

void TwoFingerSwipeDetector::Reset() {
    for (auto &t : m_track)
        t = Track{};
}

void TwoFingerSwipeDetector::SetScreenSize(float width, float height) {
    m_config.screenShortSide = width < height ? width : height;
}

float TwoFingerSwipeDetector::ScaledMinDistance() const {
    // 1080p-class devices use the configured value; smaller screens scale down
    // proportionally, never below 90 px (fingers are fingers).
    const float scale = m_config.screenShortSide > 0.0f ? m_config.screenShortSide / 1080.0f : 1.0f;
    const float d = m_config.minDistancePx * scale;
    return d < 90.0f ? 90.0f : d;
}

bool TwoFingerSwipeDetector::Feed(int action, int track, int x, int y, double nowSeconds) {
    if (track < 0 || track > 1)
        return false; // three+ fingers: not our gesture
    Track &t = m_track[track];
    const float fx = static_cast<float>(x);
    const float fy = static_cast<float>(y);

    const bool isDown = action == kActionDown || action == kActionPointerDown;
    const bool isUp = action == kActionUp || action == kActionPointerUp;
    const bool isCancel = action == kActionCancel;
    const bool isMove = action == kActionMove;

    if (isDown) {
        // A fresh touch restarts that track.
        t.active = true;
        t.startX = fx;
        t.startY = fy;
        t.lastX = fx;
        t.lastY = fy;
        t.startTime = nowSeconds;
        t.lastTime = nowSeconds;
    } else if (isUp || isCancel) {
        if (isCancel) {
            t = Track{};
            return false;
        }
        // Update the final position, evaluate, then release the track.
        t.lastX = fx;
        t.lastY = fy;
        t.lastTime = nowSeconds;
        const bool hit = Evaluate(nowSeconds);
        t = Track{};
        return hit;
    } else if (isMove || !t.active) {
        // Some builds report the very first contact as a move.
        if (!t.active) {
            t.active = true;
            t.startX = fx;
            t.startY = fy;
            t.startTime = nowSeconds;
        }
        t.lastX = fx;
        t.lastY = fy;
        t.lastTime = nowSeconds;
    }

    return Evaluate(nowSeconds);
}

bool TwoFingerSwipeDetector::Evaluate(double nowSeconds) {
    if (!m_track[0].active || !m_track[1].active)
        return false;
    if (nowSeconds - m_lastTrigger < m_config.cooldownSec)
        return false;

    const float needed = ScaledMinDistance();
    const double duration = nowSeconds - std::min(m_track[0].startTime, m_track[1].startTime);
    if (duration > m_config.maxDurationSec)
        return false;

    for (const auto &t : m_track) {
        const float dy = t.lastY - t.startY;
        const float dx = t.lastX - t.startX;
        if (dy < needed)
            return false;
        if (std::fabs(dx) > m_config.maxHorizontalSlopPx)
            return false;
    }

    m_lastTrigger = nowSeconds;
    ++m_triggers;
    return true;
}

} // namespace core
} // namespace usa1to1
