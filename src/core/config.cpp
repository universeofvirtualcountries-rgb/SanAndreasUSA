#include "config.hpp"

#include "log.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace usa1to1 {
namespace core {

Json ModConfig::ToJson() const {
    Json root = Json::Object();
    root.Set("version", 1);

    Json streaming = Json::Object();
    streaming.Set("enabled", streamingEnabled);
    streaming.Set("grid_radius", gridRadius);
    streaming.Set("max_chunks_per_update", maxChunksPerUpdate);
    streaming.Set("max_spawns_per_update", maxSpawnsPerUpdate);
    root.Set("streaming", streaming);

    Json origin = Json::Object();
    origin.Set("chunk_size", chunkSize);
    origin.Set("local_limit", localLimit);
    origin.Set("safety_margin", safetyMargin);
    root.Set("floating_origin", origin);

    Json content = Json::Object();
    content.Set("interiors", interiorsEnabled);
    content.Set("door_markers", doorMarkersEnabled);
    content.Set("auto_polygon", autoStartPolygon);
    content.Set("spawn_city", spawnCity);
    content.Set("teleport_on_first_load", teleportOnFirstLoad);
    content.Set("door_trigger_radius", static_cast<double>(doorTriggerRadius));
    content.Set("starting_money", startingMoney);
    content.Set("model_id_limit", modelIdLimit);
    root.Set("content", content);

    Json cloud = Json::Object();
    cloud.Set("enabled", cloudSyncEnabled);
    cloud.Set("interval_sec", cloudSyncIntervalSec);
    cloud.Set("manifest_url", manifestUrl);
    cloud.Set("missions_base_url", missionsBaseUrl);
    root.Set("cloud_sync", cloud);

    Json ui = Json::Object();
    ui.Set("show_on_start", showOverlayOnStart);
    ui.Set("show_telemetry", showTelemetry);
    ui.Set("verbose_log", verboseLog);
    root.Set("ui", ui);
    return root;
}

bool ModConfig::FromJson(const Json &root, std::string *error) {
    try {
        if (const Json *s = root.Find("streaming")) {
            streamingEnabled = s->GetBool("enabled", streamingEnabled);
            gridRadius = static_cast<int>(s->GetInt("grid_radius", gridRadius));
            maxChunksPerUpdate = static_cast<int>(s->GetInt("max_chunks_per_update", maxChunksPerUpdate));
            maxSpawnsPerUpdate = static_cast<int>(s->GetInt("max_spawns_per_update", maxSpawnsPerUpdate));
        }
        if (const Json *o = root.Find("floating_origin")) {
            chunkSize = o->GetDouble("chunk_size", chunkSize);
            localLimit = o->GetDouble("local_limit", localLimit);
            safetyMargin = o->GetDouble("safety_margin", safetyMargin);
        }
        if (const Json *c = root.Find("content")) {
            interiorsEnabled = c->GetBool("interiors", interiorsEnabled);
            doorMarkersEnabled = c->GetBool("door_markers", doorMarkersEnabled);
            autoStartPolygon = c->GetBool("auto_polygon", autoStartPolygon);
            spawnCity = c->GetString("spawn_city", spawnCity);
            teleportOnFirstLoad = c->GetBool("teleport_on_first_load", teleportOnFirstLoad);
            doorTriggerRadius = static_cast<float>(c->GetDouble("door_trigger_radius", doorTriggerRadius));
            startingMoney = c->GetInt("starting_money", startingMoney);
            modelIdLimit = static_cast<int>(c->GetInt("model_id_limit", modelIdLimit));
        }
        if (const Json *cl = root.Find("cloud_sync")) {
            cloudSyncEnabled = cl->GetBool("enabled", cloudSyncEnabled);
            cloudSyncIntervalSec = cl->GetDouble("interval_sec", cloudSyncIntervalSec);
            manifestUrl = cl->GetString("manifest_url", manifestUrl);
            missionsBaseUrl = cl->GetString("missions_base_url", missionsBaseUrl);
        }
        if (const Json *u = root.Find("ui")) {
            showOverlayOnStart = u->GetBool("show_on_start", showOverlayOnStart);
            showTelemetry = u->GetBool("show_telemetry", showTelemetry);
            verboseLog = u->GetBool("verbose_log", verboseLog);
        }
        // Sanity clamps so a hand-edited file cannot brick the mod.
        if (gridRadius < 1)
            gridRadius = 1;
        if (gridRadius > 3)
            gridRadius = 3;
        if (maxChunksPerUpdate < 1)
            maxChunksPerUpdate = 1;
        if (maxSpawnsPerUpdate < 1)
            maxSpawnsPerUpdate = 1;
        if (chunkSize < 50.0)
            chunkSize = 200.0;
        if (localLimit > 1000.0 || localLimit < 400.0)
            localLimit = 1000.0;
        if (modelIdLimit < 400 || modelIdLimit > 26316)
            modelIdLimit = modelIdLimit < 400 ? 400 : 26316;
        return true;
    } catch (const std::exception &e) {
        if (error)
            *error = e.what();
        return false;
    }
}

bool ModConfig::LoadFile(const std::string &path, std::string *error) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false; // missing file is not an error: defaults are used
    std::stringstream ss;
    ss << in.rdbuf();
    Json root;
    if (!Json::TryParse(ss.str(), root, error))
        return false;
    return FromJson(root, error);
}

bool ModConfig::SaveFile(const std::string &path, std::string *error) const {
    namespace fs = std::filesystem;
    try {
        const fs::path p(path);
        if (p.has_parent_path())
            fs::create_directories(p.parent_path());
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (error)
                *error = "cannot write " + path;
            return false;
        }
        out << ToJson().Dump(true) << '\n';
        return out.good();
    } catch (const std::exception &e) {
        if (error)
            *error = e.what();
        return false;
    }
}

} // namespace core
} // namespace usa1to1
