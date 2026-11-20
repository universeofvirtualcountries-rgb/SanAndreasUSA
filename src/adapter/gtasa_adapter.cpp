#include "gtasa_adapter.hpp"

#include "core/log.hpp"
#include "core/safe.hpp"
#include "core/spawn_validation.hpp"
#include "core/test_car.hpp"

#include <cfloat> // FLT_EPSILON, used by plugin-sdk/gta_base/Vector.h

#include <plugin-sdk/game_sa/Events.h>
#include <plugin-sdk/game_sa/base/ColModel.h>
#include <plugin-sdk/game_sa/base/Matrix.h>
#include <plugin-sdk/game_sa/base/Timer.h>
#include <plugin-sdk/game_sa/engine/Cam.h>
#include <plugin-sdk/game_sa/engine/ColStore.h>
#include <plugin-sdk/game_sa/engine/Game.h>
#include <plugin-sdk/game_sa/engine/Markers.h>
#include <plugin-sdk/game_sa/engine/ModelInfo.h>
#include <plugin-sdk/game_sa/engine/Streaming.h>
#include <plugin-sdk/game_sa/engine/World.h>
#include <plugin-sdk/game_sa/engine/VehicleModelInfo.h>
#include <plugin-sdk/game_sa/entity/Automobile.h>
#include <plugin-sdk/game_sa/entity/Camera.h>
#include <plugin-sdk/game_sa/entity/Object.h>
#include <plugin-sdk/game_sa/entity/PlayerPed.h>
#include <plugin-sdk/game_sa/entity/Vehicle.h>
#include <plugin-sdk/game_sa/other/Pools.h>
#include <plugin-sdk/renderware/RwObject.h>
#include <plugin-sdk/renderware/RwFrame.h>

#include <mod/amlmod.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <vector>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <unordered_set>

using namespace usa1to1::core;

