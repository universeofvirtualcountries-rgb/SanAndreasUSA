// USA 1:1 Procedural Expansion - mod entry point.
//
// This is the only file that knows how every subsystem fits together. Systems
// themselves are pure C++ (src/core) and are unit tested on the host; the
// adapter is the only part that talks to libGTASA.so.
#include <mod/amlmod.h>
#include <mod/logger.h>

#include "adapter/gtasa_adapter.hpp"
#include "core/chunk_generator.hpp"
#include "core/chunk_streamer.hpp"
#include "core/config.hpp"
#include "core/floating_origin.hpp"
#include "core/hash.hpp"
#include "core/interior.hpp"
#include "core/log.hpp"
#include "core/mission.hpp"
#include "core/model_catalog.hpp"
#include "core/regions.hpp"
#include "core/test_polygon.hpp"
#include "core/touch_gesture.hpp"
#include "core/world_delta.hpp"
#include "net/android_http.hpp"
#include "net/cloud_sync.hpp"
#include "ui/imgui_overlay.hpp"

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

MYMOD(usa1to1.procedural, USA1to1, 1.0.0, universeofvirtualcountries)

using namespace usa1to1;
using namespace usa1to1::core;

namespace {

class ModApp {
  public:
    static ModApp &Get() {
        static ModApp app;
        return app;
    }

    void OnLoad();
    void OnUnload();
    void OnTick(double dt);
    void OnTouch(int action, int track, int x, int y);

  private:
    void LoadConfig();
    void LoadCatalogue();
    void LoadMissions();
    void WireOverlayActions();
    void UpdateTelemetry();
    void AutosaveDelta(bool force);
    void TeleportToCity(const CityAnchor &city);
    void TogglePolygon();
    void StartMission(const std::string &id);
    void ApplyInitialSpawn();
    void SaveDelta();

    adapter::GTASAAdapter m_adapter;
    net::AndroidHttpTransport m_transport;
    std::unique_ptr<net::CloudSync> m_cloud;
    ui::ImGuiOverlay m_overlay;
    TwoFingerSwipeDetector m_gesture;

    ModelCatalog m_catalog;
    ModConfig m_config;
    std::unique_ptr<ChunkGenerator> m_generator;
    std::unique_ptr<ChunkStreamer> m_streamer;
    std::unique_ptr<InteriorGenerator> m_interiorGen;
    std::unique_ptr<InteriorManager> m_interiors;
    std::unique_ptr<TestPolygon> m_polygon;
    FloatingOrigin m_origin;
    WorldDelta m_delta;
    MissionManager m_missions;
    MissionRunner m_runner;

    std::string m_modDir;
    std::string m_deltaPath;
    std::string m_configPath;
    std::string m_missionsDir;
    std::string m_saveLabel;
    std::string m_cloudLabel = "not started";

