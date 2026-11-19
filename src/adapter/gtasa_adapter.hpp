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
#include "core/status_feed.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

struct CBaseModelInfo;
struct CObject;
struct CEntity;
struct CVector;
struct CColPoint;
struct CStoredCollPoly;

namespace usa1to1 {
namespace adapter {

class GTASAAdapter final : public core::IEngineBridge {
  public:
    using TickCallback = std::function<void(double dt)>;
    using TouchCallback = std::function<bool(int action, int track, int x, int y)>;
    using ShutdownCallback = std::function<void()>;

    GTASAAdapter();
    ~GTASAAdapter() override;

    // Loader-only stage: resolves libGTASA.so and installs the hooks. Touches no
    // game state beyond the symbol tables, so it is safe to run from OnModLoad.
    bool Initialize();
    // Engine stage: verifies the engine symbols we actually call and probes the
    // road model. Only valid from the first CGame::Process tick - the model info
    // array, pools and streaming are not live before that.
    bool InitializeRuntime();
    void Shutdown();

    bool IsReady() const { return m_ready; }
    bool RuntimeReady() const { return m_runtimeReady; }
    // True once the world is live (player ped exists, streaming is up). This is
    // the gate for the runtime stage: before it, model info is still loading.
    bool IsEngineLoaded();
    void SetModDirectoryOverride(const std::string &dir) { m_modDirOverride = dir; }
    // Real size of the model table for this installation (20000 on stock 2.00,
    // larger with Fastman92). Everything above it is never scanned.
    void SetModelIdLimit(int limit);
    int ModelScanLimit() const;
    const std::string &LastError() const { return m_lastError; }
    // Status messages for the overlay. Never routed through aml->ShowToast():
    // that is a JNI facade and calling it from the game thread aborts on ART.
    const core::StatusFeed &Status() const { return m_status; }
    // "6/12 engine symbols" style summary for the log/telemetry.
    std::string SymbolSummary() const;

    void SetTickCallback(TickCallback cb) { m_onTick = std::move(cb); }
    void SetTouchCallback(TouchCallback cb) { m_onTouch = std::move(cb); }
    void SetShutdownCallback(ShutdownCallback cb) { m_onShutdown = std::move(cb); }

    // Scans CModelInfo and fills `out` with real ids, names and collision bounds.
    // Returns the number of models added.
    int BuildLiveCatalog(core::ModelCatalog &out, const std::string &assetDir);

    // Diagnostics for the report / log.
    const std::string &GameVersionLabel() const { return m_versionLabel; }
    void *MainLibraryHandle() const { return m_libHandle; }

    // ---- IEngineBridge -----------------------------------------------------
    bool IsGameReady() override;
    bool GetPlayerPosition(core::Vec3f &out) override;
    void SetPlayerPosition(const core::Vec3f &pos) override;
    void HoldPlayerPosition(const core::Vec3f &pos) override;
    void RecoverPlayerPosition(const core::Vec3f &pos) override;
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
    bool IsObjectCollisionReady(core::ObjectHandle handle) override;
    bool HasRoadSupportAt(core::ObjectHandle handle, const core::Vec3f &surface) override;
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
    bool OnTouch(int action, int track, int x, int y);
    void OnGameShutdown();

  private:
    struct OwnedObject {
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

    // Every engine entry point we call is verified once, up front: a missing
    // mobile export disables the feature instead of jumping through a null
    // function pointer.
    struct EngineSymbols {
        bool streaming = false;
        bool requestModel = false;
        bool releaseModel = false;
        bool flushModels = false;
        bool worldAdd = false;
        bool worldRemove = false;
        bool objectCreate = false;
        bool modelInfo = false;
        bool findPlayer = false;
        bool findVehicle = false;
        bool markers = false;
        bool pools = false;
        int Present() const;
        int Total() const { return 12; }
    };

    void *Symbol(const char *name);
    bool VerifyEngineSymbols();
    static double MonotonicSeconds();

    void InstallHooks();
    void RemoveHooks();
    int MovePoolEntities(bool peds, bool vehicles, const core::Vec3f &delta, bool includeInterior);
    CBaseModelInfo **LiveModelTable() const;
    const CBaseModelInfo *LiveModelInfo(int modelId) const;
    bool MoveWorldObject(CObject *obj, int modelId, const core::Vec3f &pos);
    void LogUnreadyModel(int modelId, const char *reason);
    void DeletePoolObject(CObject *obj, bool inWorld);
    std::string DetectVersion();

    void *m_libHandle = nullptr;
    uintptr_t m_gameBase = 0;
    EngineSymbols m_syms;
    bool m_ready = false;
    bool m_runtimeReady = false;
    std::string m_modDirOverride;
    bool m_hooksInstalled = false;
    std::string m_lastError;
    std::string m_versionLabel = "unknown";

    double m_lastProcessTime = 0.0;
    double m_now = 0.0;
    float m_fps = 0.0f;

    int m_modelIdLimit = 20000;
    CObject *(*m_createObject)(int, bool) = nullptr;
    void (*m_createRwObject)(CEntity *) = nullptr;
    bool (*m_processVerticalLine)(const CVector &, float, CColPoint &, CEntity *&,
                                  bool, bool, bool, bool, bool, bool, CStoredCollPoly *) = nullptr;
    void *m_modelByName = nullptr;
    CBaseModelInfo **m_exportedModelTable = nullptr;
    std::unordered_map<int, double> m_nextUnreadyLog;
    int m_roadModel = -1;
    int m_interiorArea = 0;
    uint64_t m_nextMarkerEngineId = 0x55000000ULL;

    core::StatusFeed m_status;
    std::unordered_map<uintptr_t, OwnedObject> m_owned;
    std::unordered_map<uint64_t, MarkerInfo> m_markers;

    TickCallback m_onTick;
    TouchCallback m_onTouch;
    ShutdownCallback m_onShutdown;
};

} // namespace adapter
} // namespace usa1to1
