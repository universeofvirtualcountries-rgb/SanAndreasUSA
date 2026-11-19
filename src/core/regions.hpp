// 1:1 USA territory emulation: the world is a projected metric plane that
// covers the continental US. Global (0,0) sits at the south-west corner of the
// plane (Pacific, off San Diego), +X goes east, +Y goes north.
//
// Bounding boxes are deliberately coarse (they are telemetry labels, not
// borders), but the anchors for cities are real geography so a 1:1 teleport
// lands where a player expects.
#pragma once

#include "types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace usa1to1 {
namespace core {

struct RegionBox {
    const char *name;   // "California"
    const char *code;   // "CA"
    double minX, minY;  // metres on the plane
    double maxX, maxY;
};

struct CityAnchor {
    const char *name;      // "New York"
    const char *region;    // "NY"
    double x, y;           // plane metres
    int spawnModelHint;    // unused for now, kept for save-migration
};

// Plane geometry: continental US is ~4500 km wide, ~2900 km tall.
constexpr double kUsaPlaneMinX = 0.0;
constexpr double kUsaPlaneMinY = 0.0;
constexpr double kUsaPlaneMaxX = 4500000.0;
constexpr double kUsaPlaneMaxY = 2900000.0;

const std::vector<RegionBox> &GetRegionBoxes();
const std::vector<CityAnchor> &GetCityAnchors();

// Returns the region containing the point, or the closest one when offshore.
const RegionBox *FindRegion(double x, double y);

// Sector label, e.g. "CA-42" (region code + 10 km cell index inside the region).
std::string SectorLabel(const RegionBox &region, double x, double y);

// Human readable "New York (NY)" style nearest-city description for telemetry.
std::string NearestCityLabel(double x, double y);

// Anchor lookup by exact name (case sensitive, as exposed in the overlay).
const CityAnchor *FindCity(const std::string &name);

// Clamp a global position into the US plane with a small ocean margin.
Vec2d ClampToPlane(double x, double y);

constexpr double kMetersPerDegreeLat = 111320.0;
constexpr double kPlaneReferenceLat = 39.5; // Kansas/Nebraska border, plane centre
inline double MetersPerDegreeLon() {
    constexpr double kPi = 3.14159265358979323846;
    return kMetersPerDegreeLat * std::cos(kPlaneReferenceLat * kPi / 180.0);
}

// Geographic helpers used by the teleport table and by tools/parse_ide.py docs.
Vec2d LatLonToPlane(double lat, double lon);
void PlaneToLatLon(double x, double y, double &lat, double &lon);

} // namespace core
} // namespace usa1to1
