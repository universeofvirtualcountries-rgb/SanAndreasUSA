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

namespace usa1to1::core {

RawMenuTouchResult RawMenuGesture::Feed(int action, int finger, int x, int y, double now,
                                       bool visible, int screenW, int screenH) {
    if (finger < 0 || finger >= 32 || action < 1 || action > 4)
        return {};
    const bool pill = screenW > 0 && screenH > 0 && x >= screenW * 0.25 && x <= screenW * 0.75 &&
                      y >= 0 && y <= screenH * 0.12;
    const bool corner = x >= 0 && x <= 150 && y >= 0 && y <= 64;
    auto &f = m_fingers[finger];
    if (action == 2) {
        if (!f.down) {
            if (m_active++ == 0)
                m_multi = m_swiped = false;
            else {
                m_multi = true;
                m_taps = 0;
            }
            f = {true, false, !visible && (pill || corner), x, y, x, y, now};
        }
        return {false, f.hitbox};
    }
    if (!f.down)
        return {};
    f.lastX = x;
    f.lastY = y;
    f.dragged |= std::hypot(double(x) - f.x, double(y) - f.y) > 24.0;
    RawMenuTouchResult result{false, f.hitbox};
    if ((action == 1 || action == 3) && m_active == 2 && !m_swiped) {
        int direction = 0;
        for (const auto &p : m_fingers) {
            if (!p.down)
                continue;
            const double dy = double(p.lastY) - p.y, dx = double(p.lastX) - p.x;
            const int d = dy >= 60.0 ? 1 : dy <= -60.0 ? -1 : 0;
            if (!d || (direction && direction != d) || std::abs(dx) >= std::abs(dy) || now - p.start > 0.8) {
                direction = 0;
                break;
            }
            direction = d;
        }
        if (direction) {
            m_swiped = true;
            result.toggle = true;
        }
    }
    if (action == 1 || action == 4) {
        const bool tap = action == 1 && !m_multi && !f.dragged && now - f.start <= 0.3;
        if (tap && f.hitbox && (pill || corner)) {
            if (!visible)
                result.toggle = true;
            m_taps = 0; // never count the same opening tap toward another toggle
        } else if (tap) {
            if (!m_taps || now - m_firstTap > 0.600 + 1e-9) {
                m_taps = 0;
                m_firstTap = f.start;
            }
            if (++m_taps == 3) {
                result.toggle = now - m_firstTap <= 0.600 + 1e-9;
                m_taps = 0;
            }
        } else {
            m_taps = 0;
        }
        f = {};
        --m_active;
    }
    return result;
}

} // namespace usa1to1::core
