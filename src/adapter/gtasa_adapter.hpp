// Real bridge to libGTASA.so for GTA:SA Android 2.00 (armeabi-v7a) and
// 2.10 (arm64-v8a) running under Android Mod Loader.
//
// Everything that touches the game image lives here and nowhere else:
//   * symbol resolution through aml->GetSym on mangled C++ names,
//   * hooks on CGame::Process / CGame::Shutdown / AND_TouchEvent,
//   * player, vehicle, camera and pool access,
//   * CStreaming / CObject / CWorld object lifecycle,
//   * C3dMarkers for EnEx markers,
//   * a live CModelInfo scan that feeds the procedural catalogue.
#pragma once

#include "core/engine_bridge.hpp"
#include "core/model_catalog.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

namespace usa1to1 {
namespace adapter {

class GTASAAdapter final : public core::IEngineBridge {
  public:
    using TickCallback = std::function<void(double dt)>;
    using TouchCallback = std::function<void(int action, int track, int x, int y)>;
    using ShutdownCallback = std::function<void()>;

    GTASAAdapter();
    ~GTASAAdapter() override;

    // Resolves libGTASA.so, installs hooks, probes the road model.
    bool Initialize();
    void Shutdown();

    bool IsReady() const { return m_ready; }
    const std::string &LastError() const { return m_lastError; }

    void SetTickCallback(TickCallback cb) { m_onTick = std::move(cb); }
    void SetTouchCallback(TouchCallback cb) { m_onTouch = std::move(cb); }
    void SetShutdownCallback(ShutdownCallback cb) { m_onShutdown = std::move(cb); }

    // Scans CModelInfo and fills `out` with real ids, names and collision bounds.
    // Returns the number of models added.
    int BuildLiveCatalog(core::ModelCatalog &out);

    // Diagnostics for the report / log.
    const std::string &GameVersionLabel() const { return m_versionLabel; }
    void *MainLibraryHandle() const { return m_libHandle; }

    // ---- IEngineBridge -----------------------------------------------------
    bool IsGameReady() override;
    bool GetPlayerPosition(core::Vec3f &out) override;
    void SetPlayerPosition(const core::Vec3f &pos) override;
    bool IsPlayerInVehicle() override;
    float GetFps() override;

    int ShiftPlayer(const core::Vec3f &delta) override;
    int ShiftPlayerVehicle(const core::Vec3f &delta) override;
    int ShiftCamera(const core::Vec3f &delta) override;
    int ShiftStreamedEntities(const core::Vec3f &delta, bool includeInterior) override;
    int ShiftWorldPeds(const core::Vec3f &delta) override;
    int ShiftWorldVehicles(const core::Vec3f &delta) override;

    bool RequestModel(int modelId, bool dynamic) override;
    bool IsModelLoaded(int modelId) override;
    void ReleaseModel(int modelId) override;
    void FlushModelStreaming() override;

    core::ObjectHandle CreateObject(const core::SpawnParams &params) override;
    void DestroyObject(core::ObjectHandle handle) override;
    void MoveObject(core::ObjectHandle handle, const core::Vec3f &pos) override;
    size_t OwnedObjectCount() const override { return m_owned.size(); }

    void PlaceMarker(uint64_t markerId, const core::Vec3f &pos, float radius, bool interior) override;
    void RemoveMarker(uint64_t markerId) override;

    void SetPlayerInterior(int areaId) override;

    void ShowToast(const std::string &msg) override;
    void Vibrate(int ms) override;
    double NowSeconds() override;
    std::string ModDirectory() override;
    int ProbeRoadModel() override;
    int CountStreamedEntities() override;

    // ---- hook entry points (called from the static trampolines) -----------
    void OnGameProcess();
    void OnTouch(int action, int track, int x, int y);
    void OnGameShutdown();

  private:
    struct OwnedObject {
        uintptr_t pointer = 0;
        int modelId = -1;
        bool interior = false;
        core::Vec3f pos;
    };
    struct MarkerInfo {
        core::Vec3f pos;
        float radius = 1.0f;
        bool interior = false;
        uint64_t engineId = 0;
    };

    void InstallHooks();
    void RemoveHooks();
    int MovePoolEntities(bool peds, bool vehicles, const core::Vec3f &delta, bool includeInterior);
    bool SafeTeleport(uintptr_t entityPtr, const core::Vec3f &pos);
    std::string DetectVersion();

    void *m_libHandle = nullptr;
    bool m_ready = false;
    bool m_hooksInstalled = false;
    std::string m_lastError;
    std::string m_versionLabel = "unknown";

    double m_lastProcessTime = 0.0;
    double m_now = 0.0;
    float m_fps = 0.0f;
    uint32_t m_lastFrameMs = 0;

    int m_roadModel = -2; // -2 = not probed yet, -1 = nothing found
    int m_interiorArea = 0;
    uint64_t m_nextMarkerEngineId = 0x55000000ULL;

    std::unordered_map<uintptr_t, OwnedObject> m_owned;
    std::unordered_map<uint64_t, MarkerInfo> m_markers;

    TickCallback m_onTick;
    TouchCallback m_onTouch;
    ShutdownCallback m_onShutdown;
};

} // namespace adapter
} // namespace usa1to1
