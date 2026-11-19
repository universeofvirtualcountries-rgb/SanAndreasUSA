#include "imgui_impl_renderware.h"
#include "imgui_shared.h"
#include "core/screen_to_render.hpp"

#include <mod/amlmod.h>
#include <mod/interface.h>
#include <mod/logger.h>
#include <plugin-sdk/game_sa/Events.h>
#include <plugin-sdk/game_sa/base/Timer.h>
#include <plugin-sdk/game_sa/engine/RsGlobal.h>
#include <plugin-sdk/game_sa/plugin.h>
#include <plugin-sdk/gta_base/Rect.h>
#include <plugin-sdk/renderware/RwImage.h>
#include <plugin-sdk/renderware/RwRaster.h>
#include <plugin-sdk/renderware/RwRender.h>

#include <mod/thirdparty/imgui/imgui_internal.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

namespace {
using DrawIndexedFn = RwBool (*)(i32, RwIm2DVertex *, i32, RwImVertexIndex *, i32);
using ScissorFn = void (*)(CRect &);
using DefinedState2dFn = void (*)();

// Client: this plugin's own ImGui context. Every plugin built from this template is a client.
bool g_bInitialized = false;
ImGuiContext *g_pImGuiContext = nullptr;
ImGui_RenderCallback g_UserRenderCallback = nullptr;
const ImGuiShared::API *g_pSharedAPI = nullptr;
ImGuiShared::Handle g_hClient = 0;
ImGuiShared::TouchSample g_legacyTouch{}; // render thread only

bool ProviderScalesTouches() {
    return g_pSharedAPI && g_pSharedAPI->bytes >= sizeof(ImGuiShared::API) && g_pSharedAPI->lastTouch;
}

// Provider: used only by the first plugin to initialize; it owns the hooks and cross-plugin focus.
ImGuiShared::Coordinator g_Coordinator;
using usa1to1::core::PixelSize;
using usa1to1::core::ResolveTouchScreenSize;
PixelSize g_screenOverride; // config, set before hooks are installed

// Render thread only. GL_VIEWPORT can describe an offscreen/scaled target,
// so prefer the EGL draw surface when it has a usable size. Neither is a
// guaranteed physical touch extent on every device: use the config override there.
PixelSize DetectTouchScreenSize(PixelSize render) {
    if (g_screenOverride.Valid())
        return g_screenOverride;
    PixelSize surface, viewport;
    if (eglGetCurrentContext() != EGL_NO_CONTEXT) {
        const EGLDisplay display = eglGetCurrentDisplay();
        const EGLSurface drawSurface = eglGetCurrentSurface(EGL_DRAW);
        if (display != EGL_NO_DISPLAY && drawSurface != EGL_NO_SURFACE) {
            EGLint w = 0, h = 0;
            if (eglQuerySurface(display, drawSurface, EGL_WIDTH, &w) == EGL_TRUE &&
                eglQuerySurface(display, drawSurface, EGL_HEIGHT, &h) == EGL_TRUE)
                surface = {w, h};
        }
        if (!surface.Valid()) {
            GLint vp[4]{};
            glGetIntegerv(GL_VIEWPORT, vp);
            // Width/height alone cannot describe a viewport with an offset.
            if (vp[0] == 0 && vp[1] == 0)
                viewport = {vp[2], vp[3]};
        }
    }
    return ResolveTouchScreenSize(g_screenOverride, surface, viewport, render);
}

// Game pointers
DrawIndexedFn g_pfnDrawIndexed = nullptr;
ScissorFn g_pfnSetScissor = nullptr;
DefinedState2dFn g_pfnDefinedState2d = nullptr;
void **g_ppEngine = nullptr;
void **g_ppRenderQueue = nullptr;
float *g_pfNearZ = nullptr;
float *g_pfRecipNearClip = nullptr;

// Hooks (provider only)
void (*orig_AND_TouchEvent)(int, int, int, int) = nullptr;
void (*orig_MobileMenuRender)(void *) = nullptr;
void (*orig_ShutdownRenderWare)() = nullptr;

uint32_t g_LastRenderTime = 0;
uint32_t g_LastRenderFrame = 0xFFFFFFFF;
std::vector<RwIm2DVertex> g_VertexBuffer;

bool CopyPixels(unsigned char *dst, int dstPitch, const unsigned char *src, int srcPitch, int width, int height) {
    if (!dst || !src || width <= 0 || height <= 0)
        return false;
    const int rowBytes = width * 4;
    if (dstPitch < rowBytes || srcPitch < rowBytes)
        return false;
    for (int y = 0; y < height; ++y)
        std::memcpy(dst + static_cast<size_t>(y) * dstPitch, src + static_cast<size_t>(y) * srcPitch, rowBytes);
    return true;
}

void SetState(RwRenderState state, uintptr_t value) { RwRenderStateSet(state, reinterpret_cast<void *>(value)); }

void SetupRenderState(const ImDrawList *, const ImDrawCmd *) {
    SetState(rwRENDERSTATEZTESTENABLE, false);
    SetState(rwRENDERSTATEZWRITEENABLE, false);
    SetState(rwRENDERSTATEVERTEXALPHAENABLE, true);
    SetState(rwRENDERSTATESRCBLEND, rwBLENDSRCALPHA);
    SetState(rwRENDERSTATEDESTBLEND, rwBLENDINVSRCALPHA);
    SetState(rwRENDERSTATEFOGENABLE, false);
    SetState(rwRENDERSTATECULLMODE, rwCULLMODECULLNONE);
    SetState(rwRENDERSTATEALPHATESTFUNCTION, rwALPHATESTFUNCTIONGREATER);
    SetState(rwRENDERSTATEALPHATESTFUNCTIONREF, 0);
    SetState(rwRENDERSTATETEXTUREFILTER, rwFILTERLINEAR);
    SetState(rwRENDERSTATETEXTUREADDRESS, rwTEXTUREADDRESSCLAMP);
}

void SetSamplerLinear(const ImDrawList *, const ImDrawCmd *) { SetState(rwRENDERSTATETEXTUREFILTER, rwFILTERLINEAR); }

void SetSamplerNearest(const ImDrawList *, const ImDrawCmd *) { SetState(rwRENDERSTATETEXTUREFILTER, rwFILTERNEAREST); }

struct SavedRenderState {
    static constexpr RwRenderState states[] = {
        rwRENDERSTATEZTESTENABLE,   rwRENDERSTATEZWRITEENABLE,      rwRENDERSTATEVERTEXALPHAENABLE,
        rwRENDERSTATESRCBLEND,      rwRENDERSTATEDESTBLEND,         rwRENDERSTATEFOGENABLE,
        rwRENDERSTATECULLMODE,      rwRENDERSTATEALPHATESTFUNCTION, rwRENDERSTATEALPHATESTFUNCTIONREF,
        rwRENDERSTATETEXTUREFILTER, rwRENDERSTATETEXTUREADDRESS,    rwRENDERSTATETEXTURERASTER};
    uintptr_t values[std::size(states)]{};
    bool valid[std::size(states)]{};

