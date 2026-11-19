#include "hash.hpp"

namespace usa1to1 {
namespace core {

uint64_t Mix64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBULL;
    x ^= x >> 31;
    return x;
}

uint64_t Fnv1a64(const void *data, size_t len) {
    constexpr uint64_t kOffset = 0xCBF29CE484222325ULL;
    constexpr uint64_t kPrime = 0x100000001B3ULL;
    uint64_t h = kOffset;
    const unsigned char *p = static_cast<const unsigned char *>(data);
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= kPrime;
    }
    return h;
}

uint64_t Fnv1a64(const std::string &s) { return Fnv1a64(s.data(), s.size()); }

namespace {
// Order independent of sign so that -0 == 0 and neighbouring chunks never alias.
inline uint64_t Key(uint64_t a, uint64_t b) {
    return Mix64(a * 0x9E3779B97F4A7C15ULL ^ Mix64(b + 0x632BE59BD9B4E019ULL));
}
} // namespace

uint64_t ChunkSeed(int64_t chunkX, int64_t chunkY) {
    const uint64_t hx = Key(static_cast<uint64_t>(chunkX), 0x1D1ULL);
    const uint64_t hy = Key(static_cast<uint64_t>(chunkY), 0x2E2ULL);
    return Mix64(hx ^ (hy + 0x9E3779B97F4A7C15ULL + (hx << 6) + (hx >> 2)));
}

uint64_t DoorSeed(int64_t chunkX, int64_t chunkY, uint32_t doorSlot) {
    return Mix64(ChunkSeed(chunkX, chunkY) ^ (static_cast<uint64_t>(doorSlot) * 0xD6E8FEB86659FD93ULL));
}

uint64_t InteriorSeed(uint64_t doorSeed) { return Mix64(doorSeed ^ 0xA0761D6478BD642FULL); }

uint64_t MarkerIdFromDoorSeed(uint64_t doorSeed) { return Mix64(doorSeed) & 0x7FFFFFFFFFFFFFFFULL; }

} // namespace core
} // namespace usa1to1
