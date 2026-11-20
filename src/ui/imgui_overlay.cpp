#include "imgui_overlay.hpp"
#include "imgui_touch.hpp"

#include "core/log.hpp"

#include <mod/thirdparty/imgui/imgui.h>
#include <plugin-sdk/renderware/imgui/imgui_impl_renderware.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <string>

namespace usa1to1 {
namespace ui {
namespace {
ImGuiOverlay *g_overlay = nullptr;

void TextDim(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    ImGui::TextDisabled("%s", buf);
}
} // namespace

ImGuiOverlay::ImGuiOverlay() = default;

ImGuiOverlay::~ImGuiOverlay() { Shutdown(); }

bool ImGuiOverlay::Initialize() {
    if (m_initialised)
        return true;
    g_overlay = this;
    if (!ImGui_ImplRenderWare_Init(&ImGuiOverlay::RenderCallback, &ImGuiOverlay::InputCallback)) {
        USA_LOG_E("Overlay: ImGui RenderWare backend failed to initialise");
        g_overlay = nullptr;
        return false;
    }
    m_initialised = true;
    m_inputReady.store(true);
    USA_LOG_I("Overlay: ImGui ready (two-finger swipe down to open)");
    return true;
}

void ImGuiOverlay::Shutdown() {
    if (!m_initialised)
        return;
    m_inputReady.store(false);
    SetVisible(false);
    m_touches.Reset();
    ImGui_ImplRenderWare_Shutdown();
    m_initialised = false;
    if (g_overlay == this)
        g_overlay = nullptr;
}

void ImGuiOverlay::Toggle() {
    SetVisible(!Visible());
    ++m_toggleCount;
}

void ImGuiOverlay::SetMissions(const core::MissionManager *missions, const core::WorldDelta *delta) {
    m_missions = missions;
    m_delta = delta;
}

void ImGuiOverlay::SetVisible(bool visible) {
    if (m_visible.exchange(visible) != visible)
        CancelPointer();
    ImGui_ImplRenderWare_SetDirectTouchCapture(visible);
}

core::PixelSize ImGuiOverlay::TouchScreenSize() const {
    const uint32_t packed = m_touchScreen.load();
    return {static_cast<int>(packed >> 16), static_cast<int>(packed & 0xFFFF)};
}

bool ImGuiOverlay::HandleTouch(int action, int track, int x, int y) {
    if (!m_inputReady.load())
        return false;
    if (m_touches.Capture(Visible(), action, track, x, y))
        return true;
    return ImGui_ImplRenderWare_RouteTouch(action, track, x, y);
}

void ImGuiOverlay::InputCallback(int screenW, int screenH, int renderW, int renderH) {
    if (!g_overlay)
        return;
    if (screenW > 0 && screenH > 0 && screenW <= 65535 && screenH <= 65535)
        g_overlay->m_touchScreen.store((uint32_t(screenW) << 16) | uint32_t(screenH));
    // Process DOWN and UP in separate frames, even for a tap between renders.
    // This is the modern ImGui equivalent of setting io.MouseDown[0].
    const auto event = g_overlay->m_touches.Pop();
    if (!event)
        return;
    const auto pos = core::ScreenToRender(event->x, event->y, screenW, screenH, renderW, renderH);
    FeedImGuiTouch(ImGui::GetIO(), *event, pos);
    if (event->action == core::TouchAction::Down) {
        g_overlay->m_tap = event;
        g_overlay->m_tapMapped = pos;
    }
}

void ImGuiOverlay::DrawTapStatus() {
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (m_tap) {
        char line[192];
        std::snprintf(line, sizeof(line), "Tap raw=(%d,%d) -> imgui=(%.1f,%.1f) hovered=%s",
                      m_tap->x, m_tap->y, m_tapMapped.x, m_tapMapped.y,
                      ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)
                          ? "yes" : "no");
        m_tapStatus.Push(line, now);
        USA_LOG_I("%s", line);
        m_tap.reset();
    }
    std::string line;
    if (m_tapStatus.Fresh(line, now)) {
        const float x = (ImGui::GetIO().DisplaySize.x - ImGui::CalcTextSize(line.c_str()).x) * 0.5f;
        auto *draw = ImGui::GetForegroundDrawList();
        draw->AddText(ImVec2(x + 1.0f, 103.0f), IM_COL32(0, 0, 0, 220), line.c_str());
        draw->AddText(ImVec2(x, 102.0f), IM_COL32(255, 236, 160, 255), line.c_str());
    }
}

void ImGuiOverlay::RenderCallback() {
    if (g_overlay) {
        g_overlay->Draw();
        g_overlay->DrawTapStatus();
    }
}

// Small, always visible pill at the top center of the render surface.
void ImGuiOverlay::DrawCornerButton() {
    const ImVec4 bg(0.03f, 0.04f, 0.07f, 1.0f);
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f - 80.0f, 16.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(160.0f, 48.0f), ImGuiCond_Always);

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                   ImGuiWindowFlags_NoCollapse | (Visible() ? 0 : ImGuiWindowFlags_NoInputs);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, bg);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.08f, 0.28f, 0.48f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.12f, 0.39f, 0.61f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.13f, 0.38f, 0.53f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(3.0f, 3.0f));

    if (ImGui::Begin("##usa1to1_corner", nullptr, flags)) {
        if (ImGui::Button("[USA 1:1]", ImVec2(154.0f, 42.0f)))
            Toggle();
    }
    ImGui::End();

    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
}

