// Mod settings persisted as JSON in the mod folder (not AML's ini, so the file
// can also be edited on a PC and pushed back).
#pragma once

#include "json.hpp"

#include <string>

namespace usa1to1 {
namespace core {

struct ModConfig {
    // Streaming
    bool streamingEnabled = true;
    int gridRadius = 2;                 // 2 => 5x5 chunks (200 m each)
    int maxChunksPerUpdate = 3;
    int maxSpawnsPerUpdate = 40;

    // Floating origin
    double chunkSize = 200.0;
    double localLimit = 1000.0;
    double safetyMargin = 50.0;

    // Content
    bool interiorsEnabled = true;
    bool doorMarkersEnabled = true;
    bool autoStartPolygon = false;
    std::string spawnCity = "Los Angeles";
    bool teleportOnFirstLoad = true;
    float doorTriggerRadius = 1.6f;
    int64_t startingMoney = 0;
    // How many entries of CModelInfo::ms_modelInfoPtrs we are allowed to touch.
    // GTA SA 2.00 fills 20000; Fastman92's limit adjuster can raise it, so the
    // value is configurable up to the hard ceiling of the mobile arrays.
    int modelIdLimit = 20000;

    // Cloud sync
    bool cloudSyncEnabled = true;
    double cloudSyncIntervalSec = 600.0;
    std::string manifestUrl =
        "https://raw.githubusercontent.com/universeofvirtualcountries-rgb/SanAndreasUSA/main/assets/manifest.json";
    std::string missionsBaseUrl =
        "https://raw.githubusercontent.com/universeofvirtualcountries-rgb/SanAndreasUSA/main/assets/missions/";

    // UI
    bool showOverlayOnStart = false;
    bool showTelemetry = true;
    bool verboseLog = false;

    Json ToJson() const;
    bool FromJson(const Json &root, std::string *error = nullptr);
    bool LoadFile(const std::string &path, std::string *error = nullptr);
    bool SaveFile(const std::string &path, std::string *error = nullptr) const;
};

} // namespace core
} // namespace usa1to1