    double m_lastCloudCheck = 0.0;
    double m_lastAutosave = 0.0;
    double m_sessionStart = 0.0;
    Vec3f m_lastPlayerPos;
    Vec2d m_lastGlobal;
    bool m_initialSpawnDone = false;
    bool m_streamingEnabled = true;
    size_t m_shiftsThisFrame = 0;
    bool m_deltaDirty = false;
    bool m_ready = false;
};

void ModApp::OnLoad() {
    logger->SetTag("USA1to1");
    Log::SetSink([](LogLevel level, const std::string &msg) {
        switch (level) {
        case LogLevel::Error:
            logger->Error("%s", msg.c_str());
            break;
        case LogLevel::Warn:
            logger->Print(LogP_Warn, "%s", msg.c_str());
            break;
        case LogLevel::Debug:
            logger->Print(LogP_Debug, "%s", msg.c_str());
            break;
        default:
            logger->Info("%s", msg.c_str());
            break;
        }
    });
    logger->Info("USA 1:1 Procedural Expansion loading...");

    if (!m_adapter.Initialize()) {
        logger->Error("adapter init failed: %s", m_adapter.LastError().c_str());
        return;
    }

    m_modDir = m_adapter.ModDirectory();
    m_configPath = m_modDir + "/config.json";
    m_deltaPath = m_modDir + "/world_delta.json";
    m_missionsDir = m_modDir + "/assets/missions";
    logger->Info("mod dir: %s", m_modDir.c_str());

    LoadConfig();
    LoadCatalogue();
    LoadMissions();

    m_delta = WorldDelta::Load(m_deltaPath);
    if (m_config.startingMoney > 0 && m_delta.money == 0)
        m_delta.money = m_config.startingMoney;

    FloatingOrigin::Config originCfg;
    originCfg.chunkSize = m_config.chunkSize;
    originCfg.gridRadius = m_config.gridRadius;
    originCfg.localLimit = m_config.localLimit;
    originCfg.safetyMargin = m_config.safetyMargin;
    m_origin = FloatingOrigin(originCfg);
    if (m_delta.hasPlayerPos)
        m_origin.SetOffset(Vec2d(m_delta.playerGlobalX, m_delta.playerGlobalY));

    m_generator = std::make_unique<ChunkGenerator>(m_catalog);

    ChunkStreamer::Config streamCfg;
    streamCfg.chunkSize = m_config.chunkSize;
    streamCfg.radius = m_config.gridRadius;
    streamCfg.maxChunksPerUpdate = m_config.maxChunksPerUpdate;
    streamCfg.maxSpawnsPerUpdate = m_config.maxSpawnsPerUpdate;
    streamCfg.spawnMarkers = m_config.doorMarkersEnabled;
    m_streamer = std::make_unique<ChunkStreamer>(m_adapter, *m_generator, streamCfg);

    m_interiorGen = std::make_unique<InteriorGenerator>(m_catalog);
    m_interiors = std::make_unique<InteriorManager>(m_adapter, *m_interiorGen);
    m_polygon = std::make_unique<TestPolygon>(m_adapter, m_catalog);
    m_streamingEnabled = m_config.streamingEnabled;

    m_overlay.SetMissions(&m_missions, &m_delta);
    if (!m_overlay.Initialize())
        logger->Print(LogP_Warn, "ImGui overlay failed to init - the mod still runs headless");
    m_overlay.SetVisible(m_config.showOverlayOnStart);
    WireOverlayActions();

    m_cloud = std::make_unique<net::CloudSync>(m_transport, m_missionsDir, m_modDir + "/cloud_state.json");
    m_cloud->SetManifestUrl(m_config.manifestUrl);
    m_cloud->SetMissionsBaseUrl(m_config.missionsBaseUrl);
    m_cloud->SetUpdatedCallback([this](const std::vector<std::string> &ids) {
        const int loaded = m_missions.LoadDirectory(m_missionsDir, nullptr);
        logger->Info("CloudSync: %zu updated missions, %d total", ids.size(), loaded);
        char buf[128];
        snprintf(buf, sizeof(buf), "CloudSync: %zu mission(s) updated", ids.size());
        m_cloudLabel = buf;
        if (!ids.empty())
            m_adapter.ShowToast(m_cloudLabel);
    });

    m_adapter.SetTickCallback([this](double dt) { OnTick(dt); });
    m_adapter.SetTouchCallback([this](int a, int t, int x, int y) { OnTouch(a, t, x, y); });
    m_adapter.SetShutdownCallback([this]() { OnUnload(); });

    m_sessionStart = m_adapter.NowSeconds();
    m_ready = true;
    logger->Info("USA 1:1 loaded: %zu models, %zu missions, grid %dx%d",
                 m_catalog.Size(), m_missions.Size(), m_config.gridRadius * 2 + 1, m_config.gridRadius * 2 + 1);
    if (m_config.autoStartPolygon)
        TogglePolygon();
}

void ModApp::OnUnload() {
    if (!m_ready)
        return;
    SaveDelta();
    if (m_polygon)
        m_polygon->Disable();
    if (m_streamer)
        m_streamer->Shutdown();
    if (m_interiors)
        m_interiors->Exit(m_delta);
    m_overlay.Shutdown();
    m_adapter.Shutdown();
    m_ready = false;
    logger->Info("USA 1:1 unloaded");
}

void ModApp::LoadConfig() {
    std::string err;
    if (m_config.LoadFile(m_configPath, &err)) {
        logger->Info("config loaded: %s", m_configPath.c_str());
    } else if (!err.empty()) {
        logger->Print(LogP_Warn, "config parse error (%s), defaults used", err.c_str());
    }
    m_config.SaveFile(m_configPath, nullptr);
}

void ModApp::LoadCatalogue() {
    const std::string dbPath = m_modDir + "/assets/objects_db.json";
    std::string err;
    if (m_catalog.LoadFromFile(dbPath, &err)) {
        logger->Info("objects_db.json: %zu models", m_catalog.Size());
    } else {
        logger->Info("no objects_db.json (%s), scanning live CModelInfo", err.c_str());
        m_adapter.BuildLiveCatalog(m_catalog);
        // Persist what we found so tools/parse_ide.py can enrich it later.
        if (m_catalog.Size() > 0) {
            std::error_code ec;
            std::filesystem::create_directories(m_modDir + "/assets", ec);
            FILE *f = fopen((m_modDir + "/assets/objects_db_live.json").c_str(), "wb");
            if (f) {
                const std::string text = m_catalog.ToJson().Dump(true);
                fwrite(text.data(), 1, text.size(), f);
                fclose(f);
            }
        }
    }
    if (m_catalog.Size() == 0)
        logger->Print(LogP_Warn, "catalogue is empty: chunks will spawn no props (markers only)");
}

void ModApp::LoadMissions() {
    std::string err;
    const int n = m_missions.LoadDirectory(m_missionsDir, &err);
    if (n == 0)
        logger->Print(LogP_Warn, "no missions loaded (%s)", err.empty() ? "fine, cloud sync may fill them" : err.c_str());
}

void ModApp::WireOverlayActions() {
    ui::OverlayActions actions;
    actions.teleportToCity = [this](const CityAnchor &city) { TeleportToCity(city); };
    actions.togglePolygon = [this]() { TogglePolygon(); };
    actions.startMission = [this](const std::string &id) { StartMission(id); };
    actions.abortMission = [this]() {
        if (m_runner.IsActive()) {
            m_runner.Abort(m_delta);
            m_deltaDirty = true;
            m_adapter.ShowToast("Mission aborted");
        }
    };
    actions.syncNow = [this]() {
        if (!m_cloud)
            return;
        m_cloudLabel = "syncing...";
        m_cloud->RequestAsync();
    };
    actions.saveDelta = [this]() { SaveDelta(); };
    actions.reloadMissions = [this]() {
        m_missions.Clear();
        LoadMissions();
        m_overlay.SetMissions(&m_missions, &m_delta);
    };
    actions.toggleStreaming = [this]() {
        m_streamingEnabled = !m_streamingEnabled;
        if (!m_streamingEnabled && m_streamer) {
            m_streamer->Shutdown();
        }
        m_adapter.ShowToast(m_streamingEnabled ? "Streaming ON" : "Streaming OFF");
    };
    m_overlay.SetActions(std::move(actions));
}

void ModApp::ApplyInitialSpawn() {
    if (m_initialSpawnDone)
        return;
    Vec3f local;
    if (!m_adapter.GetPlayerPosition(local))
        return;
    m_initialSpawnDone = true;

    if (!m_delta.hasPlayerPos && m_config.teleportOnFirstLoad) {
        const CityAnchor *city = FindCity(m_config.spawnCity);
        if (!city)
            city = FindCity("Los Angeles");
        if (city) {
            m_origin.TeleportPlayer(m_adapter, Vec2d(city->x, city->y));
            m_adapter.ShowToast(("USA 1:1: " + std::string(city->name)).c_str());
            logger->Info("initial spawn at %s", city->name);
        }
    } else {
        logger->Info("resuming at global (%.1f, %.1f)", m_origin.Offset().x, m_origin.Offset().y);
    }
}

void ModApp::OnTick(double dt) {
    if (!m_ready)
        return;
    if (!m_adapter.IsGameReady())
        return;
    if (!m_initialSpawnDone)
        ApplyInitialSpawn();

    Vec3f local;
    if (!m_adapter.GetPlayerPosition(local))
        return;

    // --- interior pocket: main world logic is suspended while inside --------
    if (m_interiors->IsInside()) {
        const auto ev = m_interiors->Update(local, m_delta);
        if (ev == InteriorManager::Event::Robbed) {
            const int64_t reward = m_interiors->TakeRobberyReward();
            char buf[64];
            snprintf(buf, sizeof(buf), "Cash register: +$%lld", static_cast<long long>(reward));
            m_adapter.ShowToast(buf);
            m_runner.NotifyDoorRobbed(m_interiors->CurrentDoorSeed());
            m_deltaDirty = true;
        } else if (ev == InteriorManager::Event::Exited) {
            m_adapter.ShowToast("Back outside");
            m_deltaDirty = true;
        }
        UpdateTelemetry();
        return;
    }

    // --- floating origin ----------------------------------------------------
    m_shiftsThisFrame = 0;
    const auto shift = m_origin.Update(m_adapter);
    if (shift.shifted) {
        if (!shift.invariantHeld)
            logger->Print(LogP_Warn, "floating origin invariant violated");
        m_streamer->OnOriginShift(shift.delta);
        m_shiftsThisFrame = 1;
        m_deltaDirty = true;
    }

    const Vec2d global = m_origin.ToGlobal(local);
    m_lastGlobal = global;
    m_lastPlayerPos = local;

    // --- streaming ----------------------------------------------------------
    if (m_streamingEnabled) {
        m_streamer->Update(global, m_origin.Offset());

        // Doors: standing on a yellow marker opens the deterministic interior.
        if (m_config.interiorsEnabled) {
            const auto hit = m_streamer->FindDoorAt(local, m_config.doorTriggerRadius);
            if (hit.valid) {
                if (m_interiors->Enter(hit.door.seed, hit.doorLocalPos, m_delta)) {
                    m_adapter.ShowToast(hit.door.shop ? "Entering shop..." : "Entering building...");
                    m_deltaDirty = true;
                }
            }
        }
    }

    // --- test polygon -------------------------------------------------------
    if (m_polygon->Active())
        m_polygon->Update(global, m_origin.Offset());

    // --- missions -----------------------------------------------------------
    if (m_runner.IsActive()) {
        const auto result = m_runner.Update(global, m_delta, m_adapter.NowSeconds());
        if (result == MissionRunner::Result::MissionCompleted) {
            m_adapter.ShowToast("Mission complete");
            m_deltaDirty = true;
        } else if (result == MissionRunner::Result::StepCompleted) {
            m_adapter.ShowToast("Objective complete");
            m_deltaDirty = true;
        }
    }

    // --- cloud sync ---------------------------------------------------------
    if (m_config.cloudSyncEnabled && m_cloud) {
        const double now = m_adapter.NowSeconds();
        if (now - m_lastCloudCheck > m_config.cloudSyncIntervalSec || m_lastCloudCheck == 0.0) {
            m_lastCloudCheck = now;
            m_cloudLabel = "checking...";
            m_cloud->RequestAsync();
        }
        m_cloud->Poll();
        if (m_cloud->CurrentState() == net::CloudSync::State::Error && !m_cloud->LastError().empty())
            m_cloudLabel = "error: " + m_cloud->LastError();
    }

    AutosaveDelta(false);
    UpdateTelemetry();
}

void ModApp::OnTouch(int action, int track, int x, int y) {
    const double now = m_adapter.NowSeconds();
    if (m_gesture.Feed(action, track, x, y, now)) {
        m_overlay.Toggle();
        logger->Info("overlay %s", m_overlay.Visible() ? "opened" : "closed");
    }
}

void ModApp::UpdateTelemetry() {
    Vec3f local;
    m_adapter.GetPlayerPosition(local);

    ui::OverlayModel model;
    auto &t = model.telemetry;
    t.localPos = {local.x, local.y};
    t.localZ = local.z;
    t.offsetX = m_origin.Offset().x;
    t.offsetY = m_origin.Offset().y;
    t.globalX = m_origin.Offset().x + local.x;
    t.globalY = m_origin.Offset().y + local.y;
    t.fps = m_adapter.GetFps();
    t.frameShifts = m_shiftsThisFrame;
    t.totalShifts = m_origin.TotalShifts();
    t.localBudget = m_origin.RemainingBudget(local.x) < m_origin.RemainingBudget(local.y)
                        ? m_origin.RemainingBudget(local.x)
                        : m_origin.RemainingBudget(local.y);
    t.polygonActive = m_polygon->Active();

    const ChunkCoord chunk = ChunkGenerator::ChunkOf(Vec2d(t.globalX, t.globalY));
    t.chunkX = chunk.x;
    t.chunkY = chunk.y;
    t.chunkSeed = ChunkSeed(chunk.x, chunk.y);
    t.activeChunks = m_streamer->ActiveChunkCount();
    t.activeObjects = static_cast<int>(m_streamer->ObjectCount());
    t.streamedEntities = m_adapter.CountStreamedEntities();
    t.playerInterior = m_interiors->CurrentArea();

    model.regionLabel = "unknown";
    if (const RegionBox *region = FindRegion(t.globalX, t.globalY)) {
        model.regionLabel = std::string(region->name) + " (" + region->code + ")";
        model.sectorLabel = SectorLabel(*region, t.globalX, t.globalY);
    }
    model.cityLabel = NearestCityLabel(t.globalX, t.globalY);
    model.insideInterior = m_interiors->IsInside();
    if (model.insideInterior) {
        char buf[96];
        snprintf(buf, sizeof(buf), "area %d, %zu rooms, door %016llX", m_interiors->CurrentArea(),
                 m_interiors->Current().RoomCount(),
                 static_cast<unsigned long long>(m_interiors->CurrentDoorSeed()));
        model.interiorLabel = buf;
    }
    if (const MissionStep *step = m_runner.CurrentStep()) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s - %s", m_runner.Active()->title.c_str(), step->text.c_str());
        model.missionLabel = buf;
    }
    model.money = m_delta.money;
    model.missionsTotal = static_cast<int>(m_missions.Size());
    int completed = 0;
    for (const auto &m : m_missions.All()) {
        if (const MissionState *st = m_delta.FindMission(m.id))
            if (st->status == "completed")
                ++completed;
    }
    model.missionsCompleted = completed;
    model.cloudLabel = m_cloudLabel;
    model.saveLabel = m_saveLabel;
    model.polygonActive = m_polygon->Active();
    model.streamingEnabled = m_streamingEnabled;
    model.ownedObjects = m_adapter.OwnedObjectCount();
    m_overlay.SetModel(model);
}

