// Deterministic 64-bit hashing + seeded RNG plumbing for the procedural world.
// Every chunk / door / interior derives its content from these functions, so
// the same global coordinates must always produce the same output on every
// device and every game version.
#pragma once

#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace usa1to1 {
namespace core {

// splitmix64 finalizer - the workhorse of every seed derivation here.
uint64_t Mix64(uint64_t x);

// FNV-1a 64 over raw bytes / strings.
uint64_t Fnv1a64(const void *data, size_t len);
uint64_t Fnv1a64(const std::string &s);

// Seed for a chunk identified by signed chunk indices.
uint64_t ChunkSeed(int64_t chunkX, int64_t chunkY);

// Seed for a door / entrance identified by chunk key + door slot.
uint64_t DoorSeed(int64_t chunkX, int64_t chunkY, uint32_t doorSlot);

// Seed for an interior instance (stable per door, independent of visit count).
uint64_t InteriorSeed(uint64_t doorSeed);

// 64-bit identifier used as C3dMarkers identifier and as delta-save key.
// Keeps the top bit clear so it survives a float/int round trip in scripts.
uint64_t MarkerIdFromDoorSeed(uint64_t doorSeed);

// Deterministic RNG bound to a seed. std::mt19937_64 is part of the standard and
// produces identical streams on every platform, which is exactly what we need.
class Rng {
  public:
    explicit Rng(uint64_t seed) : m_gen(seed) {}

    uint64_t Next64() { return m_gen(); }
    uint32_t Next32() { return static_cast<uint32_t>(m_gen() >> 32); }

    // Uniform in [0, bound), bound > 0.
    uint32_t Below(uint32_t bound) {
        if (bound == 0)
            return 0;
        return static_cast<uint32_t>(m_gen() % bound);
    }

    double Unit() { return static_cast<double>(m_gen() >> 11) * (1.0 / 9007199254740992.0); }
    float UnitF() { return static_cast<float>(Unit()); }
    double Range(double lo, double hi) { return lo + (hi - lo) * Unit(); }
    float RangeF(float lo, float hi) { return lo + (hi - lo) * UnitF(); }
    int RangeInt(int lo, int hiInclusive) {
        if (hiInclusive <= lo)
            return lo;
        return lo + static_cast<int>(Below(static_cast<uint32_t>(hiInclusive - lo + 1)));
    }
    bool Chance(float p) { return UnitF() < p; }

    template <class T> const T &Pick(const std::vector<T> &items) {
        return items[Below(static_cast<uint32_t>(items.size()))];
    }

    uint64_t State() { return m_gen(); }
    void Reseed(uint64_t seed) { m_gen.seed(seed); }

  private:
    std::mt19937_64 m_gen;
};

} // namespace core
} // namespace usa1to1