    SavedRenderState() {
        for (size_t i = 0; i < std::size(states); ++i)
            valid[i] = RwRenderStateGet(states[i], &values[i]) != 0;
    }
    ~SavedRenderState() {
        for (size_t i = 0; i < std::size(states); ++i)
            if (valid[i])
                SetState(states[i], values[i]);
    }
};

void DestroyTexture(ImTextureData *texture) {
    if (auto *raster = static_cast<RwRaster *>(texture->BackendUserData))
        RwRasterDestroy(raster);
    texture->BackendUserData = nullptr;
    texture->SetTexID(ImTextureID_Invalid);
    texture->SetStatus(ImTextureStatus_Destroyed);
}

bool UpdateTexture(ImTextureData *texture) {
    if (texture->Status == ImTextureStatus_WantDestroy) {
        if (texture->UnusedFrames > 0)
            DestroyTexture(texture);
        return true;
    }
    if (texture->Status != ImTextureStatus_WantCreate && texture->Status != ImTextureStatus_WantUpdates)
        return true;
    if (texture->Format != ImTextureFormat_RGBA32)
        return false;

    const bool creating = texture->Status == ImTextureStatus_WantCreate;
    const auto freeImage = [](RwImage *image) { RwImageDestroy(image); };
    std::unique_ptr<RwImage, decltype(freeImage)> image(RwImageCreate(texture->Width, texture->Height, 32), freeImage);
    if (!image || !RwImageAllocatePixels(image.get()))
        return false;
    if (!CopyPixels(image->cpPixels, image->stride, static_cast<const unsigned char *>(texture->GetPixels()),
                    texture->GetPitch(), texture->Width, texture->Height))
        return false;

    auto *raster = static_cast<RwRaster *>(texture->BackendUserData);
    if (creating) {
        RwInt32 width{}, height{}, depth{}, format{};
        if (!RwImageFindRasterFormat(image.get(), rwRASTERTYPETEXTURE, &width, &height, &depth, &format))
            return false;
        raster = RwRasterCreate(width, height, depth, format);
        if (!raster)
            return false;
    }
    if (!raster || !RwRasterSetFromImage(raster, image.get())) {
        if (creating && raster)
            RwRasterDestroy(raster);
        return false;
    }
    texture->BackendUserData = raster;
    texture->SetTexID(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(raster)));
    texture->SetStatus(ImTextureStatus_OK);
    return true;
}

