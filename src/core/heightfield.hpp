// Deterministic, seam-consistent terrain height.
//
// The height is a bilinear interpolant over a coarse lattice whose spacing is a
// whole multiple of the chunk size (400 m lattice vs 200 m chunks). Lattice
// values are pure functions of integer lattice coordinates, therefore any two
// neighbouring chunks sample identical values along their shared edge - the
// requirement "соседние чанки обязаны совпадать по высоте на общих кромках"
// holds by construction instead of by luck.
#pragma once

#include "types.hpp"

#include <cstdint>

namespace usa1to1 {
namespace core {

class HeightField {
  public:
    static constexpr double kLatticeSpacing = 400.0; // must be a multiple of chunk size
    static constexpr uint64_t kWorldSalt = 0x55A1'1EE1'2026'0001ULL;

    // Height in metres at a global plane position. Continuous everywhere, C0 on
    // lattice edges (which is what chunk seams need).
    static float HeightAt(double globalX, double globalY);

    // Lattice corner value - exposed for tests.
    static float LatticeValue(int64_t ix, int64_t iy);

    // Slope magnitude (m per m) at a point, via central differences. Used to
    // reject building placement on cliffs.
    static float SlopeAt(double globalX, double globalY);

    // True when the point is flat enough to hold a road tile / building.
    static bool IsBuildable(double globalX, double globalY, float maxSlope = 0.35f);
};

} // namespace core
} // namespace usa1to1
