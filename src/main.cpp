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

#include <plugin-sdk/renderware/imgui/imgui_impl_renderware.h>

#include <atomic>
#include <chrono>
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
    void OnAllModsLoaded();
    void OnUnload();
    void OnTick(double dt);
    bool OnTouch(int action, int track, int x, int y);
    void OnGameCrash(const char *library, int sig, int code, uintptr_t libaddr);

  private:
    bool EnsureLoaderStage();
    void BootRuntime();
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
    RawMenuGesture m_menuGesture;
    std::atomic<bool> m_menuTogglePending{false};

    ModelCatalog m_catalog;
    StatusFeed m_roadStatus;
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
    bool m_loaderStage = false;
    bool m_runtimeReady = false;
    int m_bootAttempts = 0;
    bool m_streamingEnabled = true;
    size_t m_shiftsThisFrame = 0;
    bool m_deltaDirty = false;
    bool m_ready = false;
};

// Stage 1. Runs inside OnModLoad: the loader is alive, the game engine and the
// model info/pools may not be. Nothing here may touch engine state - only the
// loader, the logger and our own files.
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
    logger->Info("USA 1:1 Procedural Expansion loading (loader stage)...");

    // Paths first: ModDirectory only uses loader slots that existed long before
    // the ones this mod is built against, and validates everything it gets back.
    m_modDir = m_adapter.ModDirectory();
    m_configPath = m_modDir + "/config.json";
    m_deltaPath = m_modDir + "/world_delta.json";
    m_missionsDir = m_modDir + "/assets/missions";
    logger->Info("mod dir: %s", m_modDir.c_str());

    LoadConfig();
    const auto size = m_config.screenOverride;
    ImGui_ImplRenderWare_SetScreenSizeOverride(size.width, size.height);

    m_delta = WorldDelta::Load(m_deltaPath);
    if (m_config.startingMoney > 0 && m_delta.money == 0)
        m_delta.money = m_config.startingMoney;

    LoadMissions();
    m_overlay.SetMissions(&m_missions, &m_delta);

    m_adapter.SetTickCallback([this](double dt) { OnTick(dt); });
    m_adapter.SetTouchCallback([this](int a, int t, int x, int y) { return OnTouch(a, t, x, y); });
    m_adapter.SetShutdownCallback([this]() { OnUnload(); });

    EnsureLoaderStage();
    logger->Info("USA 1:1 loader stage done, waiting for the first game tick");
}

// Stage 1b. AML fires this when every mod finished loading. Retry the hook in
// case another mod was still initialising during OnModLoad.
void ModApp::OnAllModsLoaded() {
    logger->Info("USA 1:1: all mods loaded, finalising loader stage");
    if (!EnsureLoaderStage())
        logger->Print(LogP_Warn, "USA 1:1: engine hooks unavailable, mod stays inert (%s)",
                      m_adapter.LastError().c_str());
}

bool ModApp::EnsureLoaderStage() {
    if (m_loaderStage)
        return true;
    if (!m_adapter.Initialize()) {
        logger->Print(LogP_Warn, "adapter loader stage failed: %s", m_adapter.LastError().c_str());
        return false;
    }
    m_loaderStage = true;
    return true;
}

// Stage 2. Runs from the first CGame::Process tick: the engine is up, the model
// info array is filled, streaming and pools are live. Everything heavy (symbol
// verification, CModelInfo scan, ImGui, JNI/CloudSync) happens here.
void ModApp::BootRuntime() {
    if (m_runtimeReady)
        return;
    ++m_bootAttempts;

    if (!EnsureLoaderStage())
        return;

    if (!m_adapter.InitializeRuntime()) {
        if (m_bootAttempts <= 3)
            logger->Print(LogP_Warn, "runtime stage not ready yet (%s), retrying", m_adapter.LastError().c_str());
        return;
    }

    logger->Info("USA 1:1: %s", m_adapter.SymbolSummary().c_str());

    // Model table size for this installation: stock 2.00 fills 20000 slots,
    // Fastman92's limit adjuster can raise it. Scanning past the real size walks
    // into slots that hold plain integers, not model pointers.
    m_adapter.SetModelIdLimit(m_config.modelIdLimit);

    LoadCatalogue();

    FloatingOrigin::Config originCfg;
    originCfg.chunkSize = m_config.chunkSize;
    originCfg.gridRadius = m_config.gridRadius;
    originCfg.localLimit = m_config.localLimit;
    originCfg.safetyMargin = m_config.safetyMargin;
    m_origin = FloatingOrigin(originCfg);
    if (m_delta.hasPlayerPos)
        m_origin.SetOffset(Vec2d(m_delta.playerGlobalX, m_delta.playerGlobalY));

    m_generator = std::make_unique<ChunkGenerator>(m_catalog, m_config.generationZ);

    ChunkStreamer::Config streamCfg;
    streamCfg.chunkSize = m_config.chunkSize;
    streamCfg.radius = m_config.gridRadius;
    streamCfg.maxChunksPerUpdate = m_config.maxChunksPerUpdate;
    streamCfg.maxSpawnsPerUpdate = m_config.maxSpawnsPerUpdate;
    streamCfg.spawnMarkers = m_config.doorMarkersEnabled;
    m_streamer = std::make_unique<ChunkStreamer>(m_adapter, *m_generator, streamCfg);

    m_interiorGen = std::make_unique<InteriorGenerator>(m_catalog);
    m_interiors = std::make_unique<InteriorManager>(m_adapter, *m_interiorGen);
    TestPolygon::Config polygonCfg;
    polygonCfg.generationZ = m_config.generationZ;
    m_polygon = std::make_unique<TestPolygon>(m_adapter, m_catalog, polygonCfg);
    m_streamingEnabled = m_config.streamingEnabled;

    if (!m_overlay.Initialize())
        logger->Print(LogP_Warn, "ImGui overlay failed to init - the mod still runs headless");
    m_overlay.SetVisible(m_config.showOverlayOnStart);
    m_overlay.SetMissions(&m_missions, &m_delta);
    WireOverlayActions();

    // CloudSync needs a JNI environment; the transport reports whether it got
    // one and stays inert otherwise.
    m_cloud = std::make_unique<net::CloudSync>(m_transport, m_missionsDir, m_modDir + "/cloud_state.json");
    if (!m_transport.Available())
        logger->Print(LogP_Warn, "CloudSync disabled: %s", m_transport.LastError().c_str());
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

    m_sessionStart = m_adapter.NowSeconds();
    m_ready = true;
    m_runtimeReady = true;
    logger->Info("USA 1:1 loaded: %zu models, %zu missions, grid %dx%d", m_catalog.Size(), m_missions.Size(),
                 m_config.gridRadius * 2 + 1, m_config.gridRadius * 2 + 1);
}