// Called by the provider once per frame for every plugin, back to front, with the touches this plugin owns.
void ClientFrame(void *, const ImGuiShared::Touch *events, uint32_t count, int32_t width, int32_t height,
                 float deltaTime) {
    if (!g_pImGuiContext)
        return;

    ImGui::SetCurrentContext(g_pImGuiContext);
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    io.DeltaTime = deltaTime;

    const bool providerScalesTouch = ProviderScalesTouches();
    const PixelSize screen = providerScalesTouch ? PixelSize{} : DetectTouchScreenSize({width, height});
    for (uint32_t i = 0; i < count; ++i) {
        const ImGuiShared::Touch &ev = events[i];
        float x = ev.x, y = ev.y;
        if (!providerScalesTouch) {
            const auto mapped = usa1to1::core::ScreenToRender(x, y, screen.width, screen.height, width, height);
            g_legacyTouch = {x, y, mapped.x, mapped.y, mapped.scaleX, mapped.scaleY,
                             screen.width, screen.height, width, height, g_legacyTouch.sequence + 1};
            x = mapped.x;
            y = mapped.y;
        }
        io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
        io.AddMousePosEvent(x, y);
        if (ev.type == 2 /* TOUCH_PUSH */)
            io.AddMouseButtonEvent(0, true);
        else if (ev.type == 1 /* TOUCH_POP */ || ev.type == 4 /* TOUCH_CANCEL */)
            io.AddMouseButtonEvent(0, false);
    }

    const float uiScale = std::clamp(static_cast<float>(height) / 720.0f, 0.8f, 2.0f);
    ImGui::GetStyle().FontSizeBase = 22.0f * uiScale;
    ImGui::GetStyle().FramePadding = ImVec2(10.0f * uiScale, 6.0f * uiScale);

    // The snapshot includes captured and pass-through touches. Log on the render
    // thread, not in the hot input callback (also works while paused).
    static uint64_t loggedSequence = 0;
    static auto lastLog = std::chrono::steady_clock::time_point::min();
    const auto sample = ImGui_ImplRenderWare_LastTouch();
    const auto now = std::chrono::steady_clock::now();
    if (sample.sequence != loggedSequence &&
        (loggedSequence == 0 || now - lastLog >= std::chrono::seconds(2))) {
        char line[256];
        ImGui_ImplRenderWare_FormatTouch(sample, line, sizeof(line));
        if (sample.screenW <= 0 || sample.screenH <= 0 || sample.renderW <= 0 || sample.renderH <= 0)
            logger->Print(LogP_Warn, "%s (missing dimensions; raw fallback)", line);
        else
            logger->Info("%s", line);
        loggedSequence = sample.sequence;
        lastLog = now;
    }

    ImGui::NewFrame();

    if (g_UserRenderCallback)
        g_UserRenderCallback();
    ImGui::EndFrame();

    // Only touches inside these rectangles reach ImGui; everything else (joystick, camera, gaps between
    // windows) stays with the game. Child windows are clipped by their parent, so only top-level windows count.
    // Collected after EndFrame: until then ImGui's invisible fallback "Debug" window (400x400 at 60,60) is still
    // active and would swallow taps on the radar and the top-left of the pause map.
    std::vector<ImGuiShared::Rect> rects;
    for (const ImGuiWindow *win : g_pImGuiContext->Windows) {
        if (!win->Active || win->Hidden || (win->Flags & (ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_ChildWindow)))
            continue;
        ImGuiShared::Rect r{win->Pos.x, win->Pos.y, win->Pos.x + win->Size.x, win->Pos.y + win->Size.y};
        if (!providerScalesTouch) {
            // A legacy provider compares against unscaled input, so publish
            // screen-space rectangles for this client only.
            const auto tl = usa1to1::core::ScreenToRender(r.left, r.top, width, height, screen.width, screen.height);
            const auto br = usa1to1::core::ScreenToRender(r.right, r.bottom, width, height, screen.width, screen.height);
            r = {tl.x, tl.y, br.x, br.y};
        }
        rects.push_back(r);
    }
    g_pSharedAPI->setRects(g_pSharedAPI->state, g_hClient, rects.data(), static_cast<uint32_t>(rects.size()));

    ImGui::Render();
    ImGui_ImplRenderWare_RenderDrawData(ImGui::GetDrawData());
}

