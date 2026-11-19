#include "regions.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace usa1to1 {
namespace core {
namespace {

struct LatLonBox {
    const char *name;
    const char *code;
    double latMin, lonMin, latMax, lonMax;
};

// Coarse state envelopes. These are labels for the HUD and for spawn-biome
// selection; they are not legal borders and never gate gameplay.
const LatLonBox kStateTable[] = {
    {"Washington", "WA", 45.5, -124.8, 49.0, -117.0},
    {"Oregon", "OR", 42.0, -124.6, 46.3, -116.5},
    {"California", "CA", 32.5, -124.4, 42.0, -114.1},
    {"Nevada", "NV", 35.0, -120.0, 42.0, -114.0},
    {"Idaho", "ID", 42.0, -117.2, 49.0, -111.0},
    {"Utah", "UT", 37.0, -114.1, 42.0, -109.0},
    {"Arizona", "AZ", 31.3, -114.8, 37.0, -109.0},
    {"Montana", "MT", 44.4, -116.0, 49.0, -104.0},
    {"Wyoming", "WY", 41.0, -111.1, 45.0, -104.0},
    {"Colorado", "CO", 37.0, -109.1, 41.0, -102.0},
    {"New Mexico", "NM", 31.3, -109.1, 37.0, -103.0},
    {"North Dakota", "ND", 45.9, -104.0, 49.0, -96.6},
    {"South Dakota", "SD", 42.5, -104.1, 45.9, -96.4},
    {"Nebraska", "NE", 40.0, -104.1, 43.0, -95.3},
    {"Kansas", "KS", 37.0, -102.1, 40.0, -94.6},
    {"Oklahoma", "OK", 33.6, -103.0, 37.0, -94.4},
    {"Texas", "TX", 25.8, -106.6, 36.5, -93.5},
    {"Minnesota", "MN", 43.5, -97.2, 49.4, -89.5},
    {"Iowa", "IA", 40.4, -96.6, 43.5, -90.1},
    {"Missouri", "MO", 36.0, -95.8, 40.6, -89.1},
    {"Arkansas", "AR", 33.0, -94.6, 36.5, -89.6},
    {"Louisiana", "LA", 28.9, -94.0, 33.0, -88.8},
    {"Wisconsin", "WI", 42.5, -92.9, 47.1, -86.8},
    {"Illinois", "IL", 36.9, -91.5, 42.5, -87.0},
    {"Michigan", "MI", 41.7, -90.4, 48.3, -82.4},
    {"Indiana", "IN", 37.8, -88.1, 41.8, -84.8},
    {"Ohio", "OH", 38.4, -84.8, 42.0, -80.5},
    {"Kentucky", "KY", 36.5, -89.6, 39.1, -81.9},
    {"Tennessee", "TN", 35.0, -90.3, 36.7, -81.6},
    {"Mississippi", "MS", 30.2, -91.7, 35.0, -88.1},
    {"Alabama", "AL", 30.2, -88.5, 35.0, -84.9},
    {"Georgia", "GA", 30.4, -85.6, 35.0, -80.8},
    {"Florida", "FL", 24.4, -87.6, 31.0, -80.0},
    {"South Carolina", "SC", 32.0, -83.4, 35.2, -78.5},
    {"North Carolina", "NC", 33.9, -84.3, 36.6, -75.4},
    {"Virginia", "VA", 36.5, -83.7, 39.5, -75.2},
    {"West Virginia", "WV", 37.2, -82.6, 40.6, -77.7},
    {"Maryland", "MD", 37.9, -79.5, 39.7, -75.0},
    {"Delaware", "DE", 38.4, -75.8, 39.9, -75.0},
    {"Pennsylvania", "PA", 39.7, -80.5, 42.3, -74.7},
    {"New Jersey", "NJ", 38.9, -75.6, 41.4, -73.9},
    {"New York", "NY", 40.5, -79.8, 45.0, -71.8},
    {"Connecticut", "CT", 40.9, -73.7, 42.1, -71.8},
    {"Rhode Island", "RI", 41.1, -71.9, 42.0, -71.1},
    {"Massachusetts", "MA", 41.2, -73.5, 42.9, -69.9},
    {"Vermont", "VT", 42.7, -73.5, 45.1, -71.5},
    {"New Hampshire", "NH", 42.7, -72.6, 45.3, -70.6},
    {"Maine", "ME", 43.0, -71.1, 47.5, -66.9},
    {"District of Columbia", "DC", 38.79, -77.12, 38.99, -76.90},
    // Fictional state used by the bundled missions. It sits in western Kansas
    // so it is reachable on foot from the real world, and it only wins the
    // lookup because its envelope is the smallest one covering the point.
    {"Red Mesa", "RM", 38.20, -101.50, 39.00, -100.40},
};

struct LatLonCity {
    const char *name;
    const char *region;
    double lat, lon;
};

const LatLonCity kCityTable[] = {
    {"New York", "NY", 40.7128, -74.0060},
    {"Washington", "DC", 38.9072, -77.0369},
    {"Las Vegas", "NV", 36.1699, -115.1398},
    {"Los Angeles", "CA", 34.0522, -118.2437},
    {"San Francisco", "CA", 37.7749, -122.4194},
    {"Seattle", "WA", 47.6062, -122.3321},
    {"Chicago", "IL", 41.8781, -87.6298},
    {"Houston", "TX", 29.7604, -95.3698},
    {"Dallas", "TX", 32.7767, -96.7970},
    {"Miami", "FL", 25.7617, -80.1918},
    {"Denver", "CO", 39.7392, -104.9903},
    {"Phoenix", "AZ", 33.4484, -112.0740},
    {"Atlanta", "GA", 33.7490, -84.3880},
    {"Boston", "MA", 42.3601, -71.0589},
    {"Detroit", "MI", 42.3314, -83.0458},
    {"New Orleans", "LA", 29.9511, -90.0715},
    {"Kansas City", "KS", 39.0997, -94.5786},
    {"Red Mesa City", "RM", 38.6010, -100.9500},
};

std::vector<RegionBox> BuildRegions() {
    std::vector<RegionBox> out;
    out.reserve(sizeof(kStateTable) / sizeof(kStateTable[0]));
    for (const auto &b : kStateTable) {
        const Vec2d sw = LatLonToPlane(b.latMin, b.lonMin);
        const Vec2d ne = LatLonToPlane(b.latMax, b.lonMax);
        out.push_back(RegionBox{b.name, b.code, sw.x, sw.y, ne.x, ne.y});
    }
    return out;
}

std::vector<CityAnchor> BuildCities() {
    std::vector<CityAnchor> out;
    out.reserve(sizeof(kCityTable) / sizeof(kCityTable[0]));
    for (const auto &c : kCityTable) {
        const Vec2d p = LatLonToPlane(c.lat, c.lon);
        out.push_back(CityAnchor{c.name, c.region, p.x, p.y, -1});
    }
    return out;
}

double AreaOf(const RegionBox &b) { return (b.maxX - b.minX) * (b.maxY - b.minY); }

} // namespace

Vec2d LatLonToPlane(double lat, double lon) {
    // Plane origin: 24.5N / 125.0W, the south-west corner of the enabled area.
    constexpr double kLat0 = 24.5;
    constexpr double kLon0 = -125.0;
    return {(lon - kLon0) * MetersPerDegreeLon(), (lat - kLat0) * kMetersPerDegreeLat};
}

void PlaneToLatLon(double x, double y, double &lat, double &lon) {
    constexpr double kLat0 = 24.5;
    constexpr double kLon0 = -125.0;
    lat = y / kMetersPerDegreeLat + kLat0;
    lon = x / MetersPerDegreeLon() + kLon0;
}

const std::vector<RegionBox> &GetRegionBoxes() {
    static const std::vector<RegionBox> boxes = BuildRegions();
    return boxes;
}

const std::vector<CityAnchor> &GetCityAnchors() {
    static const std::vector<CityAnchor> cities = BuildCities();
    return cities;
}

namespace {
const RegionBox *FindRegionByCode(const char *code) {
    const RegionBox *best = nullptr;
    double bestArea = 0.0;
    for (const auto &b : GetRegionBoxes()) {
        if (std::string(b.code) != code)
            continue;
        const double area = AreaOf(b);
        if (!best || area < bestArea) {
            best = &b;
            bestArea = area;
        }
    }
    return best;
}
} // namespace

const RegionBox *FindRegion(double x, double y) {
    // 0) Metro areas belong to their own state even when a smaller neighbour
    //    box overlaps them (NYC must not report New Jersey, DC must not report
    //    Maryland). 30 km around an anchor is a sane "city limits" stand-in.
    const CityAnchor *nearestCity = nullptr;
    double nearestDist = 30000.0 * 30000.0;
    for (const auto &c : GetCityAnchors()) {
        const double dx = x - c.x;
        const double dy = y - c.y;
        const double d = dx * dx + dy * dy;
        if (d < nearestDist) {
            nearestDist = d;
            nearestCity = &c;
        }
    }
    if (nearestCity) {
        if (const RegionBox *cityRegion = FindRegionByCode(nearestCity->region))
            return cityRegion;
    }

    // 1) Smallest containing envelope wins, so the fictional Red Mesa beats
    //    the much larger Kansas box it sits inside.
    const RegionBox *best = nullptr;
    double bestArea = 0.0;
    for (const auto &b : GetRegionBoxes()) {
        if (x < b.minX || x > b.maxX || y < b.minY || y > b.maxY)
            continue;
        const double area = AreaOf(b);
        if (!best || area < bestArea) {
            best = &b;
            bestArea = area;
        }
    }
    if (best)
        return best;

    // 2) Offshore / outside: report the closest region so the HUD never goes blank.
    double bestDist = 1e30;
    for (const auto &b : GetRegionBoxes()) {
        const double cx = Clampd(x, b.minX, b.maxX);
        const double cy = Clampd(y, b.minY, b.maxY);
        const double dx = x - cx;
        const double dy = y - cy;
        const double d = dx * dx + dy * dy;
        if (d < bestDist) {
            bestDist = d;
            best = &b;
        }
    }
    return best;
}

std::string SectorLabel(const RegionBox &region, double x, double y) {
    constexpr double kCell = 10000.0; // 10 km sectors
    const int sx = static_cast<int>(std::floor((x - region.minX) / kCell));
    const int sy = static_cast<int>(std::floor((y - region.minY) / kCell));
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s %d%02d", region.code, sx, sy < 0 ? 0 : sy);
    return buf;
}

std::string NearestCityLabel(double x, double y) {
    const CityAnchor *nearest = nullptr;
    double best = 1e30;
    for (const auto &c : GetCityAnchors()) {
        const double dx = x - c.x;
        const double dy = y - c.y;
        const double d = dx * dx + dy * dy;
        if (d < best) {
            best = d;
            nearest = &c;
        }
    }
    if (!nearest)
        return "unknown";
    const double km = std::sqrt(best) / 1000.0;
    char buf[128];
    if (km <= 200.0)
        std::snprintf(buf, sizeof(buf), "%s, %s (%.0f km)", nearest->name, nearest->region, km);
    else
        std::snprintf(buf, sizeof(buf), "%s, %s (%.0f km)", nearest->name, nearest->region, km);
    return buf;
}

const CityAnchor *FindCity(const std::string &name) {
    for (const auto &c : GetCityAnchors())
        if (name == c.name)
            return &c;
    return nullptr;
}

Vec2d ClampToPlane(double x, double y) {
    constexpr double kMargin = 20000.0; // 20 km of ocean slack for the plane edges
    return {Clampd(x, kUsaPlaneMinX - kMargin, kUsaPlaneMaxX + kMargin),
            Clampd(y, kUsaPlaneMinY - kMargin, kUsaPlaneMaxY + kMargin)};
}

} // namespace core
} // namespace usa1to1
