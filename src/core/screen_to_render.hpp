#pragma once

namespace usa1to1::core {

struct RenderTouch {
    float x, y;
    float scaleX, scaleY;
    bool validDimensions;
};

// Preserve raw coordinates if either coordinate space is not known yet.
// Do not clamp: a captured finger can move outside the window before release.
inline constexpr RenderTouch ScreenToRender(float x, float y, int screenW, int screenH, int renderW, int renderH) {
    if (screenW <= 0 || screenH <= 0 || renderW <= 0 || renderH <= 0)
        return {x, y, 1.0f, 1.0f, false};
    const float sx = static_cast<float>(renderW) / screenW;
    const float sy = static_cast<float>(renderH) / screenH;
    return {x * sx, y * sy, sx, sy, true};
}

} // namespace usa1to1::core