void ClientReset(void *) { ImGui_ImplRenderWare_Shutdown(); }

void Hooked_AND_TouchEvent(int type, int finger, int x, int y) {
    if (!g_Coordinator.ProcessTouch(type, finger, static_cast<float>(x), static_cast<float>(y)) && orig_AND_TouchEvent)
        orig_AND_TouchEvent(type, finger, x, y);
}

void RenderFrame() {
    if (!g_ppEngine || !*g_ppEngine || !g_ppRenderQueue || !*g_ppRenderQueue)
        return;

    RsGlobalType *pScreen = &RsGlobal;
    if (!pScreen || pScreen->maximumWidth <= 0 || pScreen->maximumHeight <= 0)
        return;

    const int width = pScreen->maximumWidth;
    const int height = pScreen->maximumHeight;

    // Render2dStuff and MobileMenu::Render may both run in one frame. The pause-mode clock keeps ticking
    // while the game is paused (the gameplay clock freezes), so the pause menu still gets fresh UI frames.
    const uint32_t frameTimeMS = CTimer::m_snTimeInMillisecondsPauseMode;
    if (g_LastRenderFrame == frameTimeMS && frameTimeMS != 0)
        return;
    g_LastRenderFrame = frameTimeMS;

    // Calculate delta time
    uint32_t currentClockMS = CTimer::GetTimeMS();
    float deltaTime = 1.0f / 60.0f;
    if (g_LastRenderTime > 0 && currentClockMS >= g_LastRenderTime) {
        deltaTime = static_cast<float>(currentClockMS - g_LastRenderTime) / 1000.0f;
        if (deltaTime <= 0.0f || deltaTime > 0.1f)
            deltaTime = 1.0f / 60.0f;
    }
    g_LastRenderTime = currentClockMS;

    const auto screen = DetectTouchScreenSize({width, height});
    g_Coordinator.Frame(width, height, deltaTime, screen.width, screen.height);
}

void Hooked_MobileMenuRender(void *menu) {
    if (orig_MobileMenuRender)
        orig_MobileMenuRender(menu);
    RenderFrame();
}

void Hooked_ShutdownRenderWare() {
    g_Coordinator.Reset(); // Every plugin frees its ImGui textures while RenderWare is still alive.
    if (orig_ShutdownRenderWare)
        orig_ShutdownRenderWare();
}

