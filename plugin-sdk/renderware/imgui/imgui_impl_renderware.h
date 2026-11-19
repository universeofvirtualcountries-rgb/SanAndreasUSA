#pragma once

#include "imgui_shared.h"

#include <cstddef>
#include <mod/thirdparty/imgui/imgui.h>

// Called every rendered frame between ImGui::NewFrame() and ImGui::Render().
// The callback owns all UI policy: which windows exist, and when they are shown.
typedef void (*ImGui_RenderCallback)();
// Runs on the render thread before NewFrame, with current screen and render extents.
using ImGui_TouchCallback = void (*)(int screenW, int screenH, int renderW, int renderH);

// Initializes the shared rendering hooks. The adapter owns AND_TouchEvent and
// calls RouteTouch for ordinary hit-testing when the direct menu path did not consume it.
bool ImGui_ImplRenderWare_Init(ImGui_RenderCallback renderCallback, ImGui_TouchCallback inputCallback);
bool ImGui_ImplRenderWare_RouteTouch(int action, int track, int x, int y);
void ImGui_ImplRenderWare_SetDirectTouchCapture(bool capture);

// Destroys the ImGui context and its textures. Hooks stay installed and pass input through to the game.
void ImGui_ImplRenderWare_Shutdown();

// Low-level renderer APIs (for developers who manage their own ImGui context and frame loop)
bool ImGui_ImplRenderWare_InitRenderer();
bool ImGui_ImplRenderWare_RenderDrawData(ImDrawData *drawData);

// Optional config override set before installing hooks. Zero selects render-time detection.
void ImGui_ImplRenderWare_SetScreenSizeOverride(int width, int height);
ImGuiShared::TouchSample ImGui_ImplRenderWare_LastTouch();
void ImGui_ImplRenderWare_FormatTouch(const ImGuiShared::TouchSample &sample, char *out, size_t size);
