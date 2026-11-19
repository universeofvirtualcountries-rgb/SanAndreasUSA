// Deterministic chunk content generation.
//
// One chunk = 200 x 200 m of the 1:1 USA plane. Content is a pure function of
// (chunkX, chunkY) plus the model catalogue, so two devices with the same seed
// build the exact same street, from the same models, at the same heights.
#pragma once

#include "model_catalog.hpp"
#include "types.hpp"

#include <cstdint>
#include <vector>

namespace usa1to1 {
namespace core {

struct ChunkCoord {
    int64_t x = 0;
    int64_t y = 0;
    bool operator==(const ChunkCoord &o) const { return x == o.x && y == o.y; }
    bool operator!=(const ChunkCoord &o) const { return !(*this == o); }
};

struct ChunkProp {
    int modelId = -1;
    Vec3f localPos;      // metres from the chunk's south-west corner
    float headingRad = 0.0f;
    PropTag tag = PropTag::Unknown;
    float halfFootprint = 1.0f; // used for seam checks and placement margins
};

struct ChunkDoor {
    uint32_t slot = 0;
    uint64_t seed = 0;
    Vec3f localPos;
    float headingRad = 0.0f;
    bool shop = false;
};

struct ChunkContent {
    ChunkCoord coord;
    Vec2d originGlobal; // global plane metres of the SW corner
    uint64_t seed = 0;
    float heightMin = 0.0f;
    float heightMax = 0.0f;
    bool urban = false;
    std::vector<ChunkProp> props;
    std::vector<ChunkDoor> doors;
    std::vector<int> requiredModels;

    bool Empty() const { return props.empty() && doors.empty(); }
};

class ChunkGenerator {
  public:
    static constexpr double kChunkSize = 200.0;

    explicit ChunkGenerator(const ModelCatalog &catalog);

    ChunkContent Generate(int64_t chunkX, int64_t chunkY) const;

    // Height of the chunk edge, sampled at `steps` points. Used by the seam
    // consistency tests: chunk A's east edge must equal chunk B's west edge.
    std::vector<float> SampleEdge(int64_t chunkX, int64_t chunkY, int edge, int steps = 8) const;

    static Vec2d ChunkOriginGlobal(int64_t chunkX, int64_t chunkY);
    static ChunkCoord ChunkOf(const Vec2d &globalPos);
    // True when the world point lies inside the chunk (edge inclusive on min).
    static bool Contains(const ChunkCoord &coord, const Vec2d &globalPos);

    const ModelCatalog &Catalog() const { return m_catalog; }

  private:
    ModelCatalog m_catalog;
};

} // namespace core
} // namespace usa1to1
