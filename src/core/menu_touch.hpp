#pragma once

#include <array>
#include <deque>
#include <mutex>
#include <optional>

namespace usa1to1::core {

// AND_TouchEvent uses GTA's codes, not Android MotionEvent's constants.
enum class TouchAction : int { Up = 1, Down = 2, Move = 3, Cancel = 4 };
struct MenuTouch {
    TouchAction action;
    int x, y;
    bool Down() const { return action == TouchAction::Down || action == TouchAction::Move; }
};

// Menu captures a whole gesture at DOWN. Its primary finger becomes the ImGui
// mouse; additional fingers never leak to gameplay or steal the mouse button.
class MenuTouchQueue {
  public:
    bool Capture(bool visible, int type, int finger, int x, int y) {
        if (type < 1 || type > 4 || finger < 0 || finger >= static_cast<int>(m_captured.size()))
            return false;
        const std::lock_guard<std::mutex> lock(m_mutex);
        const auto action = static_cast<TouchAction>(type);
        if (action == TouchAction::Down && visible && !m_captured[finger]) {
            m_captured[finger] = true;
            if (m_primary < 0)
                m_primary = finger;
        }
        if (!m_captured[finger])
            return false;
        if (finger == m_primary) {
            const MenuTouch event{action, x, y};
            if (action == TouchAction::Move && !m_pending.empty() && m_pending.back().action == TouchAction::Move)
                m_pending.back() = event;
            else {
                // If rendering was suspended, cancel stale clicks instead of
                // replaying an unbounded queue on the first fresh frame.
                if (m_pending.size() >= 128) {
                    m_pending.clear();
                    m_pending.push_back({TouchAction::Cancel, x, y});
                    m_primary = -1;
                } else {
                    m_pending.push_back(event);
                }
            }
        }
        if (action == TouchAction::Up || action == TouchAction::Cancel) {
            if (m_primary == finger)
                m_primary = -1;
            m_captured[finger] = false;
        }
        return true;
    }

    std::optional<MenuTouch> Pop() {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_pending.empty())
            return std::nullopt;
        auto event = m_pending.front();
        m_pending.pop_front();
        return event;
    }

    void Reset() {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_captured.fill(false);
        m_pending.clear();
        m_primary = -1;
    }

  private:
    std::mutex m_mutex;
    std::array<bool, 32> m_captured{};
    std::deque<MenuTouch> m_pending;
    int m_primary = -1;
};

} // namespace usa1to1::core
