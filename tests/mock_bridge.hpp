// Host-side IEngineBridge implementation used by the unit tests.
// It records every call so the floating-origin and streaming logic can be
// verified without a phone, a game image, or a running RenderWare.
#pragma once

#include "core/engine_bridge.hpp"

#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace usa1to1 {
namespace test {

class MockBridge : public core::IEngineBridge {
  public:
    struct ObjectRec {
        int modelId = -1;
        core::Vec3f pos;
        bool interior = false;
        float heading = 0.0f;
        bool collision = false;
    };
    struct MarkerRec {
        core::Vec3f pos;
        float radius = 1.0f;
        bool interior = false;
    };
    struct MovingEntity {
        core::Vec3f pos;
        core::Vec3f velocity;
    };

    // --- player -------------------------------------------------------------
    core::Vec3f player{0.0f, 0.0f, 40.0f};
    core::Vec3f playerVelocity{1.25f, -3.5f, 0.0f};
    bool playerInVehicle = false;
    core::Vec3f vehiclePos;
    core::Vec3f vehicleVelocity{0.0f, 10.0f, 0.0f};
    core::Vec3f cameraPos{0.0f, -8.0f, 45.0f};
    int playerInterior = 0;

    // --- game state ---------------------------------------------------------
    std::set<int> loadedModels;
    std::set<int> requestedModels;
    std::map<uintptr_t, ObjectRec> objects;
    std::map<uint64_t, MarkerRec> markers;
    std::map<uintptr_t, MovingEntity> peds;
    std::map<uintptr_t, MovingEntity> vehicles;
    uintptr_t nextHandle = 0x10000;
    int roadModel = 5000;
    double now = 0.0;
    float fps = 60.0f;
    std::string modDir = ".";

    // --- call counters ------------------------------------------------------
    int shiftPlayerCalls = 0;
    int shiftVehicleCalls = 0;
    int shiftCameraCalls = 0;
    int shiftStreamedCalls = 0;
    int shiftPedsCalls = 0;
    int shiftVehiclesCalls = 0;
    int created = 0;
    int destroyed = 0;
    int markersPlaced = 0;
    int markersRemoved = 0;
    int interiorChanges = 0;
    int toasts = 0;
    int requests = 0;
    int releases = 0;
    int flushes = 0;

    // --- IEngineBridge ------------------------------------------------------
    bool IsGameReady() override { return true; }

    bool GetPlayerPosition(core::Vec3f &out) override {
        out = player;
        return true;
    }
    void SetPlayerPosition(const core::Vec3f &pos) override { player = pos; }
    bool IsPlayerInVehicle() override { return playerInVehicle; }
    float GetFps() override { return fps; }

    int ShiftPlayer(const core::Vec3f &delta) override {
        ++shiftPlayerCalls;
        player += delta;
        return 1; // velocity stays untouched on purpose, like the real adapter
    }
    int ShiftPlayerVehicle(const core::Vec3f &delta) override {
        ++shiftVehicleCalls;
        if (!playerInVehicle)
            return 0;
        vehiclePos += delta;
        return 1;
    }
    int ShiftCamera(const core::Vec3f &delta) override {
        ++shiftCameraCalls;
        cameraPos += delta;
        return 1;
    }
    int ShiftStreamedEntities(const core::Vec3f &delta, bool includeInterior) override {
        ++shiftStreamedCalls;
        int moved = 0;
        for (auto &kv : objects) {
            if (kv.second.interior && !includeInterior)
                continue;
            kv.second.pos += delta;
            ++moved;
        }
        for (auto &kv : markers) {
            if (kv.second.interior && !includeInterior)
                continue;
            kv.second.pos += delta;
        }
        return moved;
    }
    int ShiftWorldPeds(const core::Vec3f &delta) override {
        ++shiftPedsCalls;
        int moved = 0;
        for (auto &kv : peds) {
            kv.second.pos += delta;
            ++moved;
        }
        return moved;
    }
    int ShiftWorldVehicles(const core::Vec3f &delta) override {
        ++shiftVehiclesCalls;
        int moved = 0;
        for (auto &kv : vehicles) {
            kv.second.pos += delta;
            ++moved;
        }
        return moved;
    }

    bool RequestModel(int modelId, bool) override {
        ++requests;
        requestedModels.insert(modelId);
        loadedModels.insert(modelId); // instant load in tests
        return true;
    }
    bool IsModelLoaded(int modelId) override { return loadedModels.count(modelId) > 0; }
    void ReleaseModel(int modelId) override {
        ++releases;
        loadedModels.erase(modelId);
    }
    void FlushModelStreaming() override {
        ++flushes;
        for (int id : requestedModels)
            loadedModels.insert(id);
    }

    core::ObjectHandle CreateObject(const core::SpawnParams &params) override {
        ObjectRec rec;
        rec.modelId = params.modelId;
        rec.pos = params.pos;
        rec.interior = params.interior;
        rec.heading = params.headingRad;
        rec.collision = params.collision;
        const uintptr_t handle = nextHandle++;
        objects[handle] = rec;
        ++created;
        return core::ObjectHandle{handle};
    }
    void DestroyObject(core::ObjectHandle handle) override {
        if (objects.erase(handle.raw))
            ++destroyed;
    }
    void MoveObject(core::ObjectHandle handle, const core::Vec3f &pos) override {
        auto it = objects.find(handle.raw);
        if (it != objects.end())
            it->second.pos = pos;
    }
    size_t OwnedObjectCount() const override { return objects.size(); }

    void PlaceMarker(uint64_t markerId, const core::Vec3f &pos, float radius, bool interior) override {
        markers[markerId] = MarkerRec{pos, radius, interior};
        ++markersPlaced;
    }
    void RemoveMarker(uint64_t markerId) override {
        if (markers.erase(markerId))
            ++markersRemoved;
    }

    void SetPlayerInterior(int areaId) override {
        playerInterior = areaId;
        ++interiorChanges;
    }

    void ShowToast(const std::string &) override { ++toasts; }
    double NowSeconds() override { return now; }
    std::string ModDirectory() override { return modDir; }
    int ProbeRoadModel() override { return roadModel; }
    int CountStreamedEntities() override {
        return static_cast<int>(peds.size() + vehicles.size() + objects.size());
    }

    // --- helpers ------------------------------------------------------------
    void Advance(double seconds) { now += seconds; }
    size_t CountInteriorObjects() const {
        size_t n = 0;
        for (const auto &kv : objects)
            if (kv.second.interior)
                ++n;
        return n;
    }
    size_t CountWorldObjects() const { return objects.size() - CountInteriorObjects(); }
};

} // namespace test
} // namespace usa1to1