void InstallProviderHooks() {
    void *pTouchSym = (void *)aml->GetSym(GetMainLibrary(), "_Z14AND_TouchEventiiii");
#ifndef AML64
    if (!pTouchSym)
        pTouchSym = reinterpret_cast<void *>(GetMainLibraryAddress() + 0x2697C0);
#endif
    if (pTouchSym) {
        aml->Hook(pTouchSym, reinterpret_cast<void *>(&Hooked_AND_TouchEvent),
                  reinterpret_cast<void **>(&orig_AND_TouchEvent));
    }

    // Hook render loop (2D pass and pause menu)
    Events::drawingEvent.after += RenderFrame;

    void *pMobileMenuRender = reinterpret_cast<void *>(aml->GetSym(GetMainLibrary(), "_ZN10MobileMenu6RenderEv"));
    if (pMobileMenuRender) {
        aml->Hook(pMobileMenuRender, reinterpret_cast<void *>(&Hooked_MobileMenuRender),
                  reinterpret_cast<void **>(&orig_MobileMenuRender));
    }

    void *pShutdown = reinterpret_cast<void *>(aml->GetSym(GetMainLibrary(), "_ZN5CGame18ShutdownRenderWareEv"));
    if (pShutdown) {
        aml->Hook(pShutdown, reinterpret_cast<void *>(&Hooked_ShutdownRenderWare),
                  reinterpret_cast<void **>(&orig_ShutdownRenderWare));
    }
}

// C ABI shims: other plugins call these through the published API, so only POD crosses the boundary.
ImGuiShared::Handle SharedAdd(void *state, const ImGuiShared::Client *client) {
    return client ? static_cast<ImGuiShared::Coordinator *>(state)->Add(*client) : 0;
}

void SharedRemove(void *state, ImGuiShared::Handle id) { static_cast<ImGuiShared::Coordinator *>(state)->Remove(id); }

void SharedSetRects(void *state, ImGuiShared::Handle id, const ImGuiShared::Rect *rects, uint32_t count) {
    static_cast<ImGuiShared::Coordinator *>(state)->SetRects(id, rects, count);
}

void SharedLastTouch(void *state, ImGuiShared::TouchSample *out) {
    *out = static_cast<ImGuiShared::Coordinator *>(state)->LastTouch();
}

// The first plugin to initialize publishes its coordinator through AML and installs the hooks; later plugins
// join it, so the touched plugin's windows draw on top of the others. AML cannot unhook or unregister an
// interface, so the provider plugin must stay loaded for the whole session.
const ImGuiShared::API *AcquireSharedAPI() {
    static const ImGuiShared::API *const api = [] {
        using namespace ImGuiShared;
        const auto *existing = static_cast<const API *>(GetInterface(interfaceName));
        if (existing && existing->version == version && existing->bytes >= offsetof(API, lastTouch))
            return existing;

        static API local{version, sizeof(API), &g_Coordinator, SharedAdd, SharedRemove, SharedSetRects, SharedLastTouch};
        if (existing)
            logger->Error("ImGui/RW: shared ImGui interface v%u is incompatible; windows will not share focus",
                          existing->version);
        else
            RegisterInterface(interfaceName, &local);
        InstallProviderHooks();
        return static_cast<const API *>(&local);
    }();
    return api;
}
} // namespace

bool ImGui_ImplRenderWare_InitRenderer() {
    g_pfnDrawIndexed = GetMainLibrarySymbol<DrawIndexedFn>(
        "_Z35RwIm2DRenderIndexedPrimitive_BUGFIX15RwPrimitiveTypeP14RwOpenGLVertexiPti");
    if (!g_pfnDrawIndexed)
        g_pfnDrawIndexed = GetMainLibrarySymbol<DrawIndexedFn>(
            "_Z28RwIm2DRenderIndexedPrimitive15RwPrimitiveTypeP14RwOpenGLVertexiPti");

    g_pfnSetScissor = GetMainLibrarySymbol<ScissorFn>("_ZN7CWidget10SetScissorER5CRect");
    g_pfnDefinedState2d = GetMainLibrarySymbol<DefinedState2dFn>("_Z14DefinedState2dv");
    g_ppEngine = GetMainLibrarySymbol<void **>("RwEngineInstance");
    g_ppRenderQueue = GetMainLibrarySymbol<void **>("renderQueue");
    g_pfNearZ = GetMainLibrarySymbol<float *>("_ZN9CSprite2d11NearScreenZE");
    g_pfRecipNearClip = GetMainLibrarySymbol<float *>("_ZN9CSprite2d13RecipNearClipE");

    if (!g_pfnDrawIndexed || !g_pfnSetScissor || !g_ppEngine || !g_ppRenderQueue || !g_pfNearZ || !g_pfRecipNearClip ||
        !RwRenderStateSet || !RwRenderStateGet || !RwImageCreate || !RwImageDestroy || !RwImageAllocatePixels ||
        !RwImageFindRasterFormat || !RwRasterCreate || !RwRasterDestroy || !RwRasterSetFromImage) {
        logger->Error("ImGui/RW: required RenderWare symbols missing; renderer disabled");
        return false;
    }

    auto &io = ImGui::GetIO();
    io.BackendRendererName = "imgui_impl_renderware";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;

    auto &platform = ImGui::GetPlatformIO();
    platform.Renderer_TextureMaxWidth = platform.Renderer_TextureMaxHeight = 4096;
    platform.DrawCallback_ResetRenderState = SetupRenderState;
    platform.DrawCallback_SetSamplerLinear = SetSamplerLinear;
    platform.DrawCallback_SetSamplerNearest = SetSamplerNearest;
    return true;
}

