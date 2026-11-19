#pragma once

namespace usa1to1::core {

struct PixelSize {
    int width = 0, height = 0;
    constexpr bool Valid() const { return width > 0 && height > 0; }
};

// Prefer the touch space specified by the user. The window surface is a better
// proxy than the current viewport, which may belong to a smaller render target.
inline constexpr PixelSize ResolveTouchScreenSize(PixelSize overrideSize, PixelSize surface,
                                                   PixelSize viewport, PixelSize render) {
    return overrideSize.Valid() ? overrideSize : surface.Valid() ? surface : viewport.Valid() ? viewport : render;
}

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
