#pragma once

#include "core/menu_touch.hpp"
#include "core/screen_to_render.hpp"
#include <mod/thirdparty/imgui/imgui.h>
#include <cfloat>

namespace usa1to1::ui {

inline void FeedImGuiTouch(ImGuiIO &io, const core::MenuTouch &event, const core::RenderTouch &point) {
    // The native queue emits one event per frame already. ImGui's second
    // trickle would split a touch's position and button update across frames,
    // especially on DOWN and on a MOVE followed by UP.
    io.ConfigInputTrickleEventQueue = false;
    io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
    // Cancel must not click a widget at the last known coordinates.
    if (event.action == core::TouchAction::Cancel)
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    else
        io.AddMousePosEvent(point.x, point.y);
    io.AddMouseButtonEvent(0, event.Down());
}

} // namespace usa1to1::ui
