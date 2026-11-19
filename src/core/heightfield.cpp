#include "heightfield.hpp"

#include "hash.hpp"
#include "regions.hpp"

#include <algorithm>
#include <cmath>

namespace usa1to1 {
namespace core {
namespace {

inline double SmoothStep(double t) { return t * t * (3.0 - 2.0 * t); }

// Deterministic lattice noise in [-1, 1]. Integer lattice coordinates + salt.
inline double LatticeNoise(int64_t ix, int64_t iy, uint64_t salt) {
    const uint64_t h = Mix64(static_cast<uint64_t>(ix) * 0x9E3779B97F4A7C15ULL ^
                             Mix64(static_cast<uint64_t>(iy) + salt));
    // 53 bits -> [0,1) -> [-1,1)
    return (static_cast<double>(h >> 11) * (1.0 / 9007199254740992.0)) * 2.0 - 1.0;
}

// Bilinear value noise with a given lattice cell size in metres.
double ValueNoise(double x, double y, double cell, uint64_t salt) {
    const double gx = x / cell;
    const double gy = y / cell;
    const double fx = std::floor(gx);
    const double fy = std::floor(gy);
    const int64_t ix = static_cast<int64_t>(fx);
    const int64_t iy = static_cast<int64_t>(fy);
    const double tx = SmoothStep(gx - fx);
    const double ty = SmoothStep(gy - fy);

    const double v00 = LatticeNoise(ix, iy, salt);
    const double v10 = LatticeNoise(ix + 1, iy, salt);
    const double v01 = LatticeNoise(ix, iy + 1, salt);
    const double v11 = LatticeNoise(ix + 1, iy + 1, salt);

    const double a = v00 + (v10 - v00) * tx;
    const double b = v01 + (v11 - v01) * tx;
    return a + (b - a) * ty;
}

double Fbm(double x, double y, double cell, uint64_t salt, int octaves) {
    double sum = 0.0;
    double amp = 1.0;
    double norm = 0.0;
    double c = cell;
    for (int i = 0; i < octaves; ++i) {
        sum += ValueNoise(x, y, c, salt + static_cast<uint64_t>(i) * 0x100000001B3ULL) * amp;
        norm += amp;
        amp *= 0.5;
        c *= 0.5;
    }
    return norm > 0.0 ? sum / norm : 0.0;
}

inline double Gaussian(double v, double centre, double sigma) {
    const double d = (v - centre) / sigma;
    return std::exp(-0.5 * d * d);
}

} // namespace

float HeightField::LatticeValue(int64_t ix, int64_t iy) {
    // Height sampled exactly on the 400 m lattice: two octaves, sea level base.
    const double macro = LatticeNoise(ix, iy, kWorldSalt) * 140.0;
    const double micro = LatticeNoise(ix, iy, kWorldSalt ^ 0xABCDEF0123456789ULL) * 12.0;
    double h = 45.0 + macro + micro;
    if (h < 0.0)
        h = 0.0;
    return static_cast<float>(h);
}

float HeightField::HeightAt(double x, double y) {
    // Macro relief: continental plains and basins.
    const double macro = Fbm(x, y, 200000.0, kWorldSalt, 3) * 220.0;
    // Mid scale hills.
    const double mid = Fbm(x, y, 20000.0, kWorldSalt ^ 0x51ED270B1ULL, 3) * 70.0;
    // Fine bumps; keeps wheels from sliding on an unrealistically perfect plane.
    const double micro = Fbm(x, y, 2000.0, kWorldSalt ^ 0x9A1C0DE5ULL, 2) * 8.0;

    double h = 40.0 + macro + mid + micro;

    // Rockies / Sierra belt: longitude driven ridge with a noisy spine.
    double lat = 0.0;
    double lon = 0.0;
    PlaneToLatLon(x, y, lat, lon);
    const double rockies = Gaussian(lon, -106.5, 3.2) * Gaussian(lat, 39.0, 7.0);
    const double sierra = Gaussian(lon, -119.5, 0.9) * Gaussian(lat, 37.5, 3.5);
    const double appalach = Gaussian(lon, -81.0, 1.8) * Gaussian(lat, 37.5, 5.0);

    double ridge = 0.0;
    if (rockies > 0.02) {
        const double spine = Fbm(x, y, 60000.0, kWorldSalt ^ 0xB1D6E5ULL, 3);
        ridge += rockies * (900.0 + 800.0 * spine);
    }
    if (sierra > 0.02) {
        const double spine = Fbm(x, y, 40000.0, kWorldSalt ^ 0x51E77AULL, 3);
        ridge += sierra * (600.0 + 700.0 * spine);
    }
    if (appalach > 0.02) {
        const double spine = Fbm(x, y, 30000.0, kWorldSalt ^ 0xA99A1ULL, 3);
        ridge += appalach * (250.0 + 300.0 * spine);
    }
    h += ridge;

    // Atlantic / Gulf / Pacific shelves: fall off to sea level at the plane edge.
    const double edge = 60000.0;
    const double dx = std::min(x - kUsaPlaneMinX, kUsaPlaneMaxX - x);
    const double dy = std::min(y - kUsaPlaneMinY, kUsaPlaneMaxY - y);
    const double shore = std::min(1.0, std::max(0.0, std::min(dx, dy) / edge));
    h *= 0.15 + 0.85 * shore;

    if (h < 0.0)
        h = 0.0;
    return static_cast<float>(h);
}

float HeightField::SlopeAt(double x, double y) {
    constexpr double kEps = 2.0;
    const float hx1 = HeightAt(x + kEps, y);
    const float hx0 = HeightAt(x - kEps, y);
    const float hy1 = HeightAt(x, y + kEps);
    const float hy0 = HeightAt(x, y - kEps);
    const double gx = (hx1 - hx0) / (2.0 * kEps);
    const double gy = (hy1 - hy0) / (2.0 * kEps);
    return static_cast<float>(std::sqrt(gx * gx + gy * gy));
}

bool HeightField::IsBuildable(double x, double y, float maxSlope) { return SlopeAt(x, y) <= maxSlope; }

} // namespace core
} // namespace usa1to1
