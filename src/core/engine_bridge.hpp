// The single seam between pure logic (testable on a host) and libGTASA.so.
// The real implementation lives in src/adapter/gtasa_adapter.cpp; tests use a
// mock that records every call, so the floating-origin dance is verified
// without a phone in the loop.
#pragma once

#include "types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace usa1to1 {
namespace core {

struct ObjectHandle {
    uintptr_t raw = 0;
    explicit operator bool() const { return raw != 0; }
    bool operator==(const ObjectHandle &o) const { return raw == o.raw; }
    bool operator!=(const ObjectHandle &o) const { return raw != o.raw; }
};

struct SpawnParams {
    int modelId = -1;
    Vec3f pos;
    float headingRad = 0.0f;
    bool dynamic = false;   // true -> game processes physics/AI for it
    bool interior = false;  // lives in interior space (z >= 1000), never shifted with the map
    bool collision = true;
    uint64_t ownerTag = 0;  // chunk seed / interior id, for debugging and bulk cleanup
    bool roadSlab = false;  // test polygon: requires a verified road collider
};

// Everything the overlay shows and the debug log prints.
struct Telemetry {
    double globalX = 0.0;
    double globalY = 0.0;
    float localZ = 0.0f;
    double offsetX = 0.0;
    double offsetY = 0.0;
    Vec2d localPos;
    uint64_t chunkSeed = 0;
    int64_t chunkX = 0;
    int64_t chunkY = 0;
    int activeChunks = 0;
    int activeObjects = 0;
    int streamedEntities = 0;
    int playerInterior = 0;
    float fps = 0.0f;
    size_t frameShifts = 0;
    size_t totalShifts = 0;
    double localBudget = 0.0; // how much room is left before the next shift
    bool polygonActive = false;
};

class IEngineBridge {
  public:
    virtual ~IEngineBridge() = default;

    // --- lifecycle -----------------------------------------------------------
    virtual bool IsGameReady() = 0;

    // --- player --------------------------------------------------------------
    // Position in RenderWare local space (metres, origin = floating origin).
    virtual bool GetPlayerPosition(Vec3f &out) = 0;
    virtual void SetPlayerPosition(const Vec3f &pos) = 0;
    // Safety warp: implementations may clear downward speed and move a driven vehicle too.
    virtual void RecoverPlayerPosition(const Vec3f &pos) { SetPlayerPosition(pos); }
    // Pin a waiting player and stop linear motion until their floor is ready.
    virtual void HoldPlayerPosition(const Vec3f &pos) { SetPlayerPosition(pos); }
    virtual bool IsPlayerInVehicle() = 0;
    virtual float GetFps() = 0;

    // --- floating origin primitives -----------------------------------------
    // Each returns the number of entities actually moved (-1 on failure).
    // Implementations MUST NOT touch velocities: only positions/matrices move.
    virtual int ShiftPlayer(const Vec3f &delta) = 0;
    virtual int ShiftPlayerVehicle(const Vec3f &delta) = 0;
    virtual int ShiftCamera(const Vec3f &delta) = 0;
    virtual int ShiftStreamedEntities(const Vec3f &delta, bool includeInterior) = 0;
    virtual int ShiftWorldPeds(const Vec3f &delta) = 0;
    virtual int ShiftWorldVehicles(const Vec3f &delta) = 0;

    // --- streaming -----------------------------------------------------------
    virtual bool RequestModel(int modelId, bool dynamic) = 0;
    // True when streaming and the live render/collision resources are ready for spawning.
    virtual bool IsModelLoaded(int modelId) = 0;
    virtual void ReleaseModel(int modelId) = 0;
    // Blocking "load everything requested now". Only used at loading moments
    // (entering a door pocket, starting the test polygon), never per chunk.
    virtual void FlushModelStreaming() = 0;

    // --- mod owned objects ---------------------------------------------------
    virtual ObjectHandle CreateObject(const SpawnParams &params) = 0;
    virtual bool IsObjectCollisionReady(ObjectHandle handle) { return static_cast<bool>(handle); }
    virtual bool HasRoadSupportAt(ObjectHandle handle, const Vec3f &) { return IsObjectCollisionReady(handle); }
    virtual void DestroyObject(ObjectHandle handle) = 0;
    virtual void MoveObject(ObjectHandle handle, const Vec3f &pos) = 0;
    virtual size_t OwnedObjectCount() const = 0;

    // --- markers -------------------------------------------------------------
    // interior markers live in the z>=1000 pocket and are never shifted with the world.
    virtual void PlaceMarker(uint64_t markerId, const Vec3f &pos, float radius, bool interior = false) = 0;
    virtual void RemoveMarker(uint64_t markerId) = 0;

    // --- interiors -----------------------------------------------------------
    // Interior space is a pocket at z >= 1000; areaId maps to CGame::currArea.
    virtual void SetPlayerInterior(int areaId) = 0;

    // --- misc ----------------------------------------------------------------
    virtual void ShowToast(const std::string &msg) = 0;
    virtual void Vibrate(int ms) { (void)ms; }
    virtual double NowSeconds() = 0;
    virtual std::string ModDirectory() = 0;
    // Scans CModelInfo for a flat, collision-carrying road-ish atomic model.
    // Returns -1 when the game image has nothing suitable.
    virtual int ProbeRoadModel() = 0;
    // Number of entities currently inside the game's ped+vehicle+object pools.
    virtual int CountStreamedEntities() = 0;
};

} // namespace core
} // namespace usa1to1
