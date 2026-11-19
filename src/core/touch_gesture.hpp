// Two-finger swipe-down detector for opening the ImGui overlay.
//
// The overlay itself must not be the only way to open the overlay, so the
// gesture is fed from AND_TouchEvent (after translating GTA action codes) and stays
// independent from ImGui's own input handling.
//
// Feed action codes are Android MotionEvent constants: 0=DOWN, 1=UP, 2=MOVE, 3=CANCEL, 5=POINTER_DOWN, 6=POINTER_UP.
// The detector never relies on them too hard: it also triggers purely from
// coordinates, so a firmware that reports moves with action 0 still works.
#pragma once

#include <cstddef>
#include <cstdint>

namespace usa1to1 {
namespace core {

// Single-finger tap detector for a top-left zone. Coordinates are screen
// pixels, y grows downwards. The centered ImGui pill uses ImGui hit testing.
class CornerTapDetector {
  public:
    struct Config {
        float zoneWidthPx = 150.0f;
        float zoneHeightPx = 64.0f;
        float maxMovePx = 24.0f;
        double maxDurationSec = 0.30;
        double cooldownSec = 0.25;
    };

    CornerTapDetector();
    explicit CornerTapDetector(Config config);

    // Returns true exactly once per recognised tap.
    bool Feed(int action, int track, int x, int y, double nowSeconds);
    void Reset();

    size_t TriggerCount() const { return m_triggers; }

  private:
    Config m_config;
    bool m_down = false;
    bool m_multiTouch = false;
    int m_track = -1;
    float m_downX = 0.0f;
    float m_downY = 0.0f;
    double m_downTime = 0.0;
    double m_lastTrigger = -1e9;
    size_t m_triggers = 0;
};

class TwoFingerSwipeDetector {
  public:
    struct Config {
        float minDistancePx = 180.0f;  // how far the fingers must travel down
        float maxHorizontalSlopPx = 160.0f;
        double maxDurationSec = 0.8;
        double cooldownSec = 0.35;
        float screenShortSide = 1080.0f; // used to scale minDistance on small screens
    };

    TwoFingerSwipeDetector();
    explicit TwoFingerSwipeDetector(Config config);

    // Returns true exactly once per recognised swipe.
    bool Feed(int action, int track, int x, int y, double nowSeconds);

    void Reset();
    void SetScreenSize(float width, float height);

    size_t TriggerCount() const { return m_triggers; }
    bool IsTrackingTwoFingers() const { return m_track[0].active && m_track[1].active; }

  private:
    struct Track {
        bool active = false;
        float startX = 0.0f;
        float startY = 0.0f;
        float lastX = 0.0f;
        float lastY = 0.0f;
        double startTime = 0.0;
        double lastTime = 0.0;
    };

    bool Evaluate(double nowSeconds);
    float ScaledMinDistance() const;

    Config m_config;
    Track m_track[2];
    double m_lastTrigger = -1e9;
    size_t m_triggers = 0;
};

} // namespace core
} // namespace usa1to1