void ImGui_ImplRenderWare_SetScreenSizeOverride(int width, int height) {
    g_screenOverride = {width, height};
}

ImGuiShared::TouchSample ImGui_ImplRenderWare_LastTouch() {
    ImGuiShared::TouchSample sample{};
    if (!ProviderScalesTouches())
        return g_legacyTouch;
    g_pSharedAPI->lastTouch(g_pSharedAPI->state, &sample);
    return sample;
}

void ImGui_ImplRenderWare_FormatTouch(const ImGuiShared::TouchSample &sample, char *out, size_t size) {
    if (!sample.sequence) {
        std::snprintf(out, size, "touch raw -> imgui: no sample yet");
        return;
    }
    std::snprintf(out, size, "touch raw (%.1f, %.1f) -> imgui (%.1f, %.1f) | scale %.3f x %.3f | screen %dx%d vs render %dx%d",
                  sample.rawX, sample.rawY, sample.x, sample.y, sample.scaleX, sample.scaleY,
                  sample.screenW, sample.screenH, sample.renderW, sample.renderH);
}

bool ImGui_ImplRenderWare_Init(ImGui_RenderCallback renderCallback) {
    if (g_bInitialized)
        return true;

    logger->Info("ImGui/RW: initializing...");

    IMGUI_CHECKVERSION();
    g_pImGuiContext = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_pImGuiContext);

    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    ImGui::StyleColorsDark();

    g_UserRenderCallback = renderCallback;

    if (!ImGui_ImplRenderWare_InitRenderer()) {
        ImGui::DestroyContext(g_pImGuiContext);
        g_pImGuiContext = nullptr;
        return false;
    }

    g_pSharedAPI = AcquireSharedAPI();
    const ImGuiShared::Client client{nullptr, ClientFrame, ClientReset};
    g_hClient = g_pSharedAPI->add(g_pSharedAPI->state, &client);
    if (!g_hClient) {
        logger->Error("ImGui/RW: shared ImGui interface rejected this plugin");
        ImGui::DestroyContext(g_pImGuiContext);
        g_pImGuiContext = nullptr;
        return false;
    }

    g_bInitialized = true;
    logger->Info("ImGui/RW: initialized successfully");
    return true;
}

void ImGui_ImplRenderWare_Shutdown() {
    if (!g_bInitialized)
        return;

    if (g_hClient)
        g_pSharedAPI->remove(g_pSharedAPI->state, g_hClient);
    g_hClient = 0;

    if (g_pImGuiContext) {
        ImGui::SetCurrentContext(g_pImGuiContext);
        for (auto *texture : ImGui::GetPlatformIO().Textures) {
            if (texture->BackendUserData && texture->RefCount == 1)
                DestroyTexture(texture);
        }
        ImGui::GetPlatformIO().ClearRendererHandlers();
        ImGui::DestroyContext(g_pImGuiContext);
        g_pImGuiContext = nullptr;
    }

    g_bInitialized = false;
    logger->Info("ImGui/RW: shutdown completed");
}

