#include "gtasa_adapter.hpp"

#include "core/log.hpp"
#include "core/safe.hpp"
#include "core/spawn_validation.hpp"

#include <cfloat> // FLT_EPSILON, used by plugin-sdk/gta_base/Vector.h

#include <plugin-sdk/game_sa/Events.h>
#include <plugin-sdk/game_sa/base/ColModel.h>
#include <plugin-sdk/game_sa/base/Matrix.h>
#include <plugin-sdk/game_sa/base/Timer.h>
#include <plugin-sdk/game_sa/engine/Cam.h>
#include <plugin-sdk/game_sa/engine/Game.h>
#include <plugin-sdk/game_sa/engine/Markers.h>
#include <plugin-sdk/game_sa/engine/ModelInfo.h>
#include <plugin-sdk/game_sa/engine/Streaming.h>
#include <plugin-sdk/game_sa/engine/World.h>
#include <plugin-sdk/game_sa/entity/Camera.h>
#include <plugin-sdk/game_sa/entity/Object.h>
#include <plugin-sdk/game_sa/entity/PlayerPed.h>
#include <plugin-sdk/game_sa/entity/Vehicle.h>
#include <plugin-sdk/game_sa/other/Pools.h>
#include <plugin-sdk/renderware/RwObject.h>

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
    m_processVerticalLine = reinterpret_cast<decltype(m_processVerticalLine)>(
        Symbol("_ZN6CWorld19ProcessVerticalLineERK7CVectorfR9CColPointRP7CEntitybbbbbbP15CStoredCollPoly"));
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
    if (!m_runtimeReady || !m_syms.streaming || !m_syms.releaseModel)
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

static bool HasRoadSurface(const CBaseModelInfo *info) {
    if (!HasMappedModelResources(info))
        return false;
    const auto *data = info->m_pColModel->m_pColData;
    if (!IsMappedRange(data, sizeof(*data)))
        return false;
    if (data->m_nNumBoxes > 0 && !IsMappedRange(data->m_pBoxes, sizeof(*data->m_pBoxes) * data->m_nNumBoxes))
        return false;
    if (data->m_nNumTriangles > 0 && (!IsMappedRange(data->m_pVertices, sizeof(*data->m_pVertices)) ||
                                    !IsMappedRange(data->m_pTriangles, sizeof(*data->m_pTriangles) * data->m_nNumTriangles)))
        return false;
    return data->m_nNumBoxes > 0 || data->m_nNumTriangles > 0;
}

ObjectHandle GTASAAdapter::CreateObject(const SpawnParams &params) {
    const int id = params.modelId;
    if (!m_syms.objectCreate || !m_syms.worldAdd || !m_syms.worldRemove ||
        id < 0 || id >= ModelScanLimit() || (params.roadSlab && !IsRoadSlabId(id)))
        return {};
    // No synchronous flush here: the caller keeps the request pending across
    // ticks. A loaded streaming slot alone is not sufficient to construct it.
    if (!IsModelLoaded(id) || (params.roadSlab && !HasRoadSurface(LiveModelInfo(id)))) {
        LogUnreadyModel(id, "not ready for CObject::Create");
        return {};
    }

    CObject *obj = m_createObject(id, false);
    if (!obj)
        return {};

    if (!IsMappedRange(obj, sizeof(*obj)) || obj->m_nModelIndex != id) {
        LogUnreadyModel(id, "unmapped CObject or unexpected model index after Create");
        DeletePoolObject(obj, false);
        return {};
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
    if (!obj->m_pRwObject && m_createRwObject && HasMappedModelResources(LiveModelInfo(id)))
        m_createRwObject(obj);
    if (!HasMappedObjectInstance(obj)) {
        LogUnreadyModel(id, "CObject has no mapped RwObject after factory/instantiation");
        DeletePoolObject(obj, false);
        return {};
    }
    obj->UpdateRwFrame();

    // Look up the live slot again: neither a catalogue-time COL nor a
    // successful Create proves both model and instance are safe for Add.
    const auto *live = LiveModelInfo(id);
    if (!HasMappedObjectInstance(obj) ||
        !(params.roadSlab ? HasRoadSurface(live) : HasMappedModelResources(live))) {
        LogUnreadyModel(id, "model or instance resources missing before CWorld::Add");
        DeletePoolObject(obj, false);
        return {};
    }
    CWorld::Add(obj);

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
    if (!HasMappedObjectInstance(obj))
        obj->m_pRwObject = nullptr;
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
           obj->m_nModelIndex == it->second.modelId && HasRoadSurface(LiveModelInfo(it->second.modelId));
}

bool GTASAAdapter::HasRoadSupportAt(ObjectHandle handle, const Vec3f &surface) {
    if (!m_processVerticalLine)
        return false;
    CColPoint hit{};
    CEntity *entity = nullptr;
    const CVector start(surface.x, surface.y, surface.z + 1.5f);
    return m_processVerticalLine(start, surface.z - 3.0f, hit, entity,
                                 false, false, false, true, false, false, nullptr) &&
           entity == reinterpret_cast<CEntity *>(handle.raw) && hit.GetNormal().z > 0.5f;
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
    if (!HasMappedObjectInstance(obj) || obj->m_nModelIndex != modelId ||
        !HasMappedModelResources(LiveModelInfo(modelId))) {
        LogUnreadyModel(modelId, "model or instance resources missing before CObject::Teleport");
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
