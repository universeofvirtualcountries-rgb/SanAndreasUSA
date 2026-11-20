// In-game overlay: telemetry, teleports, the 1:1 test polygon, JSON missions.
// Rendered through AML's RenderWare ImGui backend (same hook family as
// eglSwapBuffers: it draws right before the frame is presented).
#pragma once

#include "core/engine_bridge.hpp"
#include "core/menu_touch.hpp"
#include "core/screen_to_render.hpp"
#include "core/status_feed.hpp"
#include "core/mission.hpp"
#include "core/regions.hpp"
#include "core/world_delta.hpp"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace usa1to1 {
namespace ui {

struct OverlayActions {
    std::function<void(const core::CityAnchor &)> teleportToCity;
    std::function<void()> togglePolygon;
    std::function<void()> spawnCar;
    std::function<void(const std::string &missionId)> startMission;
    std::function<void()> abortMission;
    std::function<void()> syncNow;
    std::function<void()> saveDelta;
    std::function<void()> reloadMissions;
    std::function<void()> toggleStreaming;
};

struct OverlayModel {
    core::Telemetry telemetry;
    std::string regionLabel;   // "California"
    std::string sectorLabel;   // "CA 42x17"
    std::string cityLabel;     // "Los Angeles, CA (12 km)"
    std::string interiorLabel;
    std::string missionLabel;
    std::string cloudLabel;
    std::string saveLabel;
    std::string roadStatus;         // persistent road status feed channel
    std::string statusLine;         // last ShowToast() message, rendered in-menu
    double statusAgeSec = 1e9;      // for the transient banner
    bool polygonActive = false;
    bool insideInterior = false;
    bool streamingEnabled = true;
    int64_t money = 0;
    int missionsCompleted = 0;
    int missionsTotal = 0;
    size_t ownedObjects = 0;
};

class ImGuiOverlay {
  public:
    ImGuiOverlay();
    ~ImGuiOverlay();

    // Sets up ImGui through the RenderWare backend. Safe to call twice.
    bool Initialize();
    void Shutdown();

    void SetActions(OverlayActions actions) { m_actions = std::move(actions); }
    void SetModel(const OverlayModel &model) { m_model = model; }
    void SetMissions(const core::MissionManager *missions, const core::WorldDelta *delta);

    void Toggle();
    core::PixelSize TouchScreenSize() const;
    void CancelPointer() { m_touches.CancelPointer(); }
    bool HandleTouch(int action, int track, int x, int y);
    // Corner button drawn even while the menu is closed.
    void DrawCornerButton();
    void SetVisible(bool visible);
    bool Visible() const { return m_visible; }
    size_t ToggleCount() const { return m_toggleCount; }

    // Called by the backend every frame.
    void Draw();

  private:
    static void RenderCallback();
    static void InputCallback(int screenW, int screenH, int renderW, int renderH);
    void DrawTapStatus();

    OverlayActions m_actions;
    OverlayModel m_model;
    const core::MissionManager *m_missions = nullptr;
    const core::WorldDelta *m_delta = nullptr;
    std::atomic<bool> m_visible{false};
    std::atomic<bool> m_inputReady{false};
    std::atomic<uint32_t> m_touchScreen{0}; // two 16-bit dimensions, from one frame
    core::MenuTouchQueue m_touches;
    core::StatusFeed m_tapStatus; // render-thread diagnostic, independent of game time and gameplay notifications
    std::optional<core::MenuTouch> m_tap;
    core::RenderTouch m_tapMapped{};
    bool m_initialised = false;
    size_t m_toggleCount = 0;
    int m_selectedTab = 0;
};

} // namespace ui
} // namespace usa1to1