bool ImGui_ImplRenderWare_RenderDrawData(ImDrawData *drawData) {
    static_assert(sizeof(ImDrawIdx) == sizeof(RwImVertexIndex), "RenderWare requires 16-bit indices");
    static_assert(sizeof(RwIm2DVertex) == 28, "GTA SA mobile vertex layout mismatch");
    if (!drawData || !drawData->Valid || !g_ppEngine || !*g_ppEngine || !g_ppRenderQueue || !*g_ppRenderQueue)
        return false;

    if (drawData->Textures) {
        for (auto *texture : *drawData->Textures) {
            if (!UpdateTexture(texture))
                return false;
        }
    }

    if (drawData->TotalVtxCount == 0)
        return true;

    const float width = drawData->DisplaySize.x * drawData->FramebufferScale.x;
    const float height = drawData->DisplaySize.y * drawData->FramebufferScale.y;
    if (width <= 0 || height <= 0 || !std::isfinite(width) || !std::isfinite(height))
        return false;

    const SavedRenderState saved;
    SetupRenderState(nullptr, nullptr);

    for (const auto *list : drawData->CmdLists) {
        g_VertexBuffer.resize(list->VtxBuffer.Size);
        for (int i = 0; i < list->VtxBuffer.Size; ++i) {
            const auto &src = list->VtxBuffer[i];
            auto &dst = g_VertexBuffer[i];
            dst.x = (src.pos.x - drawData->DisplayPos.x) * drawData->FramebufferScale.x;
            dst.y = (src.pos.y - drawData->DisplayPos.y) * drawData->FramebufferScale.y;
            dst.z = *g_pfNearZ;
            dst.rhw = *g_pfRecipNearClip;
            dst.emissiveColor = src.col;
            dst.u = src.uv.x;
            dst.v = src.uv.y;
        }

        for (const auto &command : list->CmdBuffer) {
            if (command.UserCallback) {
                if (command.UserCallback == ImDrawCallback_ResetRenderState)
                    SetupRenderState(list, &command);
                else
                    command.UserCallback(list, &command);
                continue;
            }

            const float left =
                std::clamp((command.ClipRect.x - drawData->DisplayPos.x) * drawData->FramebufferScale.x, 0.0f, width);
            const float top =
                std::clamp((command.ClipRect.y - drawData->DisplayPos.y) * drawData->FramebufferScale.y, 0.0f, height);
            const float right =
                std::clamp((command.ClipRect.z - drawData->DisplayPos.x) * drawData->FramebufferScale.x, 0.0f, width);
            const float bottom =
                std::clamp((command.ClipRect.w - drawData->DisplayPos.y) * drawData->FramebufferScale.y, 0.0f, height);

            if (left >= right || top >= bottom || command.ElemCount == 0)
                continue;
            if (command.IdxOffset > static_cast<unsigned>(list->IdxBuffer.Size) ||
                command.ElemCount > static_cast<unsigned>(list->IdxBuffer.Size) - command.IdxOffset ||
                command.VtxOffset >= static_cast<unsigned>(list->VtxBuffer.Size))
                continue;

            struct {
                float left, bottom, right, top;
            } clip{left, bottom, right, top};
            g_pfnSetScissor(*(CRect *)(void *)&clip);

            SetState(rwRENDERSTATETEXTURERASTER, static_cast<uintptr_t>(command.GetTexID()));
            g_pfnDrawIndexed(rwPRIMTYPETRILIST, g_VertexBuffer.data() + command.VtxOffset,
                             list->VtxBuffer.Size - static_cast<int>(command.VtxOffset),
                             list->IdxBuffer.Data + command.IdxOffset, static_cast<int>(command.ElemCount));

            struct {
                float left, bottom, right, top;
            } resetScissor{0.0f, 0.0f, 0.0f, 0.0f};
            g_pfnSetScissor(*(CRect *)(void *)&resetScissor);
        }
    }

    SetState(rwRENDERSTATETEXTURERASTER, 0);
    struct {
        float left, bottom, right, top;
    } resetScissor{0.0f, 0.0f, 0.0f, 0.0f};
    g_pfnSetScissor(*(CRect *)(void *)&resetScissor);

    if (g_pfnDefinedState2d)
        g_pfnDefinedState2d();

    return true;
}