void ModApp::OnGameCrash(const char *library, int sig, int code, uintptr_t libaddr) {
    char buf[256];
    snprintf(buf, sizeof(buf), "CRASH lib=%s sig=%d code=%d libaddr=0x%llX", library ? library : "?",
             sig, code, static_cast<unsigned long long>(libaddr));
    logger->Error("%s", buf);
    logger->Error("state: loader=%d runtime=%d ready=%d models=%zu missions=%zu shifts=%zu", m_loaderStage ? 1 : 0,
                  m_runtimeReady ? 1 : 0, m_ready ? 1 : 0, m_catalog.Size(), m_missions.Size(),
                  m_origin.TotalShifts());
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
    m_runtimeReady = false;
    m_loaderStage = false;
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
    const std::string assets = m_modDir + "/assets";
    if (!m_adapter.BuildLiveCatalog(m_catalog, assets)) {
        logger->Print(LogP_Warn, "catalogue is empty: chunks will spawn no props (markers only)");
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(assets, ec);
    if (FILE *f = fopen((assets + "/objects_db_live.json").c_str(), "wb")) {
        const std::string text = m_catalog.ToJson().Dump(true);
        fwrite(text.data(), 1, text.size(), f);
        fclose(f);
    }
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
    if (!m_runtimeReady) {
        // Wait until the world is actually live (player ped exists, streaming
        // is initialised): the model info array is still filling up during the
        // loading screen, and a catalogue scanned then would be empty.
        if (!m_adapter.IsEngineLoaded())
            return;
        BootRuntime();
        if (!m_runtimeReady)
            return;
    }
    if (m_menuTogglePending.exchange(false)) {
        m_overlay.Toggle();
        m_adapter.ShowToast("Touch event received");
    }
    if (!m_adapter.IsGameReady())
        return;
    if (!m_initialSpawnDone) {
        ApplyInitialSpawn();
        if (m_initialSpawnDone && m_config.autoStartPolygon)
            TogglePolygon();
    }

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
        m_polygon->OnOriginShift(m_origin.Offset());
        if (!m_adapter.GetPlayerPosition(local))
            return;
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

bool ModApp::OnTouch(int action, int track, int x, int y) {
    const auto measured = m_overlay.TouchScreenSize();
    const auto size = measured.Valid() ? measured : m_config.screenOverride;
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const auto gesture = m_menuGesture.Feed(action, track, x, y, now, m_overlay.Visible(), size.width, size.height);
    if (gesture.toggle) {
        // Don't deliver an UP for a recognised menu gesture as an ImGui click.
        // Apply Toggle and publish StatusFeed on the game thread.
        m_overlay.CancelPointer();
        m_menuTogglePending.store(true);
    }
    return gesture.consume || m_overlay.HandleTouch(action, track, x, y);
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
    t.activeObjects = static_cast<int>(m_streamer->ObjectCount() + m_polygon->TileCount());
    t.streamedEntities = m_adapter.CountStreamedEntities();
    t.playerInterior = m_interiors->CurrentArea();

    const std::string diagnostic = m_polygon->StatusLine();
    if (diagnostic != m_roadStatus.LastText()) {
        m_roadStatus.Push(diagnostic, m_adapter.NowSeconds());
        logger->Info("%s", diagnostic.c_str());
    }
    model.roadStatus = m_roadStatus.LastText();

    std::string status;
    if (m_adapter.Status().Latest(status, m_adapter.NowSeconds()))
        model.statusLine = status;
    model.statusAgeSec = m_adapter.Status().LastAge(m_adapter.NowSeconds());

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
    if (m_interiors->IsInside())
        m_interiors->Exit(m_delta);
    Vec3f local;
    if (!m_adapter.GetPlayerPosition(local))
        return;
    m_polygon->Enable(m_origin.ToGlobal(local), m_origin.Offset());
    m_adapter.ShowToast(m_polygon->WaitingForSupport() ? "Test polygon ON (waiting for collision tile)" : "Test polygon ON");
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

ON_ALL_MODS_LOAD() { ModApp::Get().OnAllModsLoaded(); }

ON_MOD_UNLOAD() { ModApp::Get().OnUnload(); }

// Forwards the loader's crash report: the log then names the library and the
// offsets instead of leaving a bare tombstone in aml_crashlog.txt.
ON_GAME_CRASH() {
    ModApp::Get().OnGameCrash(library, sig, code, libaddr);
}