void ModApp::AutosaveDelta(bool force) {
    const double now = m_adapter.NowSeconds();
    const double sinceSave = now - m_lastAutosave;
    if (force) {
        m_lastAutosave = now;
        SaveDelta();
        return;
    }
    // Write at most every 5 s, and only when something actually changed.
    if (m_deltaDirty && sinceSave >= 5.0) {
        m_lastAutosave = now;
        SaveDelta();
    }
}

void ModApp::SaveDelta() {
    Vec3f local;
    if (m_adapter.GetPlayerPosition(local) && !m_interiors->IsInside()) {
        const Vec2d global = m_origin.ToGlobal(local);
        m_delta.SetPlayerPosition(global.x, global.y, local.z);
    }
    std::string err;
    if (m_delta.SaveToFile(m_deltaPath, &err)) {
        m_deltaDirty = false;
        m_saveLabel = "world_delta.json saved";
    } else {
        m_saveLabel = "save failed: " + err;
        logger->Print(LogP_Warn, "delta save failed: %s", err.c_str());
    }
}

void ModApp::TeleportToCity(const CityAnchor &city) {
    if (m_interiors->IsInside())
        m_interiors->Exit(m_delta);
    m_origin.TeleportPlayer(m_adapter, Vec2d(city.x, city.y));
    // Everything the old area spawned is now thousands of kilometres away.
    m_streamer->Shutdown();
    if (m_polygon->Active()) {
        Vec3f local;
        m_adapter.GetPlayerPosition(local);
        m_polygon->Enable(m_origin.ToGlobal(local), m_origin.Offset());
    }
    m_adapter.ShowToast(("Teleported to " + std::string(city.name)).c_str());
    m_deltaDirty = true;
    logger->Info("teleport -> %s (global %.0f, %.0f)", city.name, city.x, city.y);
}

void ModApp::TogglePolygon() {
    if (m_polygon->Active()) {
        m_polygon->Disable();
        m_adapter.ShowToast("Test polygon OFF");
        return;
    }
    Vec3f local;
    if (!m_adapter.GetPlayerPosition(local))
        return;
    m_polygon->Enable(m_origin.ToGlobal(local), m_origin.Offset());
    m_adapter.ShowToast(m_polygon->HasVisuals() ? "Test polygon ON" : "Test polygon ON (markers only)");
}

void ModApp::StartMission(const std::string &id) {
    const MissionDef *def = m_missions.Find(id);
    if (!def) {
        m_adapter.ShowToast("Mission not found");
        return;
    }
    if (!m_runner.Start(*def, m_delta)) {
        m_adapter.ShowToast("Cannot start this mission");
        return;
    }
    m_deltaDirty = true;
    m_adapter.ShowToast(("Started: " + def->title).c_str());
}

} // namespace

ON_MOD_LOAD() { ModApp::Get().OnLoad(); }

ON_MOD_UNLOAD() { ModApp::Get().OnUnload(); }
