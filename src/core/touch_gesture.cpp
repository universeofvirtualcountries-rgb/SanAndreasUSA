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

CornerTapDetector::CornerTapDetector() : CornerTapDetector(Config()) {}

CornerTapDetector::CornerTapDetector(Config config) : m_config(config) {
    if (m_config.zoneWidthPx <= 0.0f)
        m_config.zoneWidthPx = 150.0f;
    if (m_config.zoneHeightPx <= 0.0f)
        m_config.zoneHeightPx = 64.0f;
    if (m_config.maxMovePx < 0.0f)
        m_config.maxMovePx = 0.0f;
    if (m_config.maxDurationSec <= 0.0)
        m_config.maxDurationSec = 0.3;
    if (m_config.cooldownSec < 0.0)
        m_config.cooldownSec = 0.0;
}

void CornerTapDetector::Reset() {
    m_down = false;
    m_multiTouch = false;
    m_track = -1;
}

bool CornerTapDetector::Feed(int action, int track, int x, int y, double nowSeconds) {
    const float fx = static_cast<float>(x);
    const float fy = static_cast<float>(y);
    const bool inZone = fx >= 0.0f && fy >= 0.0f && fx <= m_config.zoneWidthPx && fy <= m_config.zoneHeightPx;

    switch (action) {
    case kActionPointerDown:
    case kActionPointerUp:
        m_multiTouch = true; // second finger: not a corner tap
        return false;
    case kActionDown:
        m_multiTouch = false;
        m_down = inZone;
        m_track = track;
        m_downX = fx;
        m_downY = fy;
        m_downTime = nowSeconds;
        return false;
    case kActionMove: {
        if (!m_down || m_multiTouch || inZone)
            return false;
        const float dx = fx - m_downX;
        const float dy = fy - m_downY;
        if (std::sqrt(dx * dx + dy * dy) > m_config.maxMovePx)
            m_down = false; // dragged: that is a camera/steer gesture, not a tap
        return false;
    }
    case kActionUp: {
        const bool wasDown = m_down && !m_multiTouch && (m_track < 0 || track == m_track);
        m_down = false;
        m_track = -1;
        if (!wasDown || !inZone)
            return false;
        const double duration = nowSeconds - m_downTime;
        if (duration < 0.0 || duration > m_config.maxDurationSec)
            return false;
        if (nowSeconds - m_lastTrigger < m_config.cooldownSec)
            return false;
        m_lastTrigger = nowSeconds;
        ++m_triggers;
        return true;
    }
    case kActionCancel:
        m_down = false;
        m_multiTouch = false;
        m_track = -1;
        return false;
    default:
        return false;
    }
}

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