namespace usa1to1 {
namespace adapter {
namespace {

constexpr int kMaxModelIds = 26316;      // hard ceiling of the mobile streaming arrays
constexpr int kModelScanFirst = 400;     // ids below this are engine-reserved
constexpr int kDefaultModelIdLimit = 20000; // stock GTA SA 2.00 model table size
constexpr int kMarkerSlots = 12;      // keep well under the engine marker array size
constexpr u16 kMarkerType = MARKER3D_CYLINDER;
constexpr uint64_t kMarkerEngineBase = 0x5A000000ULL;

GTASAAdapter *g_adapter = nullptr;
void (*g_origGameProcess)() = nullptr;
void (*g_origTouchEvent)(int, int, int, int) = nullptr;
bool (*g_origShutdown)() = nullptr;

void HookGameProcess() {
    if (g_adapter)
        g_adapter->OnGameProcess();
    if (g_origGameProcess)
        g_origGameProcess();
}

void HookTouchEvent(int action, int track, int x, int y) {
    if (g_adapter && g_adapter->OnTouch(action, track, x, y))
        return;
    if (g_origTouchEvent)
        g_origTouchEvent(action, track, x, y);
}

bool HookShutdown() {
    if (g_adapter)
        g_adapter->OnGameShutdown();
    return g_origShutdown ? g_origShutdown() : true;
}

inline CVector ToCV(const Vec3f &v) { return CVector(v.x, v.y, v.z); }
inline Vec3f ToVec3(const CVector &v) { return Vec3f(v.x, v.y, v.z); }

std::string LowerCopy(const char *s) {
    std::string out(s ? s : "");
    for (char &c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool Contains(const std::string &haystack, const char *needle) {
    return haystack.find(needle) != std::string::npos;
}

PropTag ClassifyByName(const std::string &name, bool isRoad, float colRadius) {
    if (isRoad)
        return PropTag::RoadFlat;
    if (Contains(name, "tree") || Contains(name, "pine") || Contains(name, "oak") || Contains(name, "veg_") ||
        Contains(name, "bush") || Contains(name, "palm"))
        return PropTag::Tree;
    if (Contains(name, "light") || Contains(name, "lamp"))
        return PropTag::StreetLight;
    if (Contains(name, "door"))
        return PropTag::Door;
    if (Contains(name, "wall") || Contains(name, "fence") || Contains(name, "railing"))
        return PropTag::Wall;
    if (Contains(name, "floor") || Contains(name, "pave") || Contains(name, "slab") || Contains(name, "tile"))
        return PropTag::Floor;
    if (Contains(name, "shelf") || Contains(name, "desk") || Contains(name, "chair") || Contains(name, "table") ||
        Contains(name, "counter") || Contains(name, "cabinet") || Contains(name, "furn") || Contains(name, "sofa") ||
        Contains(name, "bed") || Contains(name, "crate"))
        return PropTag::Furniture;
    if (Contains(name, "shop") || Contains(name, "house") || Contains(name, "build") || Contains(name, "block") ||
        Contains(name, "apart") || Contains(name, "office") || Contains(name, "store") || Contains(name, "station") ||
        Contains(name, "tower") || Contains(name, "warehouse") || Contains(name, "hotel") || Contains(name, "bank"))
        return PropTag::Building;
    if (colRadius >= 3.0f)
        return PropTag::Building;
    return PropTag::Prop;
}

} // namespace

GTASAAdapter::GTASAAdapter() = default;
GTASAAdapter::~GTASAAdapter() { Shutdown(); }

void *GTASAAdapter::Symbol(const char *name) {
    if (!aml || !m_libHandle)
        return nullptr;
    const uintptr_t raw = aml->GetSym(m_libHandle, name);
    void *addr = reinterpret_cast<void *>(raw);
    if (!core::IsSaneHandle(addr)) {
        USA_LOG_W("Adapter: engine symbol %s unavailable (%s) - feature disabled", name,
                  core::HexPointer(addr).c_str());
        return nullptr;
    }
    return addr;
}

int GTASAAdapter::EngineSymbols::Present() const {
    return streaming + requestModel + releaseModel + flushModels + worldAdd + worldRemove + objectCreate + modelInfo +
           findPlayer + findVehicle + markers + pools;
}

std::string GTASAAdapter::SymbolSummary() const {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%d/%d engine symbols", m_syms.Present(), m_syms.Total());
    return std::string(buf);
}

void GTASAAdapter::SetModelIdLimit(int limit) {
    if (limit < kModelScanFirst || limit > kMaxModelIds) {
        USA_LOG_W("Adapter: model id limit %d out of range, using %d", limit, kDefaultModelIdLimit);
        m_modelIdLimit = kDefaultModelIdLimit;
        return;
    }
    m_modelIdLimit = limit;
}

int GTASAAdapter::ModelScanLimit() const {
    if (m_modelIdLimit < kModelScanFirst || m_modelIdLimit > kMaxModelIds)
        return kDefaultModelIdLimit;
    return m_modelIdLimit;
}

bool GTASAAdapter::VerifyEngineSymbols() {
    m_syms.streaming = Symbol("_ZN10CStreaming13IsInitialisedEv") != nullptr;
    m_syms.requestModel = Symbol("_ZN10CStreaming12RequestModelEii") != nullptr;
    m_syms.releaseModel = Symbol("_ZN10CStreaming19SetModelIsDeletableEi") != nullptr;
    m_syms.flushModels = Symbol("_ZN10CStreaming22LoadAllRequestedModelsEb") != nullptr;
    m_syms.worldAdd = Symbol("_ZN6CWorld3AddEP7CEntity") != nullptr;
    m_syms.worldRemove = Symbol("_ZN6CWorld6RemoveEP7CEntity") != nullptr;
    m_createObject = reinterpret_cast<decltype(m_createObject)>(Symbol("_ZN7CObject6CreateEib"));
    m_createRwObject = reinterpret_cast<decltype(m_createRwObject)>(Symbol("_ZN7CEntity14CreateRwObjectEv"));
    m_updateRwMatrix = reinterpret_cast<decltype(m_updateRwMatrix)>(Symbol("_ZN7CEntity14UpdateRwMatrixEv"));
    // Native reference return is a pointer at the ABI boundary. Validate it
    // before dereferencing; vehicles can use a special COL instead of model COL.
    m_getEntityCollision = reinterpret_cast<decltype(m_getEntityCollision)>(Symbol("_ZN7CEntity11GetColModelEv"));
    m_vehicleAlloc = reinterpret_cast<decltype(m_vehicleAlloc)>(Symbol(BYBIT("_ZN8CVehiclenwEj", "_ZN8CVehiclenwEm")));
    if (!m_vehicleAlloc)
        m_vehicleAlloc = reinterpret_cast<decltype(m_vehicleAlloc)>(Symbol("_ZN8CVehiclenwEj"));
    m_vehicleFree = reinterpret_cast<decltype(m_vehicleFree)>(Symbol("_ZN8CVehicledlEPv"));
    m_automobileCtor = reinterpret_cast<decltype(m_automobileCtor)>(Symbol("_ZN11CAutomobileC2Eihh"));
    m_automobileDtor = reinterpret_cast<decltype(m_automobileDtor)>(Symbol("_ZN11CAutomobileD2Ev"));
    if (const auto *vtable = static_cast<const uintptr_t *>(Symbol("_ZTV17CVehicleModelInfo")))
        m_vehicleModelVtable = vtable + 2;
    m_addColRef = reinterpret_cast<decltype(m_addColRef)>(Symbol("_ZN9CColStore6AddRefEi"));
    m_removeColRef = reinterpret_cast<decltype(m_removeColRef)>(Symbol("_ZN9CColStore9RemoveRefEi"));
    m_processVerticalLine = reinterpret_cast<decltype(m_processVerticalLine)>(
        Symbol("_ZN6CWorld19ProcessVerticalLineERK7CVectorfR9CColPointRP7CEntitybbbbbbP15CStoredCollPoly"));
    m_testWorldSphere = reinterpret_cast<decltype(m_testWorldSphere)>(
        Symbol("_ZN6CWorld22TestSphereAgainstWorldE7CVectorfP7CEntitybbbbbb"));
    if (!m_processVerticalLine)
        USA_LOG_W("Adapter: no CWorld::ProcessVerticalLine; the polygon cannot confirm road support");
    m_syms.objectCreate = m_createObject && Symbol("_ZN7CObjectD2Ev");
    m_exportedModelTable = static_cast<CBaseModelInfo **>(Symbol("_ZN10CModelInfo16ms_modelInfoPtrsE"));
    m_modelByName = Symbol("_ZN10CModelInfo12GetModelInfoEPKcPi");
    m_syms.modelInfo = m_modelByName != nullptr;
    m_syms.findPlayer = Symbol("_Z13FindPlayerPedi") != nullptr;
    m_syms.findVehicle = Symbol("_Z17FindPlayerVehicleib") != nullptr;
    m_syms.markers =
        Symbol(BYBIT("_ZN10C3dMarkers11PlaceMarkerEjtR7CVectorfhhhhtfsfffb",
                     "_ZN10C3dMarkers11PlaceMarkerEytR7CVectorfhhhhtfsfffb")) != nullptr;
    // Pools are fixed-address globals: usable only once the game base is known.
    m_syms.pools = core::IsSanePointer(reinterpret_cast<const void *>(m_gameBase));

    USA_LOG_I("Adapter: %s (%s)", SymbolSummary().c_str(), m_versionLabel.c_str());
    return m_syms.Present() > 0;
}

double GTASAAdapter::MonotonicSeconds() {
    using Clock = std::chrono::steady_clock;
    static const Clock::time_point start = Clock::now();
    return std::chrono::duration<double>(Clock::now() - start).count();
}

bool GTASAAdapter::Initialize() {
    if (m_ready)
        return true;

    if (!aml) {
        m_lastError = "AML interface is null (mod not loaded by AML?)";
        return false;
    }

    // The loader itself is third-party code with a version-dependent vtable.
    // Slot 0 (GetCurrentGame) and slot 4 (GetLib) have existed since the first
    // AML release; everything newer is treated as "may return garbage".
    const char *game = aml->GetCurrentGame();
    if (core::IsReadableCString(game, 256) && game[0] != '\0')
        USA_LOG_I("Adapter: loader game = %s", game);
    else
        USA_LOG_W("Adapter: GetCurrentGame returned %s, continuing with defaults",
                  core::HexPointer(game).c_str());

    const uintptr_t base = aml->GetLib("libGTASA.so");
    if (core::IsSanePointer(reinterpret_cast<const void *>(base))) {
        m_gameBase = base;
        USA_LOG_I("Adapter: libGTASA.so base = %s", core::HexPointer(reinterpret_cast<const void *>(base)).c_str());
    } else {
        USA_LOG_E("Adapter: GetLib(libGTASA.so) returned %s - engine data is unreachable",
                  core::HexPointer(reinterpret_cast<const void *>(base)).c_str());
        m_lastError = "libGTASA.so base address unavailable";
        return false;
    }

    // GetLibHandle is a much newer slot (28). Its result is validated before it
    // is handed to GetSym: a stale slot returns small garbage, not a handle.
    void *handle = aml->GetLibHandle("libGTASA.so");
    if (!core::IsSaneHandle(handle)) {
        USA_LOG_E("Adapter: GetLibHandle returned %s (loader predates this API?)", core::HexPointer(handle).c_str());
        m_lastError = "loader does not provide a libGTASA.so handle";
        return false;
    }
    m_libHandle = handle;

    m_versionLabel = DetectVersion();
    USA_LOG_I("Adapter: handle %s, version %s", core::HexPointer(m_libHandle).c_str(), m_versionLabel.c_str());

    InstallHooks();
    if (!m_hooksInstalled) {
        m_lastError = "failed to hook the game loop";
        return false;
    }

    // Resolving symbols only touches the loader tables, so it is safe here even
    // though the engine itself is still loading.
    VerifyEngineSymbols();

    g_adapter = this;
    m_ready = true;
    m_lastProcessTime = 0.0;
    USA_LOG_I("Adapter: hooks installed (engine stage waits for the first game tick)");
    return true;
}

bool GTASAAdapter::InitializeRuntime() {
    if (m_runtimeReady)
        return true;
    if (!m_ready || !m_hooksInstalled) {
        m_lastError = "runtime requested before the loader stage completed";
        return false;
    }

    m_runtimeReady = true;
    USA_LOG_I("Adapter: runtime ready (streaming/pools/model info are live)");
    return true;
}

void GTASAAdapter::InstallHooks() {
    if (m_hooksInstalled)
        return;

    auto hookSym = [this](const char *sym, void *replacement, void **original, const char *label) -> bool {
        void *addr = reinterpret_cast<void *>(aml->GetSym(m_libHandle, sym));
#ifndef AML64
        if (!addr && std::strcmp(sym, "_Z14AND_TouchEventiiii") == 0)
            addr = reinterpret_cast<void *>(m_gameBase + 0x2697C0); // stock 2.00 fallback
#endif
        if (!core::IsSaneHandle(addr)) {
            USA_LOG_W("Adapter: symbol %s (%s) not exported by this build (%s)", sym, label,
                      core::HexPointer(addr).c_str());
            return false;
        }
        const bool ok = aml->Hook(addr, replacement, original);
        if (ok)
            USA_LOG_I("Adapter: hooked %s at %p", label, addr);
        else
            USA_LOG_W("Adapter: HOOK FAILED for %s at %p", label, addr);
        return ok;
    };

    bool any = false;
    any |= hookSym("_ZN5CGame7ProcessEv", reinterpret_cast<void *>(&HookGameProcess),
                   reinterpret_cast<void **>(&g_origGameProcess), "CGame::Process");
    any |= hookSym("_Z14AND_TouchEventiiii", reinterpret_cast<void *>(&HookTouchEvent),
                   reinterpret_cast<void **>(&g_origTouchEvent), "AND_TouchEvent");
    if (hookSym("_ZN5CGame8ShutdownEv", reinterpret_cast<void *>(&HookShutdown),
                reinterpret_cast<void **>(&g_origShutdown), "CGame::Shutdown")) {
        any = true;
    }

    m_hooksInstalled = any;
}

void GTASAAdapter::RemoveHooks() {
    // AML hooks cannot be safely reverted in mid-flight on every version; we
    // only flip the guard so our callbacks become no-ops.
    m_hooksInstalled = false;
}

std::string GTASAAdapter::DetectVersion() {
    // AML32/AML64 come from mod/amlmod.h, which derives them from the target
    // arch (__arm__ / __aarch64__), i.e. exactly the ABI of the game build.
#if defined(AML64)
    return "2.10+ arm64-v8a";
#else
    return "2.00 armeabi-v7a";
#endif
}

void GTASAAdapter::Shutdown() {
    if (!m_ready)
        return;
    USA_LOG_I("Adapter: shutting down");
    m_onShutdown = nullptr;
    m_onTick = nullptr;
    m_onTouch = nullptr;
    // Destroy everything we own first: the game pools are about to be torn down.
    std::vector<uintptr_t> owned;
    owned.reserve(m_owned.size());
    for (const auto &kv : m_owned)
        owned.push_back(kv.first);
    for (uintptr_t p : owned)
        DestroyObject(ObjectHandle{p});
    m_markers.clear();
    if (m_requiredRoadModel >= 0)
        ReleaseRoadModel(m_requiredRoadModel);

    // Hide our marker slots.
    for (int i = 0; i < kMarkerSlots; ++i) {
        const uint64_t engineId = kMarkerEngineBase + static_cast<uint64_t>(i);
        CVector hidden(0.0f, 0.0f, -2000.0f);
        C3dMarkers::PlaceMarker(static_cast<uptr>(engineId), kMarkerType, hidden, 0.001f, 0, 0, 0, 0, 0, 0.0f, 0, 0.0f,
                                0.0f, 1.0f, false);
    }
    m_ready = false;
    m_runtimeReady = false;
    m_syms = EngineSymbols{};
    g_adapter = nullptr;
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------

void GTASAAdapter::OnGameProcess() {
    // Before the runtime stage the engine clock is not trusted: use the plain
    // monotonic clock so the very first tick still reaches ModApp::OnTick (that
    // tick is what boots the runtime stage in the first place).
    const double now = m_runtimeReady ? static_cast<double>(CTimer::GetTimeInMS()) / 1000.0 : MonotonicSeconds();
    double dt = 1.0 / 60.0;
    if (m_lastProcessTime > 0.0 && now > m_lastProcessTime)
        dt = now - m_lastProcessTime;
    m_lastProcessTime = now;
    m_now = now;
    if (dt > 0.0005 && dt < 0.5) {
        const float instant = static_cast<float>(1.0 / dt);
        m_fps = m_fps <= 0.0f ? instant : m_fps * 0.9f + instant * 0.1f;
    }

    if (m_onTick && dt > 0.0 && dt < 1.0)
        m_onTick(dt);
}

bool GTASAAdapter::OnTouch(int action, int track, int x, int y) {
    return m_onTouch && m_onTouch(action, track, x, y);
}

void GTASAAdapter::OnGameShutdown() {
    if (m_onShutdown)
        m_onShutdown();
}

// ---------------------------------------------------------------------------
// Player / vehicle / camera
// ---------------------------------------------------------------------------

bool GTASAAdapter::IsEngineLoaded() {
    if (!m_ready || !m_syms.findPlayer || !m_syms.streaming)
        return false;
    return CStreaming::IsInitialised() && FindPlayerPed(0) != nullptr;
}

bool GTASAAdapter::IsGameReady() {
    // CStreaming is initialised once the world is live; before that the player
    // does not exist yet and any shift would be garbage.
    if (!m_runtimeReady || !m_syms.findPlayer || !m_syms.streaming)
        return false;
    return FindPlayerPed(0) != nullptr && CStreaming::IsInitialised();
}

bool GTASAAdapter::GetPlayerPosition(Vec3f &out) {
    if (!m_runtimeReady || !m_syms.findPlayer)
        return false;
    CPlayerPed *ped = FindPlayerPed(0);
    if (!ped)
        return false;
    out = ToVec3(ped->GetPosition());
    return true;
}

void GTASAAdapter::SetPlayerPosition(const Vec3f &pos) {
    if (!m_runtimeReady || !m_syms.findPlayer)
        return;
    CPlayerPed *ped = FindPlayerPed(0);
    if (!ped) {
        USA_LOG_W("Adapter: SetPlayerPosition without a player ped");
        return;
    }
    CPhysical *phys = static_cast<CPhysical *>(ped);
    CVector savedSpeed = phys->m_vecMoveSpeed;
    ped->Teleport(ToCV(pos), false);
    ped->UpdateRwFrame();
    phys->m_vecMoveSpeed = savedSpeed; // teleport must never eat inertia
}

void GTASAAdapter::RecoverPlayerPosition(const Vec3f &pos) {
    Vec3f from;
    if (!GetPlayerPosition(from))
        return;
    const Vec3f delta = pos - from;
    CVehicle *vehicle = m_syms.findVehicle ? FindPlayerVehicle(0, false) : nullptr;
    if (vehicle) {
        ShiftPlayerVehicle(delta);
        vehicle->m_vecMoveSpeed.z = 0.0f;
    }
    SetPlayerPosition(pos);
    if (auto *ped = FindPlayerPed(0))
        ped->m_vecMoveSpeed.z = 0.0f;
    ShiftCamera(delta);
}

void GTASAAdapter::HoldPlayerPosition(const Vec3f &pos) {
    RecoverPlayerPosition(pos);
    auto stop = [](CPhysical *body) {
        if (body) {
            body->m_vecMoveSpeed = CVector(0.0f, 0.0f, 0.0f);
            body->m_vecFrictionMoveSpeed = CVector(0.0f, 0.0f, 0.0f);
        }
    };
    if (m_runtimeReady && m_syms.findPlayer)
        stop(FindPlayerPed(0));
    if (m_runtimeReady && m_syms.findVehicle)
        stop(FindPlayerVehicle(0, false));
}

bool GTASAAdapter::IsPlayerInVehicle() {
    if (!m_runtimeReady || !m_syms.findVehicle)
        return false;
    return FindPlayerVehicle(0, false) != nullptr;
}

float GTASAAdapter::GetFps() { return m_fps; }

int GTASAAdapter::ShiftPlayer(const Vec3f &delta) {
    CPlayerPed *ped = FindPlayerPed(0);
    if (!ped)
        return -1;
    CPhysical *phys = static_cast<CPhysical *>(ped);
    const CVector savedSpeed = phys->m_vecMoveSpeed;
    const CVector pos = ped->GetPosition();
    ped->Teleport(CVector(pos.x + delta.x, pos.y + delta.y, pos.z + delta.z), false);
    ped->UpdateRwFrame();
    phys->m_vecMoveSpeed = savedSpeed;
    return 1;
}

int GTASAAdapter::ShiftPlayerVehicle(const Vec3f &delta) {
    CVehicle *veh = FindPlayerVehicle(0, false);
    if (!veh)
        return 0;
    CPhysical *phys = static_cast<CPhysical *>(veh);
    const CVector savedSpeed = phys->m_vecMoveSpeed;
    const CVector pos = veh->GetPosition();
    veh->Teleport(CVector(pos.x + delta.x, pos.y + delta.y, pos.z + delta.z), false);
    veh->UpdateRwFrame();
    phys->m_vecMoveSpeed = savedSpeed;
    return 1;
}

int GTASAAdapter::ShiftCamera(const Vec3f &delta) {
    const CVector d(delta.x, delta.y, delta.z);
    int moved = 0;

    // The three CCam slots hold the actual camera vectors used for rendering
    // this frame; everything positional must move, directions must not.
    for (int i = 0; i < 3; ++i) {
        CCam &cam = TheCamera.m_aCams[i];
        cam.m_vecSource += d;
        cam.m_vecLastAboveWaterCamPosition += d;
        cam.m_vecSourceBeforeLookBehind += d;
        cam.m_vecCamFixedModeSource += d;
        cam.m_vecTargetCoorsForFudgeInter += d;
        for (int h = 0; h < 4; ++h)
            cam.m_avecTargetHistoryPos[h] += d;
        ++moved;
    }

    // Camera-level interpolation state.
    TheCamera.m_vecStartingSourceForInterPol += d;
    TheCamera.m_vecSourceWhenInterPol += d;
    TheCamera.m_vecTargetWhenInterPol += d;
    TheCamera.m_vecSourceDuringInter += d;
    TheCamera.m_vecTargetDuringInter += d;
    TheCamera.m_vecGameCamPos += d;
    TheCamera.m_vecRealPreviousCameraPosition += d;
    TheCamera.m_vecFixedModeSource += d;
    TheCamera.m_vecAimingTargetCoors += d;
    TheCamera.m_vecCutSceneOffset += d;
    TheCamera.m_vecAttachedCamOffset += d;
    return moved;
}

int GTASAAdapter::MovePoolEntities(bool peds, bool vehicles, const Vec3f &delta, bool includeInterior) {
    const CVector d(delta.x, delta.y, delta.z);
    const CPlayerPed *playerPed = FindPlayerPed(0);
    const CVehicle *playerVeh = FindPlayerVehicle(0, false);
    int moved = 0;

    auto moveEntity = [&](CPhysical *phys) {
        CEntity *ent = static_cast<CEntity *>(phys);
        if (!ent)
            return;
        if (!includeInterior) {
            const CVector &pos = ent->GetPosition();
            if (IsInteriorZ(pos.z))
                return; // interior pocket, never shifted
            if (ent->m_nInterior != 0)
                return;
        }
        const CVector pos = ent->GetPosition();
        const CVector savedSpeed = phys->m_vecMoveSpeed;
        ent->Teleport(CVector(pos.x + d.x, pos.y + d.y, pos.z + d.z), false);
        ent->UpdateRwFrame();
        phys->m_vecMoveSpeed = savedSpeed; // inertia survives the shift
        ++moved;
    };

    if (peds && CPools::ms_pPedPool) {
        for (int i = 0; i < CPools::ms_pPedPool->GetSize(); ++i) {
            CPed *ped = CPools::ms_pPedPool->GetAt(i);
            if (!ped || ped == playerPed)
                continue;
            moveEntity(ped);
        }
    }
    if (vehicles && CPools::ms_pVehiclePool) {
        for (int i = 0; i < CPools::ms_pVehiclePool->GetSize(); ++i) {
            CVehicle *veh = CPools::ms_pVehiclePool->GetAt(i);
            if (!veh || veh == playerVeh)
                continue;
            moveEntity(veh);
        }
    }
    return moved;
}

int GTASAAdapter::ShiftWorldPeds(const Vec3f &delta) { return MovePoolEntities(true, false, delta, false); }

int GTASAAdapter::ShiftWorldVehicles(const Vec3f &delta) { return MovePoolEntities(false, true, delta, false); }

int GTASAAdapter::ShiftStreamedEntities(const Vec3f &delta, bool includeInterior) {
    const CVector d(delta.x, delta.y, delta.z);
    int moved = 0;

    // 1) Objects this mod spawned (chunk props, polygon slabs, interiors).
    for (auto &kv : m_owned) {
        OwnedObject &obj = kv.second;
        if (obj.interior && !includeInterior)
            continue;
        auto *object = reinterpret_cast<CObject *>(kv.first);
        if (auto *pool = CPools::ms_pObjectPool; !pool || !pool->IsObjectValid(object))
            continue;
        const Vec3f target = ToVec3(object->GetPosition() + d);
        if (MoveWorldObject(object, obj.modelId, target)) {
            obj.pos = target;
            ++moved;
        }
    }

    // 2) Our 3D markers (doors, polygon rings, exit markers).
    for (auto &kv : m_markers) {
        MarkerInfo &m = kv.second;
        if (m.interior && !includeInterior)
            continue;
        m.pos.x += delta.x;
        m.pos.y += delta.y;
        m.pos.z += delta.z;
        CVector p = ToCV(m.pos);
        C3dMarkers::PlaceMarker(static_cast<uptr>(m.engineId), kMarkerType, p, m.radius, 255, 214, 0, 200, 0, 0.0f, 0,
                                0.0f, 0.0f, 1.0f, false);
    }

    // 3) Generic world objects (traffic cones, script objects) as well.
    if (!includeInterior && CPools::ms_pObjectPool) {
        for (int i = 0; i < CPools::ms_pObjectPool->GetSize(); ++i) {
            CObject *o = CPools::ms_pObjectPool->GetAt(i);
            if (!o)
                continue;
            if (m_owned.count(reinterpret_cast<uintptr_t>(o)))
                continue; // already shifted above
            const CVector pos = o->GetPosition();
            if (IsInteriorZ(pos.z) || o->m_nInterior != 0)
                continue;
            if (MoveWorldObject(o, o->m_nModelIndex, ToVec3(pos + d)))
                ++moved;
        }
    }
    return moved;
}

// ---------------------------------------------------------------------------
// Streaming
// ---------------------------------------------------------------------------

CBaseModelInfo **GTASAAdapter::LiveModelTable() const {
    // The GOT indirection can be repointed by a limit adjuster. Read it
    // again rather than caching model entries or preferring the ELF export.
    auto ***slot = reinterpret_cast<CBaseModelInfo ***>(m_gameBase + BYBIT(0x6796D4, 0x850DB8));
    if (IsMappedRange(slot, sizeof(*slot))) {
        auto **table = *slot;
        if (IsMappedRange(table, sizeof(*table)))
            return table;
    }
    return m_exportedModelTable;
}

const CBaseModelInfo *GTASAAdapter::LiveModelInfo(int modelId) const {
    CBaseModelInfo *info = nullptr;
    if (modelId < 0 || modelId >= ModelScanLimit() ||
        !IsUsableTableEntry(LiveModelTable(), modelId, info, sizeof(*info)))
        return nullptr;
    return info;
}

namespace {
CStreamingInfo *LiveStreamingInfo(uintptr_t gameBase, int id) {
    auto **slot = reinterpret_cast<CStreamingInfo **>(gameBase + BYBIT(0x677DD8, 0x84DBD0));
    if (!IsMappedRange(slot, sizeof(*slot)) || !IsSanePointer(*slot))
        return nullptr;
    auto *entry = *slot + id;
    return IsMappedRange(entry, sizeof(*entry)) ? entry : nullptr;
}
} // namespace

bool GTASAAdapter::RequestModel(int modelId, bool dynamic) {
    (void)dynamic;
    if (!m_runtimeReady || !m_syms.streaming || !m_syms.requestModel)
        return false;
    if (modelId < 0 || modelId >= ModelScanLimit())
        return false;
    if (!CStreaming::IsInitialised())
        return false;
    if (!LiveStreamingInfo(m_gameBase, modelId) || !LiveModelInfo(modelId))
        return false;
    CStreaming::RequestModel(modelId, 0);
    return true;
}

bool GTASAAdapter::IsModelLoaded(int modelId) {
    if (!m_runtimeReady || !m_syms.streaming)
        return false;
    if (modelId < 0 || modelId >= ModelScanLimit())
        return false;
    if (!CStreaming::IsInitialised())
        return false;
    const auto *entry = LiveStreamingInfo(m_gameBase, modelId);
    if (!entry || entry->m_status != 1)
        return false;
    if (!HasMappedModelResources(LiveModelInfo(modelId))) {
        LogUnreadyModel(modelId, "streaming status loaded but model RwObject/COL is missing or unmapped");
        return false;
    }
    return true;
}

void GTASAAdapter::ReleaseModel(int modelId) {
    if (modelId == m_requiredRoadModel || !m_runtimeReady || !m_syms.streaming || !m_syms.releaseModel)
        return;
    if (modelId < 0 || modelId >= ModelScanLimit() || !CStreaming::IsInitialised())
        return;
    CStreaming::SetModelIsDeletable(modelId);
}

void GTASAAdapter::FlushModelStreaming() {
    if (!m_runtimeReady || !m_syms.streaming || !m_syms.flushModels)
        return;
    if (!CStreaming::IsInitialised())
        return;
    // Blocking by design: called on a door transition, never from the chunk loop.
    CStreaming::LoadAllRequestedModels(false);
}

// ---------------------------------------------------------------------------
// Objects
// ---------------------------------------------------------------------------

void GTASAAdapter::LogUnreadyModel(int modelId, const char *reason) {
    const double now = MonotonicSeconds();
    auto &next = m_nextUnreadyLog[modelId];
    if (now >= next) {
        USA_LOG_W("Adapter: skipping unready modelId=%d (%s)", modelId, reason);
        next = now + 5.0;
    }
}

static bool HasCollisionPrimitives(const CBaseModelInfo *info, bool allowSphereOnly = false) {
    if (!HasMappedModelResources(info))
        return false;
    const auto *data = info->m_pColModel->m_pColData;
    if (!IsMappedRange(data, sizeof(*data)))
        return false;
    if (data->m_nNumSpheres > 0 && !IsMappedRange(data->m_pSpheres, sizeof(*data->m_pSpheres) * data->m_nNumSpheres))
        return false;
    if (data->m_nNumBoxes > 0 && !IsMappedRange(data->m_pBoxes, sizeof(*data->m_pBoxes) * data->m_nNumBoxes))
        return false;
    if (data->m_nNumTriangles > 0 && (!IsMappedRange(data->m_pVertices, sizeof(*data->m_pVertices)) ||
                                    !IsMappedRange(data->m_pTriangles, sizeof(*data->m_pTriangles) * data->m_nNumTriangles)))
        return false;
    return data->m_nNumBoxes > 0 || data->m_nNumTriangles > 0 || (allowSphereOnly && data->m_nNumSpheres > 0);
}

static const ColDef *LiveCollisionSlot(uintptr_t gameBase, int slot, int modelId) {
    // Read the live GOT entry (also used by the SDK), not a guessed streaming
    // file ID. FLA can relocate the COL pool and change file-ID boundaries.
    auto ***entry = reinterpret_cast<CColPool ***>(gameBase + BYBIT(0x679660, 0x850CD0));
    if (slot <= 0 || !IsMappedRange(entry, sizeof(*entry)) || !IsMappedRange(*entry, sizeof(**entry)))
        return nullptr;
    const auto *pool = **entry;
    if (!IsMappedRange(pool, sizeof(*pool)) || slot >= pool->m_nSize ||
        !IsSanePointer(pool->m_byteMap) || !IsSanePointer(pool->m_pObjects))
        return nullptr;
    const auto *flag = pool->m_byteMap + slot;
    const auto *def = pool->m_pObjects + slot;
    if (!IsMappedRange(flag, sizeof(*flag)) || flag->bEmpty || !IsMappedRange(def, sizeof(*def)) ||
        modelId < def->m_nModelIdStart || modelId > def->m_nModelIdEnd)
        return nullptr;
    return def;
}

void GTASAAdapter::ReleaseRoadModel(int id) {
    if (id != m_requiredRoadModel)
        return;
    if (m_roadColDef && LiveCollisionSlot(m_gameBase, m_roadColSlot, id) == m_roadColDef &&
        m_roadColDef->m_nRefCount > 0)
        m_removeColRef(m_roadColSlot);
    m_roadColDef = nullptr;
    m_roadColSlot = -1;
    m_requiredRoadModel = -1;
    ReleaseModel(id);
}

bool GTASAAdapter::RequestRoadModel(int id) {
    m_roadCandidateRejected = false;
    if (!IsRoadSlabId(id) || id >= ModelScanLimit() || !LiveModelInfo(id) || !LiveStreamingInfo(m_gameBase, id)) {
        m_roadSpawnError = "invalid live road model/streaming slot";
        return false;
    }
    if (!m_runtimeReady || !m_syms.streaming || !m_syms.requestModel || !CStreaming::IsInitialised()) {
        m_roadSpawnError = "RequestModel unavailable";
        return false;
    }
    if (!m_syms.flushModels) {
        m_roadSpawnError = "LoadAllRequestedModels unavailable";
        return false;
    }
    const auto *info = LiveModelInfo(id);
    const auto *col = info->m_pColModel;
    // DFF loading alone doesn't keep the geographically streamed COL loaded at
    // the polygon's new coordinates. Pin the matching native COL slot when it
    // can be verified; the game's collision streaming will load/retain it.
    if (!m_roadColDef && m_addColRef && m_removeColRef && IsMappedRange(col, sizeof(*col))) {
        const int slot = col->m_nColSlot;
        const auto *def = LiveCollisionSlot(m_gameBase, slot, id);
        if (def && def->m_nRefCount >= 0 && def->m_nRefCount < std::numeric_limits<int16_t>::max()) {
            m_addColRef(slot);
            m_roadColSlot = slot;
            m_roadColDef = def;
        }
    }
    m_requiredRoadModel = id;
    CStreaming::RequestModel(id, 2); // game-required until the polygon releases this candidate
    m_roadSpawnError = nullptr;
    return true;
}

bool GTASAAdapter::IsRoadModelLoaded(int id) {
    const auto *entry = m_runtimeReady && IsRoadSlabId(id) && id < ModelScanLimit()
                            ? LiveStreamingInfo(m_gameBase, id) : nullptr;
    if (!entry || entry->m_status != 1)
        m_roadSpawnError = "model not loaded after RequestModel/flush";
    else if (!IsModelLoaded(id))
        m_roadSpawnError = "loaded model missing RW or COL";
    else if (!HasCollisionPrimitives(LiveModelInfo(id)))
        m_roadSpawnError = "loaded COL missing boxes/triangles";
    else {
        m_roadSpawnError = nullptr;
        return true;
    }
    return false;
}

namespace {
template <class Pool> bool HasFreeNodes(uintptr_t gotAddress, unsigned needed) {
    auto ***got = reinterpret_cast<Pool ***>(gotAddress);
    if (!IsMappedRange(got, sizeof(*got)) || !IsMappedRange(*got, sizeof(**got)))
        return false;
    const auto *pool = **got;
    if (!IsMappedRange(pool, sizeof(*pool)) || pool->m_nSize <= 0 || pool->m_nSize > (1 << 22) ||
        !IsMappedRange(pool->m_byteMap, static_cast<size_t>(pool->m_nSize) * sizeof(*pool->m_byteMap)))
        return false;
    for (int i = 0; i < pool->m_nSize; ++i)
        if (pool->m_byteMap[i].bEmpty && --needed == 0)
            return true;
    return false;
}

bool FinitePosition(const CVector &pos) {
    return std::isfinite(pos.x) && std::isfinite(pos.y) && std::isfinite(pos.z);
}
} // namespace

const char *GTASAAdapter::PhysicalAddError(CPhysical *entity, int id, unsigned type, bool fresh) {
    if (!HasMappedObjectInstance(entity) || !IsMappedRange(entity->m_pRwObject->parent, sizeof(RwFrame)))
        return "instance RW/frame null or unmapped; Add not called";
    if (entity->m_nModelIndex != id || entity->m_nType != type || (fresh && entity->m_pCollisionList))
        return "invalid physical type, model or existing sector links; Add not called";
    if (!IsModelLoaded(id))
        return "model lost streaming/RW/COL readiness; Add not called";
    if (entity->m_matrix && !IsMappedRange(entity->m_matrix, sizeof(*entity->m_matrix)))
        return "unmapped placeable matrix; Add not called";
    if (!m_getEntityCollision)
        return "CEntity::GetColModel unavailable; Add not called";
    const auto *col = m_getEntityCollision(entity);
    if (!IsMappedRange(col, sizeof(*col)))
        return "effective entity COL null or unmapped; Add not called";
    const auto &bounds = col->m_boundSphere;
    const float radius = bounds.m_fRadius;
    if (!FinitePosition(entity->GetPosition()) || !FinitePosition(bounds.m_vecCentre) ||
        !FinitePosition(col->m_boundBox.m_vecMin) || !FinitePosition(col->m_boundBox.m_vecMax) || col->m_boundBox.IsInvalid() ||
        !std::isfinite(radius) || radius <= 0.0f || radius > 1024.0f)
        return "invalid transform or COL bounds; Add not called";
    if (fresh) {
        const auto **sectors = reinterpret_cast<const CRepeatSector **>(m_gameBase + BYBIT(0x6771E4, 0x84C400));
        if (!IsMappedRange(sectors, sizeof(*sectors)) || !IsMappedRange(*sectors, sizeof(**sectors) * 16 * 16))
            return "repeat sectors unavailable; Add not called";
        // One entry-info node + one double-link per overlapping repeat-sector.
        // Overestimate the count by a sector to cover arbitrary grid alignment.
        const unsigned side = static_cast<unsigned>(std::ceil(2.0f * radius / 50.0f)) + 2;
        if (!HasFreeNodes<CEntryInfoNodePool>(m_gameBase + BYBIT(0x679C1C, 0x851850), side * side) ||
            !HasFreeNodes<CPtrNodeDoubleLinkPool>(m_gameBase + BYBIT(0x67836C, 0x84E6F0), side * side))
            return "sector-link pools unavailable or too full; Add not called";
    }
    return nullptr;
}

const char *GTASAAdapter::SyncPhysicalTransform(CPhysical *entity, const Vec3f &pos, float heading) {
    if (!HasMappedObjectInstance(entity))
        return "instance RW null or unmapped; Add not called";
    auto *frame = static_cast<RwFrame *>(entity->m_pRwObject->parent);
    if (!IsMappedRange(frame, sizeof(*frame)))
        return "instance RW frame null or unmapped; Add not called";
    if (m_updateRwMatrix)
        m_updateRwMatrix(entity);
    else {
        const float c = std::cos(heading), s = std::sin(heading);
        auto &m = frame->modelling;
        m.right = {c, s, 0.0f};
        m.up = {-s, c, 0.0f};
        m.at = {0.0f, 0.0f, 1.0f};
        m.pos = {pos.x, pos.y, pos.z};
        m.flags = (m.flags & ~(rwMATRIXINTERNALIDENTITY | rwMATRIXTYPEMASK)) | rwMATRIXTYPEORTHONORMAL;
    }
    entity->UpdateRwFrame();
    return nullptr;
}

bool GTASAAdapter::IsTestCarModel(int id) const {
    if (id != TestCarRequest::kModel || !m_vehicleModelVtable)
        return false;
    const auto *info = LiveModelInfo(id);
    if (!IsMappedRange(info, sizeof(CVehicleModelInfo)))
        return false;
    const void *vtable = nullptr;
    std::memcpy(&vtable, info, sizeof(vtable));
    return vtable == m_vehicleModelVtable &&
           static_cast<const CVehicleModelInfo *>(info)->m_nVehicleType == VEHICLE_AUTOMOBILE;
}

bool GTASAAdapter::RequestTestCarModel(int id) {
    if (!m_runtimeReady || !m_syms.streaming || !m_syms.requestModel || !m_syms.flushModels ||
        !m_syms.releaseModel || !m_syms.worldAdd || !m_vehicleAlloc || !m_vehicleFree ||
        !m_automobileCtor || !m_automobileDtor || !m_getEntityCollision ||
        !CStreaming::IsInitialised() || !IsTestCarModel(id) || !LiveStreamingInfo(m_gameBase, id)) {
        ShowToast("Spawn Car unavailable: vehicle symbols or live Sultan model missing");
        return false;
    }
    CStreaming::RequestModel(id, 2);
    return true;
}

bool GTASAAdapter::FindTestCarGround(const Vec3f &player, Vec3f &position) {
    if (!m_processVerticalLine)
        return false;
    for (const Vec2d &delta : {Vec2d{0, 7}, Vec2d{0, -7}, Vec2d{7, 0}, Vec2d{-7, 0}}) {
        const CVector start(player.x + static_cast<float>(delta.x), player.y + static_cast<float>(delta.y), player.z + 2.0f);
        CColPoint hit{};
        CEntity *entity = nullptr;
        if (m_processVerticalLine(start, player.z - 2.5f, hit, entity,
                                  true, false, false, true, true, false, nullptr) && hit.GetNormal().z > 0.6f) {
            position = {start.x, start.y, hit.GetPosition().z + 1.0f};
            return true;
        }
    }
    return false;
}

bool GTASAAdapter::SpawnTestCar(int id, const Vec3f &pos) {
    const auto fail = [&](const char *reason) {
        ShowToast(std::string("Spawn Car: ") + reason);
        return false;
    };
    if (!m_syms.worldAdd || !m_vehicleAlloc || !m_vehicleFree || !m_automobileCtor || !m_automobileDtor ||
        !IsTestCarModel(id) || !IsModelLoaded(id) || !HasCollisionPrimitives(LiveModelInfo(id), true) || !FinitePosition(ToCV(pos)))
        return fail("Sultan model or collision not ready; Add not called");

    // Repeated button presses must not stack vehicles at the same point.
    if (m_testWorldSphere && m_testWorldSphere(ToCV(pos), 2.8f, nullptr, false, true, true, false, false, false))
        return fail("nearby vehicle or pedestrian blocks the spawn point");
    auto *car = static_cast<CAutomobile *>(m_vehicleAlloc(sizeof(CAutomobile)));
    if (!car)
        return fail("vehicle pool allocation failed");
    if (!IsMappedRange(car, sizeof(*car))) {
        m_vehicleFree(car);
        return fail("vehicle slot not mapped");
    }
    m_automobileCtor(car, id, PARKED_VEHICLE, 1);
    const auto discard = [&] {
        if (!HasMappedObjectInstance(car) || !IsMappedRange(car->m_pRwObject->parent, sizeof(RwFrame)))
            car->m_pRwObject = nullptr;
        if (car->m_matrix && !IsMappedRange(car->m_matrix, sizeof(*car->m_matrix)))
            car->m_matrix = nullptr;
        m_automobileDtor(car);
        m_vehicleFree(car);
    };
    if (!HasMappedObjectInstance(car) || (car->m_matrix && !IsMappedRange(car->m_matrix, sizeof(*car->m_matrix)))) {
        discard();
        return fail("vehicle instance RW/matrix unavailable; Add not called");
    }
    car->m_nStatus = STATUS_ABANDONED;
    car->bUsesCollision = true;
    car->GetPosition() = ToCV(pos);
    if (const char *error = SyncPhysicalTransform(car, pos, 0.0f)) {
        discard();
        return fail(error);
    }
    if (const char *error = PhysicalAddError(car, id, ENTITY_TYPE_VEHICLE, true)) {
        discard();
        return fail(error);
    }
    USA_LOG_I("Car Add begin: model=%d vehicle=%p", id, static_cast<void *>(car));
    CWorld::Add(car);
    USA_LOG_I("Car Add returned: model=%d vehicle=%p", id, static_cast<void *>(car));
    ShowToast("Spawn Car: Sultan ready");
    return true;
}

ObjectHandle GTASAAdapter::CreateObject(const SpawnParams &params) {
    const int id = params.modelId;
    const auto fail = [&](const char *reason, bool reject = true) -> ObjectHandle {
        if (params.roadSlab) {
            m_roadSpawnError = reason;
            m_roadCandidateRejected = reject;
            if (reject)
                ShowToast("Road " + std::to_string(id) + ": " + reason);
        }
        LogUnreadyModel(id, reason);
        return {};
    };
    if (!m_syms.objectCreate || !m_syms.worldAdd || !m_syms.worldRemove)
        return fail("Create/Add/Remove unavailable; Add not called");
    if (id < 0 || id >= ModelScanLimit() || (params.roadSlab && !IsRoadSlabId(id)))
        return fail("invalid model ID; Add not called");
    if (!IsModelLoaded(id))
        return fail("model not loaded before CObject::Create; Add not called", false);
    if (params.roadSlab) {
        m_roadCandidateRejected = false;
        if (m_requiredRoadModel != id)
            return fail("road model not requested; Add not called");
        if (!IsRoadModelLoaded(id))
            return {}; // already has a precise reason; Add not called
    }

    CObject *obj = params.roadSlab ? m_createObject(id, true) : m_createObject(id, false);
    if (!obj)
        return fail("CObject::Create returned nullptr; Add not called");

    if (!IsMappedRange(obj, sizeof(*obj)) || obj->m_nModelIndex != id) {
        DeletePoolObject(obj, false);
        return fail("invalid instance/model index after Create; Add not called");
    }

    // Reject a broken Create(true) result before touching its matrix or flags.
    if (params.roadSlab && !HasMappedObjectInstance(obj)) {
        DeletePoolObject(obj, false);
        return fail("Create(true) returned null or unmapped instance RW; Add not called");
    }
    if (!IsModelLoaded(id)) {
        DeletePoolObject(obj, false);
        return fail("model lost readiness after Create; Add not called", false);
    }
    if (obj->m_matrix && !IsMappedRange(obj->m_matrix, sizeof(*obj->m_matrix))) {
        DeletePoolObject(obj, false);
        return fail("Create returned unmapped matrix; Add not called");
    }

    obj->bIsStatic = !params.dynamic;
    obj->bApplyGravity = params.dynamic;
    obj->bUsesCollision = params.collision;

    // Insert into the world's spatial lists only after the final transform.
    if (CMatrix *mtx = obj->GetMatrix())
        mtx->SetRotateZOnly(params.headingRad);
    else
        obj->m_placement.m_fHeading = params.headingRad;
    obj->GetPosition() = ToCV(params.pos);
    // Some builds leave the instance RW object empty when the factory flag is
    // false. Instantiate it explicitly, never insert a collision-only shell.
    if (!params.roadSlab && !obj->m_pRwObject && m_createRwObject && IsModelLoaded(id))
        m_createRwObject(obj);
    if (const char *error = SyncPhysicalTransform(obj, params.pos, params.headingRad)) {
        DeletePoolObject(obj, false);
        return fail(error);
    }

    // Last gate sits directly before Add, after any calls into RenderWare.
    if (params.roadSlab && !HasCollisionPrimitives(LiveModelInfo(id))) {
        DeletePoolObject(obj, false);
        return fail("road COL lost its surface; Add not called", false);
    }
    if (const char *error = PhysicalAddError(obj, id, ENTITY_TYPE_OBJECT, true)) {
        DeletePoolObject(obj, false);
        return fail(error);
    }
    if (params.roadSlab)
        USA_LOG_I("Road Add begin: model=%d object=%p pos=(%.2f,%.2f,%.2f)", id, static_cast<void *>(obj),
                  params.pos.x, params.pos.y, params.pos.z);
    CWorld::Add(obj);
    if (params.roadSlab)
        USA_LOG_I("Road Add returned: model=%d object=%p", id, static_cast<void *>(obj));

    const auto handle = reinterpret_cast<uintptr_t>(obj);
    m_owned[handle] = OwnedObject{id, params.interior, params.pos};
    return ObjectHandle{handle};
}

void GTASAAdapter::DeletePoolObject(CObject *obj, bool inWorld) {
    auto *pool = CPools::ms_pObjectPool;
    if (!IsMappedRange(obj, sizeof(*obj)) || !pool || !pool->IsObjectValid(obj))
        return; // the game may already have reclaimed the slot
    if (inWorld)
        CWorld::Remove(obj);
    // A non-null but unmapped instance must not be dereferenced by the destructor.
    if (!HasMappedObjectInstance(obj) || !IsMappedRange(obj->m_pRwObject->parent, sizeof(RwFrame)))
        obj->m_pRwObject = nullptr;
    if (obj->m_matrix && !IsMappedRange(obj->m_matrix, sizeof(*obj->m_matrix)))
        obj->m_matrix = nullptr;
    obj->~CObject();
    pool->Delete(obj);
}

bool GTASAAdapter::IsObjectCollisionReady(ObjectHandle handle) {
    const auto it = m_owned.find(handle.raw);
    if (it == m_owned.end())
        return false;
    auto *obj = reinterpret_cast<CObject *>(handle.raw);
    auto *pool = CPools::ms_pObjectPool;
    return pool && pool->IsObjectValid(obj) && HasMappedObjectInstance(obj) && obj->bUsesCollision &&
           obj->m_nModelIndex == it->second.modelId && HasCollisionPrimitives(LiveModelInfo(it->second.modelId));
}

bool GTASAAdapter::HasRoadSupportAt(ObjectHandle handle, const Vec3f &surface) {
    if (!m_processVerticalLine) {
        m_roadSpawnError = "Add done; collision query unavailable";
        return false;
    }
    CColPoint hit{};
    CEntity *entity = nullptr;
    const CVector start(surface.x, surface.y, surface.z + 1.5f);
    if (!m_processVerticalLine(start, surface.z - 3.0f, hit, entity,
                              false, false, false, true, false, false, nullptr)) {
        m_roadSpawnError = "Add done; downward probe missed";
        return false;
    }
    if (entity != reinterpret_cast<CEntity *>(handle.raw) || hit.GetNormal().z <= 0.5f) {
        m_roadSpawnError = "Add done; probe hit another object or steep surface";
        return false;
    }
    m_roadSpawnError = nullptr;
    return true;
}

void GTASAAdapter::DestroyObject(ObjectHandle handle) {
    const auto it = m_owned.find(handle.raw);
    if (it == m_owned.end())
        return;
    const int id = it->second.modelId;
    m_owned.erase(it);
    auto *obj = reinterpret_cast<CObject *>(handle.raw);
    if (IsMappedRange(obj, sizeof(*obj)) && obj->m_nModelIndex == id)
        DeletePoolObject(obj, true);
    // Streaming requests are released by their chunk / polygon / interior owner.
}

bool GTASAAdapter::MoveWorldObject(CObject *obj, int modelId, const Vec3f &pos) {
    // The native Teleport path can remove and internally re-add a CObject.
    if (const char *error = PhysicalAddError(obj, modelId, ENTITY_TYPE_OBJECT, false)) {
        LogUnreadyModel(modelId, error);
        return false;
    }
    obj->Teleport(ToCV(pos), false);
    obj->UpdateRwFrame();
    return true;
}

void GTASAAdapter::MoveObject(ObjectHandle handle, const Vec3f &pos) {
    auto it = m_owned.find(handle.raw);
    if (it == m_owned.end())
        return;
    auto *obj = reinterpret_cast<CObject *>(handle.raw);
    auto *pool = CPools::ms_pObjectPool;
    if (pool && pool->IsObjectValid(obj) && MoveWorldObject(obj, it->second.modelId, pos))
        it->second.pos = pos;
}

// ---------------------------------------------------------------------------
// Markers
// ---------------------------------------------------------------------------

void GTASAAdapter::PlaceMarker(uint64_t markerId, const Vec3f &pos, float radius, bool interior) {
    if (!m_runtimeReady || !m_syms.markers)
        return;
    if (!markerId)
        return;

    auto existing = m_markers.find(markerId);
    MarkerInfo info;
    if (existing != m_markers.end()) {
        info = existing->second;
        info.pos = pos;
        info.radius = radius;
        info.interior = interior;
    } else {
        // Find a free engine slot. Slots are recycled, never leaked, because the
        // engine marker array is tiny and its size differs between versions.
        bool haveSlot = false;
        for (int i = 0; i < kMarkerSlots; ++i) {
            const uint64_t engineId = kMarkerEngineBase + static_cast<uint64_t>(i);
            bool used = false;
            for (const auto &kv : m_markers) {
                if (kv.second.engineId == engineId) {
                    used = true;
                    break;
                }
            }
            if (used)
                continue;
            info.pos = pos;
            info.radius = radius;
            info.interior = interior;
            info.engineId = engineId;
            haveSlot = true;
            break;
        }
        if (!haveSlot)
            return; // all slots busy, drop the marker instead of fighting the engine
    }

    CVector p = ToCV(info.pos);
    // Yellow EnEx cylinder, slowly rotating, alpha 200.
    C3dMarkers::PlaceMarker(static_cast<uptr>(info.engineId), kMarkerType, p, info.radius > 0.0f ? info.radius : 1.0f,
                            255, 214, 0, 200, 0, 0.0f, 0, 0.0f, 0.0f, 1.0f, false);
    m_markers[markerId] = info;
}

void GTASAAdapter::RemoveMarker(uint64_t markerId) {
    if (!m_syms.markers)
        return;
    auto it = m_markers.find(markerId);
    if (it == m_markers.end())
        return;
    // Hide the slot underground; releasing the id lets it be reused immediately.
    CVector hidden(0.0f, 0.0f, -2000.0f);
    C3dMarkers::PlaceMarker(static_cast<uptr>(it->second.engineId), kMarkerType, hidden, 0.001f, 0, 0, 0, 0, 0, 0.0f,
                            0, 0.0f, 0.0f, 1.0f, false);
    m_markers.erase(it);
}

// ---------------------------------------------------------------------------
// Interiors / misc
// ---------------------------------------------------------------------------

void GTASAAdapter::SetPlayerInterior(int areaId) {
    m_interiorArea = areaId;
    CPlayerPed *ped = FindPlayerPed(0);
    if (ped)
        ped->m_nInterior = static_cast<unsigned char>(areaId < 0 ? 0 : areaId);
    CGame::currArea = areaId; // tells the game which interior the camera lives in
}

// Both of these used to go through aml->ShowToast() / aml->DoVibro(). Those are
// JNI facades: AML calls Java methods with a JNIEnv cached from another thread,
// which on Android 15 (ART) ends in art::JavaVMExt::JniAbort and kills the
// process. Status is now logged and rendered by the ImGui overlay instead.
void GTASAAdapter::ShowToast(const std::string &msg) {
    USA_LOG_I("status: %s", msg.c_str());
    m_status.Push(msg, NowSeconds());
}

void GTASAAdapter::Vibrate(int ms) {
    if (ms <= 0)
        return;
    USA_LOG_D("vibrate(%d ms) ignored: no JNI facades are used on the game thread", ms);
}

double GTASAAdapter::NowSeconds() {
    // CTimer lives in the game image; before the runtime stage its globals are
    // not guaranteed to be reachable, so the clock starts as a plain monotonic
    // counter and switches to the engine clock once it is verified.
    if (!m_runtimeReady || !m_syms.streaming || !CStreaming::IsInitialised())
        return MonotonicSeconds();
    return static_cast<double>(CTimer::GetTimeInMS()) / 1000.0;
}

namespace {

bool DirectoryExists(const std::string &path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool EnsureDirectory(const std::string &path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    return DirectoryExists(path);
}

// Takes the directory part of a loader-provided *file* path and appends the mod
// folder, dropping the app's private "files" segment when it is there.
std::string ModDirectoryFromFile(const char *file, const char *folder) {
    std::string path(file);
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos || slash == 0)
        return std::string();
    std::string dir = path.substr(0, slash);
    const std::string filesSuffix = "/files";
    if (dir.size() > filesSuffix.size() &&
        dir.compare(dir.size() - filesSuffix.size(), filesSuffix.size(), filesSuffix) == 0)
        dir.erase(dir.size() - filesSuffix.size());
    if (dir.empty())
        return std::string();
    return dir + "/" + folder;
}

} // namespace

// Paths are resolved without touching any AML slot newer than GetConfigPath /
// GetDataPath: on a loader that predates them, calling a missing slot returns
// garbage (a value like 0x7) and std::string on it is an instant SIGSEGV.
std::string GTASAAdapter::ModDirectory() {
    if (!m_modDirOverride.empty())
        return m_modDirOverride;

    static const char *kFolder = "USA1to1";

    std::string package = "com.rockstargames.gtasa";
    if (aml) {
        const char *game = aml->GetCurrentGame();
        if (core::IsReadableCString(game, 256) && game[0] != '\0' && std::strchr(game, '/') == nullptr &&
            std::strchr(game, ' ') == nullptr)
            package = game;
        else
            USA_LOG_W("Adapter: GetCurrentGame returned %s, assuming %s", core::HexPointer(game).c_str(),
                      package.c_str());
    }

    // 1) The standard AML layout, same order the loader itself documents.
    const std::string standard[] = {
        "/storage/emulated/0/AMLMods/" + package + "/" + kFolder,
        "/sdcard/AMLMods/" + package + "/" + kFolder,
        "/storage/emulated/0/Android/data/" + package + "/files/" + kFolder,
    };
    for (const std::string &candidate : standard) {
        if (DirectoryExists(candidate))
            return candidate;
    }
    for (const std::string &candidate : standard) {
        if (EnsureDirectory(candidate)) {
            USA_LOG_I("Adapter: created mod directory %s", candidate.c_str());
            return candidate;
        }
    }

    // 2) Loader config/data paths (ancient slots). Validated before use.
    if (aml) {
        const char *cfg = aml->GetConfigPath();
        if (core::IsSanePath(cfg)) {
            const std::string dir = ModDirectoryFromFile(cfg, kFolder);
            if (!dir.empty() && (DirectoryExists(dir) || EnsureDirectory(dir)))
                return dir;
        } else {
            USA_LOG_W("Adapter: GetConfigPath returned %s, ignored", core::HexPointer(cfg).c_str());
        }

        const char *data = aml->GetDataPath();
        if (core::IsSanePath(data)) {
            const std::string dir = ModDirectoryFromFile(data, kFolder);
            if (!dir.empty() && (DirectoryExists(dir) || EnsureDirectory(dir)))
                return dir;
        } else {
            USA_LOG_W("Adapter: GetDataPath returned %s, ignored", core::HexPointer(data).c_str());
        }
    }

    USA_LOG_W("Adapter: no writable mod directory found, saving next to the process cwd");
    return ".";
}

int GTASAAdapter::ProbeRoadModel() { return m_roadModel; }

int GTASAAdapter::CountStreamedEntities() {
    int count = 0;
    if (CPools::ms_pPedPool)
        count += static_cast<int>(CPools::ms_pPedPool->GetNoOfUsedSpaces());
    if (CPools::ms_pVehiclePool)
        count += static_cast<int>(CPools::ms_pVehiclePool->GetNoOfUsedSpaces());
    if (CPools::ms_pObjectPool)
        count += static_cast<int>(CPools::ms_pObjectPool->GetNoOfUsedSpaces());
    return count;
}

// ---------------------------------------------------------------------------
// Live catalogue scan
// ---------------------------------------------------------------------------

int GTASAAdapter::BuildLiveCatalog(ModelCatalog &out, const std::string &assetDir) {
    auto **table = LiveModelTable();
    const int limit = ModelScanLimit();
    auto lookup = reinterpret_cast<decltype(CModelInfo::GetModelInfo1)>(m_modelByName);

    // Do not call a virtual method through a pointer obtained by sweeping the
    // numeric slots. Only recognize native static-object vtables (not vehicles,
    // peds, weapons or unknown pointers in a limit-adjusted table).
    std::vector<const void *> staticVtables;
    for (const char *symbol : {"_ZTV16CAtomicModelInfo", "_ZTV22CDamageAtomicModelInfo", "_ZTV14CTimeModelInfo"})
        if (auto *p = static_cast<uintptr_t *>(Symbol(symbol)))
            staticVtables.push_back(p + 2); // Itanium ABI address point

    struct {
        int lookup = 0, slot = 0, type = 0, name = 0, lod = 0, collision = 0, draw = 0;
    } rejected;
    auto describe = [&](int id, const CBaseModelInfo *info, const std::string *expected, ModelProto &proto) {
        if (id < kModelScanFirst || id >= limit || !IsMappedRange(info, sizeof(*info))) {
            ++rejected.slot;
            return false;
        }
        const void *vtable = nullptr;
        std::memcpy(&vtable, static_cast<const void *>(info), sizeof(vtable));
        if (std::find(staticVtables.begin(), staticVtables.end(), vtable) == staticVtables.end()) {
            ++rejected.type;
            return false;
        }
        const auto *terminator = static_cast<const char *>(std::memchr(info->m_szModelName, 0, sizeof(info->m_szModelName)));
        if (!terminator || terminator == info->m_szModelName || info->m_szModelName[0] == '#' ||
            (expected && LowerCopy(info->m_szModelName) != LowerCopy(expected->c_str()))) {
            ++rejected.name;
            return false;
        }
        if (info->bIsLod || LowerCopy(info->m_szModelName).rfind("lod", 0) == 0) {
            ++rejected.lod;
            return false;
        }
        if (!std::isfinite(info->m_fDrawDistance) || info->m_fDrawDistance <= 0.0f) {
            ++rejected.draw;
            return false;
        }
        const CColModel *col = info->m_pColModel;
        if (!IsMappedRange(col, sizeof(*col))) {
            ++rejected.collision;
            return false;
        }
        const auto half = ToVec3(col->m_boundBox.GetSize() * 0.5f);
        const auto center = ToVec3(col->m_boundBox.GetCenter());
        if (!std::isfinite(half.x) || !std::isfinite(half.y) || !std::isfinite(half.z) ||
            !std::isfinite(center.x) || !std::isfinite(center.y) || !std::isfinite(center.z) ||
            half.x <= 0.0f || half.y <= 0.0f || half.z < 0.0f) {
            ++rejected.collision;
            return false;
        }
        proto.id = id;
        proto.name = info->m_szModelName;
        proto.drawDistance = info->m_fDrawDistance;
        proto.isRoad = info->bIsRoad;
        // object.dat entries carry physics even though their model is atomic.
        proto.isDynamic = info->m_nObjectInfoIndex >= 0;
        proto.colHalfExtents = half;
        proto.colOffset = center;
        proto.tag = ClassifyByName(LowerCopy(proto.name.c_str()), proto.isRoad, std::max(half.x, half.y));
        return true;
    };
    auto named = [&](const std::string &name, ModelProto &proto) {
        int id = -1;
        // CModelInfo::GetModelInfo(const char *, int *), with the export
        // resolved explicitly rather than calling a possibly null SDK proxy.
        auto *info = lookup && !name.empty() && name.size() < 21 && name.find('\0') == std::string::npos
                         ? lookup(name.c_str(), &id) : nullptr;
        if (!info) {
            ++rejected.lookup;
            return false;
        }
        if (LiveModelInfo(id) != info) {
            ++rejected.slot; // avoid accepting a name from a different/relocated table
            return false;
        }
        return describe(id, info, &name, proto);
    };

    std::vector<ModelProto> models;
    std::unordered_set<int> ids;
    auto add = [&](ModelProto &&p) {
        if ((p.isRoad || p.tag == PropTag::RoadFlat) && !IsFlatRoadCandidate(p)) {
            p.isRoad = false;
            if (p.tag == PropTag::RoadFlat)
                p.tag = PropTag::Prop;
        }
        if (ids.insert(p.id).second)
            models.push_back(std::move(p));
    };
    ModelCatalog db;
    if (db.LoadFromFile(assetDir + "/objects_db.json")) {
        for (const auto &entry : db.All()) {
            ModelProto p;
            if (entry.isDynamic || !named(entry.name, p))
                continue;
            if (entry.tag != PropTag::Unknown)
                p.tag = entry.tag;
            p.isRoad = p.isRoad || entry.isRoad;
            add(std::move(p));
        }
    }
    const size_t dbCount = models.size();
    int whitelistRoad = -1;
    std::ifstream whitelist(assetDir + "/model_whitelist.json", std::ios::binary);
    Json root;
    const std::string whitelistText((std::istreambuf_iterator<char>(whitelist)), {});
    const Json *entries = Json::TryParse(whitelistText, root) ? root.Find("models") : nullptr;
    if (entries && entries->IsArray()) {
        for (const auto &entry : entries->GetArray()) {
            ModelProto p;
            if (!named(entry.GetString("name", ""), p))
                continue;
            const auto tag = PropTagFromName(entry.GetString("tag", ""));
            if (tag != PropTag::Unknown)
                p.tag = tag;
            if (whitelistRoad < 0 && p.tag == PropTag::RoadFlat && IsFlatRoadCandidate(p))
                whitelistRoad = p.id;
            add(std::move(p));
        }
    } else {
        USA_LOG_W("Adapter: model_whitelist.json missing or invalid");
    }
    const size_t whitelistCount = models.size() - dbCount;

    int tableRoad = -1;
    const bool hasTaggedRoad = std::any_of(models.begin(), models.end(), [](const ModelProto &p) {
        return (p.tag == PropTag::RoadFlat || p.isRoad) && IsFlatRoadCandidate(p);
    });
    // With no usable DB, fill the generation buckets from the numeric fallback.
    // If named entries lack a road, also keep candidates for the flat-collision fallback.
    const bool fillNumeric = !dbCount || (whitelistRoad < 0 && !hasTaggedRoad);
    const bool mappedTable = IsMappedRange(table, sizeof(*table));
    size_t numericCount = 0;
    if (mappedTable) {
        for (int id = kModelScanFirst; id < limit; ++id) {
            const bool numeric = fillNumeric && id >= 3000 && id <= 18000;
            if (!numeric && tableRoad >= 0)
                continue;
            CBaseModelInfo *info = nullptr;
            if (!IsUsableTableEntry(table, id, info, sizeof(*info))) {
                ++rejected.slot;
                continue;
            }
            ModelProto p;
            if (!describe(id, info, nullptr, p))
                continue;
            const bool tablePick = tableRoad < 0 && p.isRoad && IsFlatRoadCandidate(p);
            if (tablePick)
                tableRoad = p.id;
            if (numeric || tablePick) {
                const size_t before = models.size();
                add(std::move(p));
                numericCount += numeric && models.size() != before;
            }
        }
    }

    // Stock IDs are candidates, not trusted metadata. In particular 18450 is
    // an explicit exception, not an expansion of the generic numeric scan.
    for (const int id : kRoadFallbackIds) {
        if (ids.count(id))
            continue;
        ModelProto p;
        if (describe(id, LiveModelInfo(id), nullptr, p) && IsFlatRoadCandidate(p))
            add(std::move(p));
    }

    out.Clear();
    out.AddModels(std::move(models));
    const auto *road = out.PickRoad(tableRoad, whitelistRoad);
    m_roadModel = road ? road->id : -1;
    USA_LOG_I("Adapter: catalogue=%zu db=%zu whitelist=%zu numeric=%zu limit=%d road=%d (%s)",
              out.Size(), dbCount, whitelistCount, numericCount, limit, m_roadModel,
              !road ? "none" : road->id == tableRoad ? "table" : road->id == whitelistRoad ? "whitelist" :
              (road->isRoad || road->tag == PropTag::RoadFlat) ? "RoadFlat" : "largest flat collision");
    if (!out.Size()) {
        USA_LOG_W("Adapter: rejected lookup=%d slot=%d type=%d name=%d lod=%d collision=%d draw=%d",
                  rejected.lookup, rejected.slot, rejected.type, rejected.name, rejected.lod, rejected.collision, rejected.draw);
        for (int i = 0; i < 8; ++i) {
            const bool readable = mappedTable && IsMappedRange(&table[i], sizeof(table[i]));
            USA_LOG_W("Adapter: ms_modelInfoPtrs=%s [%d]=%s%s", HexPointer(table).c_str(), i,
                      HexPointer(readable ? table[i] : nullptr).c_str(), readable ? "" : " (unreadable)");
        }
    }
    return static_cast<int>(out.Size());
}

} // namespace adapter
} // namespace usa1to1
