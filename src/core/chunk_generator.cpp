#include "chunk_generator.hpp"

#include "hash.hpp"
#include "heightfield.hpp"
#include "log.hpp"
#include "regions.hpp"

#include <algorithm>
#include <cmath>
#include <set>

namespace usa1to1 {
namespace core {
namespace {

bool NearCity(const Vec2d &p, const char *regionCode, double radiusMetres) {
    for (const auto &c : GetCityAnchors()) {
        if (regionCode && std::string(c.region) != regionCode)
            continue;
        if (p.Distance(Vec2d(c.x, c.y)) <= radiusMetres)
            return true;
    }
    return false;
}

} // namespace

ChunkGenerator::ChunkGenerator(const ModelCatalog &catalog) : m_catalog(catalog) {}

Vec2d ChunkGenerator::ChunkOriginGlobal(int64_t chunkX, int64_t chunkY) {
    return {static_cast<double>(chunkX) * kChunkSize, static_cast<double>(chunkY) * kChunkSize};
}

ChunkCoord ChunkGenerator::ChunkOf(const Vec2d &globalPos) {
    // floor() not trunc(): negative coordinates must map to the chunk below zero.
    return {static_cast<int64_t>(std::floor(globalPos.x / kChunkSize)),
            static_cast<int64_t>(std::floor(globalPos.y / kChunkSize))};
}

bool ChunkGenerator::Contains(const ChunkCoord &coord, const Vec2d &globalPos) {
    const Vec2d o = ChunkOriginGlobal(coord.x, coord.y);
    return globalPos.x >= o.x && globalPos.x < o.x + kChunkSize && globalPos.y >= o.y &&
           globalPos.y < o.y + kChunkSize;
}

std::vector<float> ChunkGenerator::SampleEdge(int64_t chunkX, int64_t chunkY, int edge, int steps) const {
    // edge: 0 = west, 1 = east, 2 = south, 3 = north. Samples are inclusive of
    // both corners so the shared corners are compared twice.
    std::vector<float> out;
    if (steps < 2)
        steps = 2;
    const Vec2d o = ChunkOriginGlobal(chunkX, chunkY);
    for (int i = 0; i < steps; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(steps - 1);
        double x = o.x;
        double y = o.y;
        switch (edge) {
        case 0:
            y += t * kChunkSize;
            break;
        case 1:
            x += kChunkSize;
            y += t * kChunkSize;
            break;
        case 2:
            x += t * kChunkSize;
            break;
        default:
            x += t * kChunkSize;
            y += kChunkSize;
            break;
        }
        out.push_back(HeightField::HeightAt(x, y));
    }
    return out;
}

ChunkContent ChunkGenerator::Generate(int64_t chunkX, int64_t chunkY) const {
    ChunkContent out;
    out.coord = {chunkX, chunkY};
    out.seed = ChunkSeed(chunkX, chunkY);
    out.originGlobal = ChunkOriginGlobal(chunkX, chunkY);

    Rng rng(out.seed);

    // --- macro properties from geography ------------------------------------
    const Vec2d centre{out.originGlobal.x + kChunkSize * 0.5, out.originGlobal.y + kChunkSize * 0.5};
    const RegionBox *region = FindRegion(centre.x, centre.y);
    const bool inCity = NearCity(centre, nullptr, 12000.0);
    const bool nearCity = NearCity(centre, nullptr, 30000.0);
    const float slope = HeightField::SlopeAt(centre.x, centre.y);
    // Urban cores get dense blocks, suburbs sparse, mountains almost nothing.
    float urbanChance = 0.35f;
    if (inCity)
        urbanChance = 0.98f;
    else if (nearCity)
        urbanChance = 0.7f;
    else
        urbanChance = 0.25f;
    if (slope > 0.5f)
        urbanChance *= 0.2f;
    out.urban = rng.Chance(urbanChance);

    const std::vector<const ModelProto *> &buildings = m_catalog.ByTag(PropTag::Building);
    const std::vector<const ModelProto *> &trees = m_catalog.ByTag(PropTag::Tree);
    const std::vector<const ModelProto *> &props = m_catalog.ByTag(PropTag::Prop);
    const std::vector<const ModelProto *> &lights = m_catalog.ByTag(PropTag::StreetLight);

    // --- edges must never be crossed ---------------------------------------
    // Every prop is kept at least halfFootprint + margin away from the border,
    // and additionally must fit fully inside the chunk. This is what makes the
    // 200 m grid visually seamless: nothing dangles over a boundary.
    auto placeInside = [&](float localX, float localY, float halfFootprint) -> bool {
        return localX - halfFootprint >= 0.0f && localX + halfFootprint <= kChunkSize &&
               localY - halfFootprint >= 0.0f && localY + halfFootprint <= kChunkSize;
    };

    // --- buildings on a jittered 40 m grid ----------------------------------
    int buildingCount = 0;
    if (!buildings.empty()) {
        const int gridN = 5; // 5 x 5 lots of 40 m
        const float lot = static_cast<float>(kChunkSize / gridN);
        for (int gx = 0; gx < gridN; ++gx) {
            for (int gy = 0; gy < gridN; ++gy) {
                if (!out.urban && !rng.Chance(0.35f))
                    continue;
                if (out.urban && rng.Chance(0.25f))
                    continue; // alleys, plazas, gaps
                if (buildingCount >= 14)
                    break;

                const ModelProto *proto = buildings[rng.Below(static_cast<uint32_t>(buildings.size()))];
                if (!proto)
                    continue;
                const float halfFp = std::max(proto->colHalfExtents.x, proto->colHalfExtents.y);
                if (halfFp * 2.0f > lot + 1.0f)
                    continue; // model cannot fit a lot, would overlap the neighbour

                const float jx = rng.RangeF(-3.0f, 3.0f);
                const float jy = rng.RangeF(-3.0f, 3.0f);
                const float lx = (static_cast<float>(gx) + 0.5f) * lot + jx;
                const float ly = (static_cast<float>(gy) + 0.5f) * lot + jy;
                if (!placeInside(lx, ly, halfFp))
                    continue;

                const double gxw = out.originGlobal.x + lx;
                const double gyw = out.originGlobal.y + ly;
                if (!HeightField::IsBuildable(gxw, gyw, 0.45f))
                    continue;

                ChunkProp p;
                p.modelId = proto->id;
                p.localPos = {lx, ly, HeightField::HeightAt(gxw, gyw)};
                p.headingRad = static_cast<float>(rng.RangeInt(0, 3)) * 1.5707963f;
                p.tag = PropTag::Building;
                p.halfFootprint = halfFp;
                out.props.push_back(p);
                ++buildingCount;

                // Doors: roughly every third building has an entrance with an
                // EnEx marker. Slots are stable per chunk.
                if (rng.Chance(0.55f) && out.doors.size() < 4) {
                    ChunkDoor d;
                    d.slot = static_cast<uint32_t>(out.doors.size());
                    d.seed = DoorSeed(chunkX, chunkY, d.slot);
                    const float side = rng.Chance(0.5f) ? -1.0f : 1.0f;
                    d.localPos = {lx + side * (halfFp - 0.6f), ly - (proto->colHalfExtents.y - 0.4f), p.localPos.z};
                    if (d.localPos.x > 2.0f && d.localPos.x < kChunkSize - 2.0f && d.localPos.y > 2.0f &&
                        d.localPos.y < kChunkSize - 2.0f) {
                        d.headingRad = side > 0.0f ? 1.5707963f : -1.5707963f;
                        d.shop = rng.Chance(0.4f);
                        out.doors.push_back(d);
                    }
                }
            }
        }
    }

    // --- street lights along the chunk's central road line ------------------
    if (out.urban && !lights.empty()) {
        const ModelProto *light = lights[rng.Below(static_cast<uint32_t>(lights.size()))];
        const float halfFp = std::max(light->colHalfExtents.x, light->colHalfExtents.y);
        for (int i = 0; i < 4; ++i) {
            const float lx = 20.0f + static_cast<float>(i) * (static_cast<float>(kChunkSize) - 40.0f) / 3.0f;
            const float ly = 8.0f + rng.RangeF(0.0f, 4.0f);
            if (!placeInside(lx, ly, halfFp))
                continue;
            const double gxw = out.originGlobal.x + lx;
            const double gyw = out.originGlobal.y + ly;
            if (!HeightField::IsBuildable(gxw, gyw, 0.6f))
                continue;
            ChunkProp p;
            p.modelId = light->id;
            p.localPos = {lx, ly, HeightField::HeightAt(gxw, gyw)};
            p.headingRad = 0.0f;
            p.tag = PropTag::StreetLight;
            p.halfFootprint = halfFp;
            out.props.push_back(p);
        }
    }

    // --- vegetation ----------------------------------------------------------
    if (!trees.empty()) {
        const int wanted = out.urban ? rng.RangeInt(0, 3) : rng.RangeInt(2, 9);
        for (int i = 0; i < wanted; ++i) {
            const ModelProto *tree = trees[rng.Below(static_cast<uint32_t>(trees.size()))];
            const float halfFp = std::max(1.0f, tree->colHalfExtents.x);
            const float lx = rng.RangeF(4.0f, static_cast<float>(kChunkSize) - 4.0f);
            const float ly = rng.RangeF(4.0f, static_cast<float>(kChunkSize) - 4.0f);
            if (!placeInside(lx, ly, halfFp))
                continue;
            const double gxw = out.originGlobal.x + lx;
            const double gyw = out.originGlobal.y + ly;
            if (HeightField::SlopeAt(gxw, gyw) > 0.9f)
                continue;
            ChunkProp p;
            p.modelId = tree->id;
            p.localPos = {lx, ly, HeightField::HeightAt(gxw, gyw)};
            p.headingRad = rng.RangeF(0.0f, 6.2831853f);
            p.tag = PropTag::Tree;
            p.halfFootprint = halfFp;
            out.props.push_back(p);
        }
    }

    // --- misc props / junk ---------------------------------------------------
    if (!props.empty()) {
        const int wanted = out.urban ? rng.RangeInt(1, 4) : rng.RangeInt(0, 2);
        for (int i = 0; i < wanted; ++i) {
            const ModelProto *prop = props[rng.Below(static_cast<uint32_t>(props.size()))];
            const float halfFp = std::max(prop->colHalfExtents.x, prop->colHalfExtents.y);
            const float lx = rng.RangeF(6.0f, static_cast<float>(kChunkSize) - 6.0f);
            const float ly = rng.RangeF(6.0f, static_cast<float>(kChunkSize) - 6.0f);
            if (!placeInside(lx, ly, halfFp))
                continue;
            const double gxw = out.originGlobal.x + lx;
            const double gyw = out.originGlobal.y + ly;
            ChunkProp p;
            p.modelId = prop->id;
            p.localPos = {lx, ly, HeightField::HeightAt(gxw, gyw)};
            p.headingRad = rng.RangeF(0.0f, 6.2831853f);
            p.tag = PropTag::Prop;
            p.halfFootprint = halfFp;
            out.props.push_back(p);
        }
    }

    // --- height range + required model list ---------------------------------
    out.heightMin = 1e9f;
    out.heightMax = -1e9f;
    for (int i = 0; i <= 4; ++i) {
        for (int j = 0; j <= 4; ++j) {
            const double hx = out.originGlobal.x + kChunkSize * (i / 4.0);
            const double hy = out.originGlobal.y + kChunkSize * (j / 4.0);
            const float h = HeightField::HeightAt(hx, hy);
            out.heightMin = std::min(out.heightMin, h);
            out.heightMax = std::max(out.heightMax, h);
        }
    }

    std::set<int> uniqueModels;
    for (const auto &p : out.props)
        uniqueModels.insert(p.modelId);
    out.requiredModels.assign(uniqueModels.begin(), uniqueModels.end());

    (void)region;
    return out;
}

} // namespace core
} // namespace usa1to1
