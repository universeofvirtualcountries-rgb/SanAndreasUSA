#include "imgui_overlay.hpp"

#include "core/log.hpp"

#include <mod/thirdparty/imgui/imgui.h>
#include <plugin-sdk/renderware/imgui/imgui_impl_renderware.h>

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
    if (!ImGui_ImplRenderWare_Init(&ImGuiOverlay::RenderCallback)) {
        USA_LOG_E("Overlay: ImGui RenderWare backend failed to initialise");
        g_overlay = nullptr;
        return false;
    }
    m_initialised = true;
    USA_LOG_I("Overlay: ImGui ready (two-finger swipe down to open)");
    return true;
}

void ImGuiOverlay::Shutdown() {
    if (!m_initialised)
        return;
    ImGui_ImplRenderWare_Shutdown();
    m_initialised = false;
    if (g_overlay == this)
        g_overlay = nullptr;
}

void ImGuiOverlay::Toggle() {
    m_visible = !m_visible;
    ++m_toggleCount;
}

void ImGuiOverlay::SetMissions(const core::MissionManager *missions, const core::WorldDelta *delta) {
    m_missions = missions;
    m_delta = delta;
}

void ImGuiOverlay::RenderCallback() {
    if (g_overlay)
        g_overlay->Draw();
}

void ImGuiOverlay::Draw() {
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
            ImGui::Text("Local:  %.1f, %.1f (limit 1000)", t.localPos.x, t.localPos.y);
            ImGui::Text("Origin offset: %.1f, %.1f", t.offsetX, t.offsetY);
            ImGui::Separator();
            ImGui::Text("Region: %s", m_model.regionLabel.c_str());
            ImGui::Text("Sector: %s", m_model.sectorLabel.c_str());
            ImGui::Text("Nearest: %s", m_model.cityLabel.c_str());
            ImGui::Separator();
            ImGui::Text("Chunk: %lld, %lld", static_cast<long long>(t.chunkX), static_cast<long long>(t.chunkY));
            ImGui::Text("Chunk seed: %016llX", static_cast<unsigned long long>(t.chunkSeed));
            ImGui::Text("Active chunks: %d", t.activeChunks);
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
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Teleports")) {
            m_selectedTab = 1;
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
        m_visible = false;

    ImGui::End();
}

} // namespace ui
} // namespace usa1to1
