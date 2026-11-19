#include "gtasa_adapter.hpp"

#include "core/log.hpp"

#include <cfloat> // FLT_EPSILON, used by plugin-sdk/gta_base/Vector.h

#include <plugin-sdk/game_sa/Events.h>
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

#include <mod/amlmod.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <sys/stat.h>

using namespace usa1to1::core;

namespace usa1to1 {
namespace adapter {
namespace {

constexpr int kMaxModelIds = 26316;   // hard limit of the mobile streaming arrays
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
    if (g_adapter)
        g_adapter->OnTouch(action, track, x, y);
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

bool GTASAAdapter::Initialize() {
    if (m_ready)
        return true;

    if (!aml) {
        m_lastError = "AML interface is null (mod not loaded by AML?)";
        return false;
    }

    m_libHandle = aml->GetLibHandle("libGTASA.so");
    if (!m_libHandle) {
        m_lastError = "libGTASA.so not found in the process";
        USA_LOG_E("%s", m_lastError.c_str());
        return false;
    }

    m_versionLabel = DetectVersion();
    USA_LOG_I("Adapter: libGTASA.so @ %p, version %s", m_libHandle, m_versionLabel.c_str());

    InstallHooks();
    if (!m_hooksInstalled) {
        m_lastError = "failed to hook the game loop";
        return false;
    }

    // Probe the road model once the game image is up; the streaming pool is not
    // ready that early, so we only record the id here.
    ProbeRoadModel();

    g_adapter = this;
    m_ready = true;
    m_lastProcessTime = 0.0;
    USA_LOG_I("Adapter: ready");
    return true;
}

void GTASAAdapter::InstallHooks() {
    if (m_hooksInstalled)
        return;

    auto hookSym = [this](const char *sym, void *replacement, void **original, const char *label) -> bool {
        void *addr = reinterpret_cast<void *>(aml->GetSym(m_libHandle, sym));
        if (!addr) {
            USA_LOG_W("Adapter: symbol %s (%s) not exported by this build", sym, label);
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
    g_adapter = nullptr;
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------

void GTASAAdapter::OnGameProcess() {
    const uint32_t nowMs = CTimer::GetTimeInMS();
    double dt = 0.0;
    if (m_lastFrameMs != 0 && nowMs >= m_lastFrameMs)
        dt = static_cast<double>(nowMs - m_lastFrameMs) / 1000.0;
    m_lastFrameMs = nowMs;
    m_now = static_cast<double>(nowMs) / 1000.0;

    if (dt > 0.0005 && dt < 0.5) {
        const float instant = static_cast<float>(1.0 / dt);
        m_fps = m_fps <= 0.0f ? instant : m_fps * 0.9f + instant * 0.1f;
    }

    if (m_onTick && dt > 0.0 && dt < 1.0)
        m_onTick(dt);
}

void GTASAAdapter::OnTouch(int action, int track, int x, int y) {
    if (m_onTouch)
        m_onTouch(action, track, x, y);
}

void GTASAAdapter::OnGameShutdown() {
    if (m_onShutdown)
        m_onShutdown();
}

// ---------------------------------------------------------------------------
// Player / vehicle / camera
// ---------------------------------------------------------------------------

bool GTASAAdapter::IsGameReady() {
    // CStreaming is initialised once the world is live; before that the player
    // does not exist yet and any shift would be garbage.
    return m_ready && FindPlayerPed(0) != nullptr && CStreaming::IsInitialised();
}

bool GTASAAdapter::GetPlayerPosition(Vec3f &out) {
    CPlayerPed *ped = FindPlayerPed(0);
    if (!ped)
        return false;
    out = ToVec3(ped->GetPosition());
    return true;
}

void GTASAAdapter::SetPlayerPosition(const Vec3f &pos) {
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

bool GTASAAdapter::IsPlayerInVehicle() { return FindPlayerVehicle(0, false) != nullptr; }

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
        CEntity *ent = reinterpret_cast<CEntity *>(obj.pointer);
        if (!ent)
            continue;
        const CVector pos = ent->GetPosition();
        ent->Teleport(CVector(pos.x + d.x, pos.y + d.y, pos.z + d.z), false);
        ent->UpdateRwFrame();
        obj.pos.x += delta.x;
        obj.pos.y += delta.y;
        obj.pos.z += delta.z;
        ++moved;
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
            o->Teleport(CVector(pos.x + d.x, pos.y + d.y, pos.z + d.z), false);
            o->UpdateRwFrame();
            ++moved;
        }
    }
    return moved;
}

// ---------------------------------------------------------------------------
// Streaming
// ---------------------------------------------------------------------------

bool GTASAAdapter::RequestModel(int modelId, bool dynamic) {
    (void)dynamic;
    if (modelId < 0 || modelId >= kMaxModelIds)
        return false;
    if (!CStreaming::IsInitialised())
        return false;
    CStreaming::RequestModel(modelId, 0);
    return true;
}

bool GTASAAdapter::IsModelLoaded(int modelId) {
    if (modelId < 0 || modelId >= kMaxModelIds)
        return false;
    if (!CStreaming::IsInitialised())
        return false;
    return CStreaming::ms_aInfoForModel[modelId].m_status == 1;
}

void GTASAAdapter::ReleaseModel(int modelId) {
    if (modelId < 0 || modelId >= kMaxModelIds || !CStreaming::IsInitialised())
        return;
    CStreaming::SetModelIsDeletable(modelId);
}

void GTASAAdapter::FlushModelStreaming() {
    if (!CStreaming::IsInitialised())
        return;
    // Blocking by design: called on a door transition, never from the chunk loop.
    CStreaming::LoadAllRequestedModels(false);
}

// ---------------------------------------------------------------------------
// Objects
// ---------------------------------------------------------------------------

ObjectHandle GTASAAdapter::CreateObject(const SpawnParams &params) {
    if (params.modelId < 0 || params.modelId >= kMaxModelIds || !CStreaming::IsInitialised())
        return {};
    if (!IsModelLoaded(params.modelId))
        return {};

    CObject *obj = CObject::Create(params.modelId, true);
    if (!obj)
        return {};

    obj->bUsesCollision = params.collision ? 1 : 0;

    CWorld::Add(obj);

    // Move it into place and orient it. Teleport keeps the rotation, so we set
    // the Z rotation afterwards and re-sync the RenderWare matrix.
    const CVector target = ToCV(params.pos);
    obj->Teleport(target, false);
    CMatrix *mtx = obj->GetMatrix();
    if (mtx) {
        mtx->SetRotateZOnly(params.headingRad);
        mtx->GetPosition() = target;
    }
    obj->UpdateRwFrame();

    OwnedObject owned;
    owned.pointer = reinterpret_cast<uintptr_t>(obj);
    owned.modelId = params.modelId;
    owned.interior = params.interior;
    owned.pos = params.pos;
    m_owned[owned.pointer] = owned;
    return ObjectHandle{owned.pointer};
}

void GTASAAdapter::DestroyObject(ObjectHandle handle) {
    if (!handle.raw)
        return;
    auto it = m_owned.find(handle.raw);
    if (it == m_owned.end())
        return;

    CObject *obj = reinterpret_cast<CObject *>(handle.raw);
    const int modelId = it->second.modelId;
    m_owned.erase(it);

    if (!CPools::ms_pObjectPool || !CPools::ms_pObjectPool->IsObjectValid(obj)) {
        return; // the game already reclaimed the slot (restart, interior change, ...)
    }

    CWorld::Remove(obj);
    // Native teardown order used by the game itself: remove from the world,
    // run the destructor through the pipeline shim, then free the pool slot.
    obj->~CObject();
    CPools::ms_pObjectPool->Delete(obj);

    if (modelId >= 0)
        CStreaming::SetModelIsDeletable(modelId);
}

void GTASAAdapter::MoveObject(ObjectHandle handle, const Vec3f &pos) {
    auto it = m_owned.find(handle.raw);
    if (it == m_owned.end())
        return;
    CEntity *ent = reinterpret_cast<CEntity *>(handle.raw);
    ent->Teleport(ToCV(pos), false);
    ent->UpdateRwFrame();
    it->second.pos = pos;
}

// ---------------------------------------------------------------------------
// Markers
// ---------------------------------------------------------------------------

void GTASAAdapter::PlaceMarker(uint64_t markerId, const Vec3f &pos, float radius, bool interior) {
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

void GTASAAdapter::ShowToast(const std::string &msg) {
    if (aml)
        aml->ShowToast(false, "%s", msg.c_str());
}

void GTASAAdapter::Vibrate(int ms) {
    if (aml && ms > 0)
        aml->DoVibro(ms);
}

double GTASAAdapter::NowSeconds() {
    if (!CStreaming::IsInitialised())
        return 0.0;
    return static_cast<double>(CTimer::GetTimeInMS()) / 1000.0;
}

std::string GTASAAdapter::ModDirectory() {
    // PreferAML's per-game mods path: /sdcard/AMLMods/<game>/USA1to1
    if (aml) {
        const char *base = aml->GetInternalModsPath();
        if (base && *base) {
            std::string dir = std::string(base) + "/USA1to1";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            return dir;
        }
        const char *data = aml->GetAndroidDataPath();
        if (data && *data) {
            std::string dir = std::string(data) + "/USA1to1";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            return dir;
        }
    }
    return ".";
}

int GTASAAdapter::ProbeRoadModel() {
    if (m_roadModel != -2)
        return m_roadModel;
    m_roadModel = -1;

    if (!CModelInfo::ms_modelInfoPtrs) {
        USA_LOG_W("Adapter: CModelInfo not resolved, road model unknown");
        return m_roadModel;
    }

    // 1) Trust the game's own road flag on atomic models.
    for (int i = 612; i < kMaxModelIds; ++i) {
        CBaseModelInfo *info = CModelInfo::ms_modelInfoPtrs[i];
        if (!info || !info->m_szModelName[0])
            continue;
        if (info->bIsLod || !info->m_pColModel)
            continue;
        if (CModelInfo::IsVehicleModelType(i))
            continue;
        if (!info->bIsRoad)
            continue;
        if (info->m_fDrawDistance < 30.0f)
            continue;
        m_roadModel = i;
        USA_LOG_I("Adapter: road model from bIsRoad flag = %d (%s)", i, info->m_szModelName);
        return m_roadModel;
    }

    // 2) Fall back to name heuristics among flat, collidable atomics.
    for (int i = 612; i < kMaxModelIds; ++i) {
        CBaseModelInfo *info = CModelInfo::ms_modelInfoPtrs[i];
        if (!info || !info->m_szModelName[0] || info->bIsLod || !info->m_pColModel)
            continue;
        if (CModelInfo::IsVehicleModelType(i))
            continue;
        const std::string name = LowerCopy(info->m_szModelName);
        if (name.find("road") == std::string::npos && name.find("pave") == std::string::npos &&
            name.find("asphalt") == std::string::npos)
            continue;
        m_roadModel = i;
        USA_LOG_I("Adapter: road model by name = %d (%s)", i, info->m_szModelName);
        return m_roadModel;
    }

    USA_LOG_W("Adapter: no road model found; test polygon will be marker-only");
    return m_roadModel;
}

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

int GTASAAdapter::BuildLiveCatalog(ModelCatalog &out) {
    if (!CModelInfo::ms_modelInfoPtrs) {
        USA_LOG_W("Adapter: cannot scan models, CModelInfo unavailable");
        return 0;
    }

    int added = 0;
    for (int i = 400; i < kMaxModelIds; ++i) {
        CBaseModelInfo *info = CModelInfo::ms_modelInfoPtrs[i];
        if (!info || !info->m_szModelName[0])
            continue;
        if (info->bIsLod)
            continue;
        if (CModelInfo::IsVehicleModelType(i))
            continue;

        const std::string name = LowerCopy(info->m_szModelName);
        if (name.empty() || name[0] == '#')
            continue;
        // Skip pure LOD-ish garbage and dummy entries without any draw distance.
        if (info->m_fDrawDistance <= 0.0f)
            continue;

        ModelProto proto;
        proto.id = i;
        proto.name = info->m_szModelName;
        proto.drawDistance = info->m_fDrawDistance;
        proto.hasCollision = info->m_pColModel != nullptr;
        proto.isRoad = info->bIsRoad;

        float radius = 1.0f;
        Vec3f offset;
        if (info->m_pColModel) {
            radius = info->m_pColModel->m_boundSphere.m_fRadius;
            offset = ToVec3(info->m_pColModel->m_boundSphere.m_vecCentre);
        }
        proto.colHalfExtents = {radius, radius, radius};
        proto.colOffset = offset;
        proto.tag = ClassifyByName(name, info->bIsRoad, radius);

        // Buildings that cannot hold a floor are useless for city blocks.
        if (proto.tag == PropTag::Building && radius < 2.0f)
            proto.tag = PropTag::Prop;
        if (proto.tag == PropTag::Unknown)
            proto.tag = PropTag::Prop;

        out.Add(proto);
        ++added;
    }

    USA_LOG_I("Adapter: live catalogue built, %d models", added);
    return added;
}

} // namespace adapter
} // namespace usa1to1