void ImGuiOverlay::Draw() {
    // Drawn before the visibility check: the corner button is the way *in*.
    DrawCornerButton();
    if (m_model.polygonActive && !m_model.roadStatus.empty()) {
        const char *text = m_model.roadStatus.c_str();
        const float width = ImGui::GetIO().DisplaySize.x;
        const float wrap = std::max(1.0f, width - 20.0f);
        const ImVec2 size = ImGui::CalcTextSize(text, nullptr, false, wrap);
        const float x = (width - size.x) * 0.5f;
        auto *draw = ImGui::GetForegroundDrawList();
        draw->AddRectFilled(ImVec2(x - 5.0f, 66.0f), ImVec2(x + size.x + 5.0f, 70.0f + size.y),
                            IM_COL32(8, 12, 18, 225), 4.0f);
        draw->AddText(nullptr, 0.0f, ImVec2(x, 68.0f), IM_COL32(255, 245, 196, 255), text, nullptr, wrap);
    }

    // Status line from ShowToast() (no AML toasts - they abort on ART): a short
    // centred banner that does not depend on the menu being open.
    if (!m_model.statusLine.empty() && m_model.statusAgeSec < 3.0) {
        const ImGuiIO &io = ImGui::GetIO();
        const float w = io.DisplaySize.x > 0.0f ? io.DisplaySize.x : 1280.0f;
        const char *text = m_model.statusLine.c_str();
        const float textWidth = ImGui::CalcTextSize(text).x;
        const float alpha = m_model.statusAgeSec > 2.0 ? static_cast<float>(3.0 - m_model.statusAgeSec) : 1.0f;
        ImGui::GetForegroundDrawList()->AddText(ImVec2((w - textWidth) * 0.5f, 92.0f),
                                                IM_COL32(0, 0, 0, static_cast<int>(160 * alpha)), text);
        ImGui::GetForegroundDrawList()->AddText(ImVec2((w - textWidth) * 0.5f - 1.0f, 91.0f),
                                                IM_COL32(255, 236, 160, static_cast<int>(230 * alpha)), text);
    }

    if (!m_visible)
        return;

    const ImGuiIO &io = ImGui::GetIO();
    const float width = io.DisplaySize.x > 0.0f ? io.DisplaySize.x : 1280.0f;
    const float height = io.DisplaySize.y > 0.0f ? io.DisplaySize.y : 720.0f;
    // Touch UI: larger font and roomier widgets on a phone screen.
    ImGui::SetNextWindowSize(ImVec2(width * 0.72f, height * 0.78f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(width * 0.14f, height * 0.10f), ImGuiCond_FirstUseEver);

    ImGui::Begin("USA 1:1 Procedural Expansion", nullptr, ImGuiWindowFlags_NoCollapse);

    const auto &t = m_model.telemetry;
    if (ImGui::BeginTabBar("##tabs")) {
        if (ImGui::BeginTabItem("Telemetry")) {
            m_selectedTab = 0;
            ImGui::Text("Global: %.1f, %.1f", t.globalX, t.globalY);
            ImGui::Text("Local:  %.1f, %.1f, Z %.1f (XY limit 1000)", t.localPos.x, t.localPos.y, t.localZ);
            ImGui::Text("Origin offset: %.1f, %.1f", t.offsetX, t.offsetY);
            ImGui::Separator();
            ImGui::Text("Region: %s", m_model.regionLabel.c_str());
            ImGui::Text("Sector: %s", m_model.sectorLabel.c_str());
            ImGui::Text("Nearest: %s", m_model.cityLabel.c_str());
            ImGui::Separator();
            ImGui::Text("Chunk: %lld, %lld", static_cast<long long>(t.chunkX), static_cast<long long>(t.chunkY));
            ImGui::Text("Chunk seed: %016llX", static_cast<unsigned long long>(t.chunkSeed));
            ImGui::Text("Active chunks: %d", t.activeChunks);
            char touchLine[256];
            ImGui_ImplRenderWare_FormatTouch(ImGui_ImplRenderWare_LastTouch(), touchLine, sizeof(touchLine));
            ImGui::TextWrapped("%s", touchLine);
            ImGui::Text("Mod objects: %d (owned %zu)", t.activeObjects, m_model.ownedObjects);
            ImGui::Text("Streamed entities: %d", t.streamedEntities);
            ImGui::Text("FPS: %.1f", t.fps);
            ImGui::Text("Origin shifts: %zu this frame / %zu total", t.frameShifts, t.totalShifts);
            ImGui::Text("Shift headroom: %.0f m", t.localBudget);
            if (m_model.insideInterior)
                TextDim("Interior: %s", m_model.interiorLabel.c_str());
            ImGui::Separator();
            ImGui::Text("Money: $%lld", static_cast<long long>(m_model.money));
            ImGui::Text("Missions: %d / %d complete", m_model.missionsCompleted, m_model.missionsTotal);
            if (!m_model.missionLabel.empty())
                ImGui::TextWrapped("Active: %s", m_model.missionLabel.c_str());
            if (!m_model.saveLabel.empty())
                TextDim("%s", m_model.saveLabel.c_str());
            if (!m_model.roadStatus.empty())
                ImGui::TextWrapped("%s", m_model.roadStatus.c_str());
            if (!m_model.statusLine.empty())
                TextDim("status: %s", m_model.statusLine.c_str());
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Teleports")) {
            m_selectedTab = 1;
            if (ImGui::Button("Spawn Car") && m_actions.spawnCar)
                m_actions.spawnCar();
            ImGui::TextWrapped("1:1 plane teleport keeps the floating origin intact: the world shifts "
                               "under the player, physics keeps its velocities.");
            for (const auto &city : core::GetCityAnchors()) {
                char label[96];
                std::snprintf(label, sizeof(label), "%s (%s)##tp_%s", city.name, city.region, city.name);
                if (ImGui::Button(label)) {
                    if (m_actions.teleportToCity)
                        m_actions.teleportToCity(city);
                }
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Polygon")) {
            m_selectedTab = 2;
            if (ImGui::Button("Spawn Car") && m_actions.spawnCar)
                m_actions.spawnCar();
            ImGui::TextWrapped("Infinite straight road with a marker every 200 m in global space. "
                               "Drive it and watch the shift counter - that is the 1:1 stress test.");
            if (ImGui::Button(m_model.polygonActive ? "Stop polygon" : "Start polygon")) {
                if (m_actions.togglePolygon)
                    m_actions.togglePolygon();
            }
            ImGui::SameLine();
            if (ImGui::Button(m_model.streamingEnabled ? "Streaming: ON" : "Streaming: OFF")) {
                if (m_actions.toggleStreaming)
                    m_actions.toggleStreaming();
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Missions")) {
            m_selectedTab = 3;
            if (!m_missions || m_missions->Size() == 0) {
                ImGui::TextDisabled("No missions found in %s", "assets/missions");
            } else {
                for (const auto &mission : m_missions->All()) {
                    ImGui::PushID(mission.id.c_str());
                    ImGui::Text("%s", mission.title.empty() ? mission.id.c_str() : mission.title.c_str());
                    if (!mission.brief.empty())
                        ImGui::TextWrapped("%s", mission.brief.c_str());
                    int step = 0;
                    std::string status = "available";
                    if (m_delta) {
                        if (const core::MissionState *st = m_delta->FindMission(mission.id)) {
                            status = st->status.empty() ? "available" : st->status;
                            step = st->step;
                        }
                    }
                    ImGui::TextDisabled("status: %s  step: %d/%zu  reward: $%lld", status.c_str(), step,
                                        mission.steps.size(), static_cast<long long>(mission.reward));
                    const bool completed = status == "completed";
                    if (completed)
                        ImGui::BeginDisabled();
                    if (ImGui::Button("Start")) {
                        if (m_actions.startMission)
                            m_actions.startMission(mission.id);
                    }
                    if (completed)
                        ImGui::EndDisabled();
                    ImGui::Separator();
                    ImGui::PopID();
                }
            }
            if (ImGui::Button("Abort active mission") && m_actions.abortMission)
                m_actions.abortMission();
            ImGui::SameLine();
            if (ImGui::Button("Reload JSON") && m_actions.reloadMissions)
                m_actions.reloadMissions();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Cloud")) {
            m_selectedTab = 4;
            ImGui::TextWrapped("Missions and manifest are pulled straight from the GitHub repo through "
                               "java.net.HttpURLConnection (JNI).");
            ImGui::TextWrapped("%s", m_model.cloudLabel.empty() ? "idle" : m_model.cloudLabel.c_str());
            if (ImGui::Button("Sync now") && m_actions.syncNow)
                m_actions.syncNow();
            ImGui::SameLine();
            if (ImGui::Button("Save world delta") && m_actions.saveDelta)
                m_actions.saveDelta();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::Separator();
    if (ImGui::Button("Close (two-finger swipe again)") || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        SetVisible(false);

    ImGui::End();
}

} // namespace ui
} // namespace usa1to1
