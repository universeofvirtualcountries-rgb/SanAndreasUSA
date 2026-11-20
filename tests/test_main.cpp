// Host test suite for every engine-neutral subsystem.
// Run: cmake --build build-host && ./build-host/usa1to1_tests
#include "mock_bridge.hpp"

#include "core/chunk_generator.hpp"
#include "core/chunk_streamer.hpp"
#include "core/config.hpp"
#include "core/floating_origin.hpp"
#include "core/hash.hpp"
#include "core/heightfield.hpp"
#include "core/interior.hpp"
#include "core/json.hpp"
#include "core/log.hpp"
#include "core/safe.hpp"
#include "core/spawn_validation.hpp"
#include "core/screen_to_render.hpp"
#include "core/status_feed.hpp"
#include "core/mission.hpp"
#include "core/model_catalog.hpp"
#include "core/regions.hpp"
#include "core/test_polygon.hpp"
#include "core/touch_gesture.hpp"
#include "core/world_delta.hpp"
#include "net/cloud_sync.hpp"
#include "ui/imgui_touch.hpp"
#include "plugin-sdk/renderware/imgui/imgui_shared.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef USA1TO1_SOURCE_DIR
#define USA1TO1_SOURCE_DIR "."
#endif

using namespace usa1to1;
using namespace usa1to1::core;
namespace fs = std::filesystem;

namespace {

int g_failures = 0;
int g_checks = 0;
std::string g_currentTest;

void Fail(const char *file, int line, const std::string &msg) {
    ++g_failures;
    std::printf("  FAIL [%s] %s:%d: %s\n", g_currentTest.c_str(), file, line, msg.c_str());
}

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        ++g_checks;                                                                                                    \
        if (!(cond))                                                                                                   \
            Fail(__FILE__, __LINE__, #cond);                                                                           \
    } while (0)

#define CHECK_MSG(cond, msg)                                                                                           \
    do {                                                                                                               \
        ++g_checks;                                                                                                    \
        if (!(cond))                                                                                                   \
            Fail(__FILE__, __LINE__, std::string(#cond) + " | " + (msg));                                              \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                                                          \
    do {                                                                                                               \
        ++g_checks;                                                                                                    \
        const double va = static_cast<double>(a);                                                                      \
        const double vb = static_cast<double>(b);                                                                      \
        if (std::fabs(va - vb) > (eps))                                                                                 \
            Fail(__FILE__, __LINE__,                                                                                   \
                 std::string(#a " ~ " #b " (") + std::to_string(va) + " vs " + std::to_string(vb) + ")");              \
    } while (0)

using TestFn = std::function<void()>;
struct TestCase {
    const char *name;
    TestFn fn;
};
std::vector<TestCase> &Tests() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(const char *name, TestFn fn) { Tests().push_back({name, std::move(fn)}); }
};

#define TEST(name)                                                                                                     \
    static void name();                                                                                                \
    static Registrar reg_##name(#name, name);                                                                          \
    static void name()

std::string SrcPath(const std::string &rel) { return std::string(USA1TO1_SOURCE_DIR) + "/" + rel; }

std::string ReadFile(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    std::string data;
    if (in) {
        std::stringstream ss;
        ss << in.rdbuf();
        data = ss.str();
    }
    return data;
}

// A catalogue with a few clearly fake ids: the tests must never depend on real
// game data, that is what the live scan and parse_ide.py are for.
ModelCatalog MakeTestCatalog() {
    ModelCatalog cat;
    auto add = [&](int id, const char *name, PropTag tag, float half, float height = 4.0f, bool road = false) {
        ModelProto p;
        p.id = id;
        p.name = name;
        p.tag = tag;
        p.colHalfExtents = {half, half, height};
        p.drawDistance = 250.0f;
        p.isRoad = road;
        cat.Add(p);
    };
    add(1001, "test_apartment_a", PropTag::Building, 6.0f, 12.0f);
    add(1002, "test_apartment_b", PropTag::Building, 5.0f, 9.0f);
    add(1003, "test_house_c", PropTag::Building, 4.0f, 7.0f);
    add(2001, "test_tree_a", PropTag::Tree, 1.5f, 5.0f);
    add(2002, "test_pine_b", PropTag::Tree, 1.8f, 6.0f);
    add(3001, "test_road_tile", PropTag::RoadFlat, 10.0f, 0.3f, true);
    add(4001, "test_wall", PropTag::Wall, 1.5f, 2.0f);
    add(4002, "test_floor", PropTag::Floor, 4.0f, 0.2f);
    add(5001, "test_counter", PropTag::Furniture, 1.0f, 1.0f);
    add(5002, "test_desk", PropTag::Furniture, 1.2f, 1.0f);
    add(6001, "test_lamp", PropTag::StreetLight, 0.4f, 3.0f);
    add(7001, "test_crate", PropTag::Prop, 0.6f, 0.6f);
    return cat;
}

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------

TEST(json_roundtrip_utf8_and_numbers) {
    const std::string text = R"({
        "id": "mission_001", "title": "Ограбление", "reward": 2500,
        "ratio": -0.125, "flag": true, "nothing": null,
        "list": [1, 2.5, "три", {"nested": false}],
        "escape": "quote\" backslash\\ tab\t newline\n"
    })";
    Json root = Json::Parse(text);
    CHECK(root.IsObject());
    CHECK(root.GetString("title", "") == "Ограбление");
    CHECK(root.GetInt("reward", 0) == 2500);
    CHECK_NEAR(root.GetDouble("ratio", 0.0), -0.125, 1e-12);
    CHECK(root.GetBool("flag", false));
    CHECK(root["list"].Size() == 4);
    CHECK(root["list"][2].AsString() == "три");
    CHECK(root["list"][3].GetBool("nested", true) == false);

    const std::string dumped = root.Dump(true);
    Json again = Json::Parse(dumped);
    CHECK(again.GetString("title", "") == "Ограбление");
    CHECK(again.GetString("escape", "") == root.GetString("escape", "x"));
    CHECK(again.Dump() == root.Dump());
}

TEST(json_rejects_garbage) {
    Json j;
    std::string err;
    CHECK(!Json::TryParse("{broken", j, &err));
    CHECK(!err.empty());
    CHECK(!Json::TryParse("{} trailing", j, &err));
    CHECK(!Json::TryParse("\"unterminated", j, &err));
    CHECK(!Json::TryParse("[1,2,]", j, &err));
    CHECK(Json::TryParse("[]", j) && j.IsArray());
}

TEST(json_ints_stay_ints) {
    Json root = Json::Parse(R"({"a": 42, "b": 42.5, "big": 9007199254740991})");
    CHECK(root["a"].GetType() == Json::Type::Int);
    CHECK(root["b"].GetType() == Json::Type::Double);
    CHECK(root["big"].AsInt() == 9007199254740991LL);
    CHECK(root.Dump().find("\"a\":42") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Hash / seeds
// ---------------------------------------------------------------------------

TEST(seeds_are_deterministic_and_unique) {
    CHECK(ChunkSeed(12, -34) == ChunkSeed(12, -34));
    CHECK(ChunkSeed(12, -34) != ChunkSeed(-34, 12));
    CHECK(ChunkSeed(0, 0) != ChunkSeed(1, 0));
    CHECK(DoorSeed(5, 6, 0) != DoorSeed(5, 6, 1));
    CHECK(InteriorSeed(DoorSeed(5, 6, 0)) != InteriorSeed(DoorSeed(5, 6, 1)));

    // mt19937_64 stream equality: the whole procedural world depends on it.
    Rng a(InteriorSeed(0xDEADBEEF));
    Rng b(InteriorSeed(0xDEADBEEF));
    for (int i = 0; i < 32; ++i)
        CHECK(a.Next64() == b.Next64());

    Rng c(12345);
    bool allInRange = true;
    for (int i = 0; i < 500; ++i) {
        const int v = c.RangeInt(3, 7);
        if (v < 3 || v > 7)
            allInRange = false;
    }
    CHECK(allInRange);
}

// ---------------------------------------------------------------------------
// Height field and chunk seams
// ---------------------------------------------------------------------------

TEST(heightfield_is_continuous) {
    const double x = 123456.0;
    const double y = 987654.0;
    CHECK(HeightField::HeightAt(x, y) == HeightField::HeightAt(x, y));
    CHECK(std::fabs(HeightField::HeightAt(x, y) - HeightField::HeightAt(x + 0.01, y)) < 1.0f);
    CHECK(HeightField::HeightAt(x, y) >= 0.0f);
    // Lattice corners are exact functions of integer coordinates.
    CHECK(HeightField::LatticeValue(10, -4) == HeightField::LatticeValue(10, -4));
}

TEST(chunk_edges_match_between_neighbours) {
    ChunkGenerator gen(MakeTestCatalog());
    for (int64_t cx = -6; cx <= 6; cx += 3) {
        for (int64_t cy = -6; cy <= 6; cy += 3) {
            const auto east = gen.SampleEdge(cx, cy, 1, 9);
            const auto west = gen.SampleEdge(cx + 1, cy, 0, 9);
            CHECK(east.size() == west.size());
            for (size_t i = 0; i < east.size(); ++i)
                CHECK_MSG(east[i] == west[i], "east edge of chunk != west edge of next");

            const auto north = gen.SampleEdge(cx, cy, 3, 9);
            const auto south = gen.SampleEdge(cx, cy + 1, 2, 9);
            for (size_t i = 0; i < north.size(); ++i)
                CHECK_MSG(north[i] == south[i], "north edge != south edge of next");
        }
    }
}

TEST(chunk_content_is_deterministic) {
    ChunkGenerator gen(MakeTestCatalog());
    const ChunkContent a = gen.Generate(2901, 5316);
    const ChunkContent b = gen.Generate(2901, 5316);
    CHECK(a.seed == b.seed);
    CHECK(a.props.size() == b.props.size());
    CHECK(a.doors.size() == b.doors.size());
    for (size_t i = 0; i < a.props.size(); ++i) {
        CHECK(a.props[i].modelId == b.props[i].modelId);
        CHECK(a.props[i].localPos.x == b.props[i].localPos.x);
        CHECK(a.props[i].localPos.y == b.props[i].localPos.y);
    }
    const ChunkContent other = gen.Generate(2902, 5316);
    CHECK(a.seed != other.seed);
}

TEST(chunk_props_never_cross_edges) {
    ChunkGenerator gen(MakeTestCatalog());
    int checkedProps = 0;
    for (int64_t cx = 100; cx < 108; ++cx) {
        for (int64_t cy = 200; cy < 208; ++cy) {
            const ChunkContent c = gen.Generate(cx, cy);
            for (const auto &p : c.props) {
                ++checkedProps;
                CHECK_MSG(p.localPos.x - p.halfFootprint >= -0.001f, "prop leaks over the west edge");
                CHECK_MSG(p.localPos.x + p.halfFootprint <= ChunkGenerator::kChunkSize + 0.001f, "east edge");
                CHECK_MSG(p.localPos.y - p.halfFootprint >= -0.001f, "south edge");
                CHECK_MSG(p.localPos.y + p.halfFootprint <= ChunkGenerator::kChunkSize + 0.001f, "north edge");
            }
            for (const auto &d : c.doors) {
                CHECK(d.localPos.x > 0.0f && d.localPos.x < ChunkGenerator::kChunkSize);
                CHECK(d.localPos.y > 0.0f && d.localPos.y < ChunkGenerator::kChunkSize);
                CHECK(d.seed == DoorSeed(cx, cy, d.slot));
            }
        }
    }
    CHECK(checkedProps > 50);
}

TEST(chunk_of_handles_negative_coordinates) {
    CHECK(ChunkGenerator::ChunkOf(Vec2d(0.0, 0.0)) == (ChunkCoord{0, 0}));
    CHECK(ChunkGenerator::ChunkOf(Vec2d(-0.5, -0.5)) == (ChunkCoord{-1, -1}));
    CHECK(ChunkGenerator::ChunkOf(Vec2d(199.9, 200.1)) == (ChunkCoord{0, 1}));
    CHECK(ChunkGenerator::Contains(ChunkCoord{0, 0}, Vec2d(0.0, 0.0)));
    CHECK(!ChunkGenerator::Contains(ChunkCoord{0, 0}, Vec2d(200.0, 0.0)));
}

// ---------------------------------------------------------------------------
// Floating origin
// ---------------------------------------------------------------------------

TEST(floating_origin_shift_moves_everything_once) {
    test::MockBridge bridge;
    FloatingOrigin::Config cfg;
    cfg.gridRadius = 2;
    FloatingOrigin origin(cfg);

    // World entities that must follow the shift.
    bridge.peds[1] = {Vec3f(400.0f, 10.0f, 40.0f), Vec3f(0.0f, 1.0f, 0.0f)};
    bridge.vehicles[2] = {Vec3f(390.0f, -20.0f, 40.0f), Vec3f(12.0f, 0.0f, 0.0f)};
    SpawnParams sp;
    sp.modelId = 1001;
    sp.pos = Vec3f(410.0f, 5.0f, 40.0f);
    const ObjectHandle worldObj = bridge.CreateObject(sp);
    sp.interior = true;
    const ObjectHandle interiorObj = bridge.CreateObject(sp);
    bridge.PlaceMarker(77, Vec3f(405.0f, 0.0f, 40.0f), 1.0f, false);
    bridge.PlaceMarker(88, Vec3f(405.0f, 0.0f, 1040.0f), 1.0f, true);

    bridge.player = Vec3f(450.0f, 20.0f, 40.0f);
    bridge.cameraPos = Vec3f(440.0f, 10.0f, 45.0f);
    const Vec3f speedBefore = bridge.playerVelocity;

    const auto result = origin.Update(bridge);
    CHECK(result.shifted);
    CHECK(result.invariantHeld);
    // trunc(450/200) = 2 chunks -> -400 m; trunc(20/200) = 0 -> no Y shift.
    CHECK_NEAR(result.delta.x, -400.0f, 1e-4);
    CHECK_NEAR(result.delta.y, 0.0f, 1e-4);
    CHECK_NEAR(bridge.player.x, 50.0f, 1e-3);
    CHECK_NEAR(bridge.cameraPos.x, 40.0f, 1e-3);

    // Velocities are sacrosanct.
    CHECK_NEAR(bridge.playerVelocity.x, speedBefore.x, 1e-6);
    CHECK_NEAR(bridge.playerVelocity.y, speedBefore.y, 1e-6);
    CHECK_NEAR(bridge.peds[1].velocity.y, 1.0f, 1e-6);
    CHECK_NEAR(bridge.vehicles[2].velocity.x, 12.0f, 1e-6);

    // World entities and their markers moved, interior ones did not.
    CHECK_NEAR(bridge.objects[worldObj.raw].pos.x, 10.0f, 1e-3);
    CHECK_NEAR(bridge.objects[interiorObj.raw].pos.x, 410.0f, 1e-3);
    CHECK_NEAR(bridge.markers[77].pos.x, 5.0f, 1e-3);
    CHECK_NEAR(bridge.markers[88].pos.x, 405.0f, 1e-3);
    CHECK_NEAR(bridge.peds[1].pos.x, 0.0f, 1e-3);

    // Offset bookkeeping: global = offset + local must be preserved.
    CHECK_NEAR(origin.Offset().x, 400.0, 1e-6);
    const Vec3f local{50.0f, 20.0f, 40.0f};
    CHECK_NEAR(origin.ToGlobal(local).x, 450.0, 1e-3);
    CHECK(origin.TotalShifts() == 1);
}

TEST(floating_origin_keeps_grid_inside_limit) {
    test::MockBridge bridge;
    FloatingOrigin::Config cfg;
    cfg.gridRadius = 2;
    cfg.chunkSize = 200.0;
    cfg.localLimit = 1000.0;
    FloatingOrigin origin(cfg);
    bridge.player = Vec3f(0.0f, 0.0f, 40.0f);

    double globalY = 0.0;
    const double step = 17.0;
    double worstLocal = 0.0;
    for (int i = 0; i < 2000; ++i) {
        globalY += step;
        // Simulate movement in local space (the player walks, the world stands).
        bridge.player.y += static_cast<float>(step);
        origin.Update(bridge);

        const double limit = 1000.0;
        const double gridExtent = origin.MaxGridExtent();
        const double localAbs = std::max(std::fabs(bridge.player.x), std::fabs(bridge.player.y));
        worstLocal = std::max(worstLocal, localAbs);
        CHECK_MSG(localAbs + gridExtent <= limit,
                  "active grid leaves +/-1000 (local=" + std::to_string(localAbs) + ")");

        // Global position must equal offset + local at all times.
        const Vec3f localNow = bridge.player;
        CHECK_NEAR(origin.ToGlobal(localNow).y, globalY, 0.05);
    }
    CHECK(worstLocal <= origin.TriggerThreshold() + 1e-3);
    CHECK(origin.TotalShifts() > 50);
    CHECK(origin.MaxGridExtent() == 600.0);
    CHECK_NEAR(origin.TriggerThreshold(), 350.0, 1e-9);
}

TEST(floating_origin_teleport_is_shift_free) {
    test::MockBridge bridge;
    FloatingOrigin origin;
    bridge.player = Vec3f(120.0f, -60.0f, 40.0f);
    const int shiftsBefore = bridge.shiftPlayerCalls;

    const auto city = FindCity("New York");
    CHECK(city != nullptr);
    origin.TeleportPlayer(bridge, Vec2d(city->x, city->y));

    CHECK(bridge.shiftPlayerCalls == shiftsBefore); // no entity was touched
    CHECK_NEAR(bridge.player.x, 120.0f, 1e-3);
    CHECK_NEAR(origin.ToGlobal(bridge.player).x, city->x, 0.5);
    CHECK_NEAR(origin.ToGlobal(bridge.player).y, city->y, 0.5);
}

TEST(floating_origin_ignores_interior_pocket) {
    test::MockBridge bridge;
    FloatingOrigin origin;
    bridge.player = Vec3f(900.0f, 900.0f, 1045.0f); // inside an interior
    const auto result = origin.Update(bridge);
    CHECK(!result.shifted);
    CHECK(bridge.shiftPlayerCalls == 0);
}

TEST(compute_shift_chunks_is_exact) {
    CHECK(FloatingOrigin::ComputeShiftChunks(100.0, 200.0, 350.0) == 0);
    CHECK(FloatingOrigin::ComputeShiftChunks(-100.0, 200.0, 350.0) == 0);
    CHECK(FloatingOrigin::ComputeShiftChunks(450.0, 200.0, 350.0) == 2);
    CHECK(FloatingOrigin::ComputeShiftChunks(-450.0, 200.0, 350.0) == -2);
    CHECK(FloatingOrigin::ComputeShiftChunks(399.0, 200.0, 350.0) == 1);
    CHECK(FloatingOrigin::ComputeShiftChunks(1e6, 200.0, 350.0) == 5000);
}

// ---------------------------------------------------------------------------
// Chunk streaming
// ---------------------------------------------------------------------------

TEST(streamer_loads_grid_and_unloads_far_chunks) {
    test::MockBridge bridge;
    const ModelCatalog cat = MakeTestCatalog();
    ChunkGenerator gen(cat);
    ChunkStreamer::Config cfg;
    cfg.radius = 1;
    cfg.maxChunksPerUpdate = 9;
    cfg.maxSpawnsPerUpdate = 500;
    ChunkStreamer streamer(bridge, gen, cfg);

    const Vec2d origin(0.0, 0.0);
    streamer.Update(Vec2d(1000.0, 1000.0), origin);
    CHECK(streamer.ActiveChunkCount() == 9);
    CHECK(streamer.IsFullyLoadedAround(Vec2d(1000.0, 1000.0)));
    CHECK(streamer.ObjectCount() > 0);
    CHECK(streamer.PendingCount() == 0);
    CHECK(bridge.markers.size() > 0);

    // Objects must sit at their global chunk position minus the origin.
    for (const auto &kv : streamer.Active()) {
        const Vec2d chunkOrigin = ChunkGenerator::ChunkOriginGlobal(kv.first.first, kv.first.second);
        CHECK_NEAR(kv.second.seed, ChunkSeed(kv.first.first, kv.first.second), 1e-9);
        CHECK(chunkOrigin.x >= 800.0 && chunkOrigin.x <= 1200.0);
    }

    // Walk far away: every old chunk must be gone, nothing leaks.
    streamer.Update(Vec2d(1000.0 + 4000.0, 1000.0), origin);
    for (const auto &kv : streamer.Active()) {
        const int64_t dx = kv.first.first - static_cast<int64_t>(std::floor((1000.0 + 4000.0) / 200.0));
        CHECK(std::llabs(dx) <= 1);
    }
    CHECK(bridge.destroyed > 0);
    CHECK(bridge.markersRemoved > 0);
    CHECK(bridge.objects.size() == streamer.ObjectCount());

    streamer.Shutdown();
    CHECK(streamer.ActiveChunkCount() == 0);
    CHECK(bridge.objects.empty());
    CHECK(bridge.markers.empty());
}

TEST(streamer_retries_props_until_models_load) {
    // A bridge that keeps models unloaded until we say otherwise.
    class SlowBridge : public test::MockBridge {
      public:
        bool allowLoads = false;
        bool IsModelLoaded(int id) override { return allowLoads && loadedModels.count(id) > 0; }
        bool RequestModel(int id, bool) override {
            requestedModels.insert(id);
            loadedModels.insert(id);
            return true;
        }
    };
    SlowBridge bridge;
    const ModelCatalog cat = MakeTestCatalog();
    ChunkGenerator gen(cat);
    ChunkStreamer::Config cfg;
    cfg.radius = 0;
    cfg.maxSpawnsPerUpdate = 100;
    ChunkStreamer streamer(bridge, gen, cfg);

    streamer.Update(Vec2d(100.0, 100.0), Vec2d(0.0, 0.0));
    CHECK(streamer.PendingCount() > 0);
    CHECK(streamer.ObjectCount() == 0);

    // Models arrive now: the pending props must be spawned on the next update
    // instead of being silently dropped.
    bridge.allowLoads = true;
    streamer.Update(Vec2d(100.0, 100.0), Vec2d(0.0, 0.0));
    CHECK(streamer.PendingCount() == 0);
    CHECK(streamer.ObjectCount() > 0);
}

TEST(streamer_keeps_prop_if_readiness_is_lost_during_creation) {
    class FlakyBridge : public test::MockBridge {
      public:
        bool failOnce = true;
        int staleId = -1;
        ObjectHandle CreateObject(const SpawnParams &params) override {
            if (failOnce) {
                failOnce = false;
                staleId = params.modelId;
                loadedModels.erase(staleId);
                return {};
            }
            return MockBridge::CreateObject(params);
        }
    } bridge;
    ChunkGenerator gen(MakeTestCatalog());
    ChunkStreamer::Config cfg;
    cfg.radius = 0;
    cfg.maxSpawnsPerUpdate = 100;
    ChunkStreamer streamer(bridge, gen, cfg);
    streamer.Update({100, 100}, {});
    const size_t pending = streamer.PendingCount(), spawned = streamer.ObjectCount();
    CHECK(bridge.staleId >= 0 && pending > 0 && bridge.flushes == 0);
    bridge.loadedModels.insert(bridge.staleId);
    streamer.Update({100, 100}, {});
    CHECK(streamer.PendingCount() == 0);
    CHECK(streamer.ObjectCount() == spawned + pending && bridge.flushes == 0);
}

TEST(streamer_finds_door_marker) {
    test::MockBridge bridge;
    const ModelCatalog cat = MakeTestCatalog();
    ChunkGenerator gen(cat);
    ChunkStreamer::Config cfg;
    cfg.radius = 1;
    cfg.maxChunksPerUpdate = 9;
    ChunkStreamer streamer(bridge, gen, cfg);
    streamer.Update(Vec2d(1000.0, 1000.0), Vec2d(0.0, 0.0));
    CHECK(!bridge.markers.empty());

    const auto it = bridge.markers.begin();
    const Vec3f doorPos = it->second.pos;
    const auto hit = streamer.FindDoorAt(doorPos, 1.6f);
    CHECK(hit.valid);
    CHECK(hit.markerId == it->first);
    CHECK(hit.door.seed != 0);

    // Far away: no hit.
    CHECK(!streamer.FindDoorAt(Vec3f(doorPos.x + 50.0f, doorPos.y, doorPos.z), 1.6f).valid);

    // After a shift both the world marker and the cached door position move in
    // lockstep, so the door stays under the player.
    const Vec3f delta(-200.0f, 0.0f, 0.0f);
    bridge.ShiftStreamedEntities(delta, false);
    streamer.OnOriginShift(delta);
    const auto hit2 = streamer.FindDoorAt(Vec3f(doorPos.x - 200.0f, doorPos.y, doorPos.z), 1.6f);
    CHECK(hit2.valid);
}

// ---------------------------------------------------------------------------
// Interiors
// ---------------------------------------------------------------------------

TEST(interior_layout_is_deterministic_and_connected) {
    const ModelCatalog cat = MakeTestCatalog();
    InteriorGenerator gen(cat);
    for (uint64_t seed = 1; seed <= 250; ++seed) {
        const uint64_t doorSeed = DoorSeed(static_cast<int64_t>(seed), static_cast<int64_t>(seed * 7), 1);
        const int area = InteriorGenerator::AreaIdForSeed(doorSeed);
        const float z = InteriorGenerator::PocketZForArea(area);
        const InteriorLayout a = gen.Generate(doorSeed, area, z);
        const InteriorLayout b = gen.Generate(doorSeed, area, z);

        CHECK(a.RoomCount() >= 3 && a.RoomCount() <= 7);
        CHECK(b.RoomCount() == a.RoomCount());
        CHECK(a.props.size() == b.props.size());
        CHECK(InteriorGenerator::IsConnected(a));
        CHECK(area >= 1 && area <= InteriorGenerator::kMaxAreaId);
        CHECK(z >= kInteriorBaseZ);
        CHECK(a.hasRegister);
        CHECK(a.playerSpawn.z >= kInteriorBaseZ);
        CHECK(a.exitMarker.z >= kInteriorBaseZ);
        for (const auto &p : a.props)
            CHECK(p.localPos.z >= kInteriorBaseZ);
        CHECK(a.requiredModels.size() > 0);
    }
}

TEST(interior_enter_exit_and_robbery) {
    test::MockBridge bridge;
    const ModelCatalog cat = MakeTestCatalog();
    InteriorGenerator gen(cat);
    InteriorManager manager(bridge, gen);
    WorldDelta delta;

    const uint64_t doorSeed = DoorSeed(2901, 5316, 2);
    const Vec3f doorPos(10.0f, 10.0f, 40.0f);
    CHECK(manager.Enter(doorSeed, doorPos, delta));
    CHECK(manager.IsInside());
    CHECK(bridge.playerInterior != 0);
    CHECK(bridge.player.z >= kInteriorBaseZ);
    CHECK(bridge.CountInteriorObjects() > 0);
    CHECK(bridge.markers.size() == 1);
    CHECK(bridge.markers.begin()->second.interior);

    // Interior objects must survive a world shift untouched.
    const size_t before = bridge.CountInteriorObjects();
    bridge.ShiftStreamedEntities(Vec3f(-400.0f, 0.0f, 0.0f), false);
    CHECK(bridge.CountInteriorObjects() == before);
    for (const auto &kv : bridge.objects)
        if (kv.second.interior)
            CHECK(kv.second.pos.z >= kInteriorBaseZ);

    // Rob the register.
    bridge.player = manager.Current().registerPos;
    const auto ev = manager.Update(bridge.player, delta);
    CHECK(ev == InteriorManager::Event::Robbed);
    const int64_t reward = manager.TakeRobberyReward();
    CHECK(reward > 0);
    CHECK(delta.money == reward);
    CHECK(delta.Door(doorSeed).robbed);

    // Second visit: no double dipping.
    bridge.player = manager.Current().registerPos;
    CHECK(manager.Update(bridge.player, delta) == InteriorManager::Event::None);

    // Exit marker teleports back in front of the door.
    bridge.player = manager.Current().exitMarker;
    CHECK(manager.Update(bridge.player, delta) == InteriorManager::Event::Exited);
    CHECK(!manager.IsInside());
    CHECK_NEAR(bridge.player.x, doorPos.x, 1e-3);
    CHECK(bridge.playerInterior == 0);
    CHECK(bridge.CountInteriorObjects() == 0);
    CHECK(bridge.markers.empty());
}

// ---------------------------------------------------------------------------
// World delta
// ---------------------------------------------------------------------------

TEST(world_delta_roundtrip_and_rules) {
    WorldDelta d;
    d.SetPlayerPosition(1234567.5, -987654.25, 40.0f);
    d.money = 1000;
    CHECK(d.RobDoor(0xABCDEF, 300));
    CHECK(!d.RobDoor(0xABCDEF, 300)); // already robbed
    CHECK(d.money == 1300);
    CHECK(d.BuyHouse(0x1234, 500));
    CHECK(d.money == 800);
    CHECK(!d.BuyHouse(0x1234, 500)); // already owned
    CHECK(!d.BuyHouse(0x9999, 5000)); // not enough cash
    d.SetMissionStatus("mission_001_robbery", "active", 2);

    const fs::path tmp = fs::temp_directory_path() / "usa1to1_delta_test.json";
    std::string err;
    CHECK_MSG(d.SaveToFile(tmp.string(), &err), err);
    WorldDelta loaded;
    CHECK_MSG(loaded.LoadFromFile(tmp.string(), &err), err);
    CHECK_NEAR(loaded.playerGlobalX, 1234567.5, 1e-6);
    CHECK_NEAR(loaded.playerGlobalY, -987654.25, 1e-6);
    CHECK(loaded.money == 800);
    CHECK(loaded.FindDoor(0xABCDEF) && loaded.FindDoor(0xABCDEF)->robbed);
    CHECK(loaded.FindDoor(0x1234) && loaded.FindDoor(0x1234)->owned);
    CHECK(loaded.FindMission("mission_001_robbery")->step == 2);
    fs::remove(tmp);
}

TEST(world_delta_is_compact) {
    WorldDelta d;
    d.SetPlayerPosition(1.0, 2.0, 3.0);
    const std::string json = d.ToJson().Dump();
    CHECK(json.size() < 200);
    CHECK(json.find("version") != std::string::npos);
    CHECK(json.find("doors") == std::string::npos); // no empty noise
}

TEST(world_delta_load_missing_file_is_fresh_start) {
    WorldDelta d = WorldDelta::Load("/nonexistent/path/world_delta.json");
    CHECK(d.money == 0);
    CHECK(!d.hasPlayerPos);
}

// ---------------------------------------------------------------------------
// Missions
// ---------------------------------------------------------------------------

TEST(mission_files_parse_and_validate) {
    MissionManager manager;
    const int loaded = manager.LoadDirectory(SrcPath("assets/missions"), nullptr);
    CHECK(loaded == 2);
    const MissionDef *m1 = manager.Find("mission_001_robbery");
    const MissionDef *m2 = manager.Find("mission_002_presidential_hit");
    CHECK(m1 && m2);
    CHECK(m1->steps.size() == 3);
    CHECK(m1->steps[1].type == MissionStepType::RobDoor);
    CHECK(m1->steps[1].doorSeed == 0);
    CHECK(m1->reward == 2500);
    CHECK(m2->steps.size() == 4);
    CHECK(m2->steps[2].type == MissionStepType::Wait);
    CHECK_NEAR(m2->steps[2].seconds, 8.0, 1e-9);
    CHECK(m2->region == "RM");
    // The bundled anchors must land inside the US plane and inside their state.
    const RegionBox *rm = FindRegion(m2->start.x, m2->start.y);
    CHECK(rm != nullptr);
    CHECK(std::string(rm->code) == "RM");
    const RegionBox *ca = FindRegion(m1->start.x, m1->start.y);
    CHECK(ca != nullptr);
    CHECK(std::string(ca->code) == "CA");
}

TEST(mission_runner_full_playthrough) {
    MissionManager manager;
    manager.LoadDirectory(SrcPath("assets/missions"), nullptr);
    const MissionDef *def = manager.Find("mission_001_robbery");
    CHECK(def != nullptr);

    WorldDelta delta;
    MissionRunner runner;
    CHECK(runner.Start(*def, delta));
    CHECK(runner.IsActive());
    CHECK(delta.FindMission(def->id)->status == "active");

    // Step 0: reach the store.
    CHECK(runner.Update(def->steps[0].target, delta, 1.0) == MissionRunner::Result::StepCompleted);
    CHECK(runner.StepIndex() == 1);

    // Step 1: rob any register - only the door manager can report that.
    CHECK(runner.Update(def->steps[0].target, delta, 2.0) == MissionRunner::Result::None);
    delta.RobDoor(0xFEEDFACE, 400);
    runner.NotifyDoorRobbed(0xFEEDFACE);
    CHECK(runner.Update(def->steps[0].target, delta, 3.0) == MissionRunner::Result::StepCompleted);

    // Step 2: return to the meeting point and finish.
    const int64_t moneyBefore = delta.money;
    CHECK(runner.Update(def->steps[2].target, delta, 4.0) == MissionRunner::Result::MissionCompleted);
    CHECK(!runner.IsActive());
    CHECK(delta.money == moneyBefore + def->reward);
    CHECK(delta.FindMission(def->id)->status == "completed");

    // A completed mission cannot be restarted.
    CHECK(!runner.Start(*def, delta));
}

TEST(mission_runner_wait_step_needs_time) {
    MissionManager manager;
    manager.LoadDirectory(SrcPath("assets/missions"), nullptr);
    const MissionDef *def = manager.Find("mission_002_presidential_hit");
    CHECK(def != nullptr);

    WorldDelta delta;
    MissionRunner runner;
    CHECK(runner.Start(*def, delta));
    CHECK(runner.Update(def->steps[0].target, delta, 0.0) == MissionRunner::Result::StepCompleted);
    CHECK(runner.Update(def->steps[1].target, delta, 0.5) == MissionRunner::Result::StepCompleted);

    // Wait step: not done until 8 seconds have passed at the target.
    CHECK(runner.Update(def->steps[2].target, delta, 1.0) == MissionRunner::Result::None);
    CHECK(runner.Update(def->steps[2].target, delta, 8.5) == MissionRunner::Result::None ||
          runner.StepIndex() == 3);
    CHECK(runner.StepIndex() == 3);
    CHECK(runner.Update(Vec2d(0.0, 0.0), delta, 20.0) == MissionRunner::Result::None);
    CHECK(runner.Update(def->steps[3].target, delta, 21.0) == MissionRunner::Result::MissionCompleted);
}

TEST(mission_abort_restores_available_state) {
    MissionManager manager;
    manager.LoadDirectory(SrcPath("assets/missions"), nullptr);
    WorldDelta delta;
    MissionRunner runner;
    CHECK(runner.Start(*manager.Find("mission_001_robbery"), delta));
    runner.Abort(delta);
    CHECK(!runner.IsActive());
    CHECK(delta.FindMission("mission_001_robbery")->status == "available");
    CHECK(delta.FindMission("mission_001_robbery")->step == 0);
}

// ---------------------------------------------------------------------------
// Cloud sync
// ---------------------------------------------------------------------------

namespace fake {
class FakeTransport : public net::IHttpTransport {
  public:
    std::map<std::string, std::string> files;
    std::set<std::string> failUrls;

    bool Get(const std::string &url, std::string &out, int) override {
        if (failUrls.count(url))
            return false;
        auto it = files.find(url);
        if (it == files.end())
            return false;
        out = it->second;
        return true;
    }
    bool Download(const std::string &url, const std::string &path, int) override {
        if (failUrls.count(url))
            return false;
        auto it = files.find(url);
        if (it == files.end())
            return false;
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << it->second;
        return out.good();
    }
};

std::string ManifestFor(const std::string &id, const std::string &content, int version) {
    Json root = Json::Object();
    root.Set("version", 1);
    Json arr = Json::MakeArray();
    Json e = Json::Object();
    e.Set("id", id);
    e.Set("file", "missions/" + id + ".json");
    e.Set("url", "https://fake/" + id + ".json");
    e.Set("version", version);
    e.Set("size", static_cast<int64_t>(content.size()));
    e.Set("hash", net::HashHex(content));
    arr.Push(e);
    root.Set("missions", arr);
    return root.Dump();
}
} // namespace fake

TEST(cloud_sync_downloads_new_and_skips_unchanged) {
    const fs::path root = fs::temp_directory_path() / "usa1to1_cloud_test";
    fs::remove_all(root);
    fs::create_directories(root / "missions");

    const std::string missionId = "mission_cloud_001";
    const std::string content = R"({"id":"mission_cloud_001","title":"From the cloud","steps":[{"type":"goto","target":{"x":1,"y":2}}]})";
    const std::string manifestUrl = "https://fake/manifest.json";

    fake::FakeTransport transport;
    transport.files[manifestUrl] = fake::ManifestFor(missionId, content, 1);
    transport.files["https://fake/" + missionId + ".json"] = content;

    net::CloudSync sync(transport, (root / "missions").string(), (root / "cloud_state.json").string());
    sync.SetManifestUrl(manifestUrl);
    sync.SetMissionsBaseUrl("https://fake/");

    auto result = sync.SyncNow();
    CHECK(result.updated == 1);
    CHECK(result.failed == 0);
    CHECK(fs::exists(root / "missions" / (missionId + ".json")));
    CHECK(ReadFile((root / "missions" / (missionId + ".json")).string()) == content);

    // Second run: nothing to do.
    auto again = sync.SyncNow();
    CHECK(again.updated == 0);
    CHECK(again.state == net::CloudSync::State::NothingToDo);

    // New content + version: re-download.
    const std::string content2 = content + " ";
    transport.files["https://fake/" + missionId + ".json"] = content2;
    transport.files[manifestUrl] = fake::ManifestFor(missionId, content2, 2);
    auto third = sync.SyncNow();
    CHECK(third.updated == 1);
    CHECK(ReadFile((root / "missions" / (missionId + ".json")).string()) == content2);
    CHECK(sync.LastUpdatedIdsCount() == 1);

    fs::remove_all(root);
}

TEST(cloud_sync_handles_failures_and_hash_mismatch) {
    const fs::path root = fs::temp_directory_path() / "usa1to1_cloud_fail";
    fs::remove_all(root);
    const std::string manifestUrl = "https://fake/manifest.json";
    const std::string id = "mission_bad";

    fake::FakeTransport transport;
    // Manifest promises one hash, the file served has another: must be rejected.
    std::string manifest = fake::ManifestFor(id, "original", 1);
    transport.files[manifestUrl] = manifest;
    transport.files["https://fake/" + id + ".json"] = "tampered";

    net::CloudSync sync(transport, (root / "missions").string(), (root / "state.json").string());
    sync.SetManifestUrl(manifestUrl);
    sync.SetMissionsBaseUrl("https://fake/");
    auto result = sync.SyncNow();
    CHECK(result.updated == 0);
    CHECK(result.failed == 1);
    CHECK(!fs::exists(root / "missions" / (id + ".json")));

    // Manifest itself unreachable.
    transport.failUrls.insert(manifestUrl);
    auto failed = sync.SyncNow();
    CHECK(failed.state == net::CloudSync::State::Error);
    CHECK(!failed.error.empty());

    fs::remove_all(root);
}

TEST(cloud_sync_manifest_rejects_garbage) {
    net::RemoteManifest manifest;
    std::string err;
    CHECK(!manifest.Parse("{not json", &err));
    CHECK(!err.empty());
    CHECK(manifest.Parse(R"({"version":1,"missions":[{"id":"a","file":"missions/a.json","version":1}]})"));
    CHECK(manifest.missions.size() == 1);
    CHECK(net::HashHex("abc") == net::HashHex("abc"));
    CHECK(net::HashHex("abc") != net::HashHex("abd"));
    CHECK(net::HashHex("abc").size() == 16);
}

// ---------------------------------------------------------------------------
// Gesture
// ---------------------------------------------------------------------------

TEST(two_finger_swipe_down_triggers_once) {
    TwoFingerSwipeDetector gesture;
    CHECK(!gesture.Feed(0, 0, 500, 100, 0.0));
    CHECK(!gesture.Feed(0, 1, 700, 120, 0.01));
    // Both fingers travel 200 px down within 120 ms.
    CHECK(!gesture.Feed(2, 0, 505, 300, 0.10));
    CHECK(gesture.Feed(2, 1, 705, 320, 0.11));
    CHECK(gesture.TriggerCount() == 1);
    // Cooldown: the same gesture cannot fire twice.
    CHECK(!gesture.Feed(2, 0, 505, 330, 0.12));
    CHECK(gesture.TriggerCount() == 1);
}

TEST(single_finger_and_horizontal_do_not_trigger) {
    TwoFingerSwipeDetector gesture;
    for (int i = 0; i < 10; ++i)
        CHECK(!gesture.Feed(i == 0 ? 0 : 2, 0, 540, 100 + i * 40, i * 0.05));

    TwoFingerSwipeDetector horizontal;
    CHECK(!horizontal.Feed(0, 0, 100, 500, 0.0));
    CHECK(!horizontal.Feed(0, 1, 100, 700, 0.0));
    CHECK(!horizontal.Feed(2, 0, 700, 520, 0.1));
    CHECK(!horizontal.Feed(2, 1, 800, 760, 0.1));
    CHECK(horizontal.TriggerCount() == 0);
}

TEST(swipe_is_scaled_for_small_screens) {
    TwoFingerSwipeDetector::Config cfg;
    cfg.screenShortSide = 720.0f;
    TwoFingerSwipeDetector gesture(cfg);
    gesture.SetScreenSize(1280.0f, 720.0f);
    CHECK(!gesture.Feed(0, 0, 300, 100, 0.0));
    CHECK(!gesture.Feed(0, 1, 500, 100, 0.0));
    // 135 px is enough on a 720p screen (180 * 720/1080 = 120)...
    CHECK(!gesture.Feed(2, 0, 300, 235, 0.1));
    // ...but only once BOTH fingers moved.
    CHECK(gesture.Feed(2, 1, 500, 235, 0.1));
    CHECK(gesture.TriggerCount() == 1);
}

// ---------------------------------------------------------------------------
// Test polygon
// ---------------------------------------------------------------------------

TEST(screen_to_render_scales_both_axes_and_keeps_outside_coordinates) {
    constexpr auto p = ScreenToRender(1200.0f, 100.0f, 2400, 1080, 1600, 540);
    static_assert(p.validDimensions);
    CHECK_NEAR(p.x, 800.0f, 0.001);
    CHECK_NEAR(p.y, 50.0f, 0.001);
    CHECK_NEAR(p.scaleX, 2.0f / 3.0f, 0.001);
    CHECK_NEAR(p.scaleY, 0.5f, 0.001);
    constexpr auto outside = ScreenToRender(-10.0f, 1200.0f, 2400, 1080, 1200, 540);
    CHECK(outside.x == -5.0f && outside.y == 600.0f);
    constexpr auto identity = ScreenToRender(10.0f, 30.0f, 1200, 540, 1200, 540);
    CHECK(identity.validDimensions && identity.x == 10.0f && identity.y == 30.0f);
}

TEST(screen_to_render_falls_back_when_any_dimension_is_missing) {
    for (const auto &p : {ScreenToRender(42.0f, 12.0f, 0, 100, 48, 50),
                          ScreenToRender(42.0f, 12.0f, 20, -1, 48, 50),
                          ScreenToRender(42.0f, 12.0f, 20, 100, 0, 50),
                          ScreenToRender(42.0f, 12.0f, 20, 100, 48, 0)}) {
        CHECK(!p.validDimensions);
        CHECK(p.x == 42.0f && p.y == 12.0f);
        CHECK(p.scaleX == 1.0f && p.scaleY == 1.0f);
    }
}

TEST(touch_screen_size_prefers_override_then_surface_and_falls_back_on_zeros) {
    constexpr PixelSize render{1200, 540}, viewport{1600, 720}, surface{2400, 1080}, configured{3200, 1440};
    constexpr auto manual = ResolveTouchScreenSize(configured, surface, viewport, render);
    CHECK(manual.width == 3200 && manual.height == 1440);
    constexpr auto autoSurface = ResolveTouchScreenSize({3200, 0}, surface, viewport, render);
    CHECK(autoSurface.width == 2400 && autoSurface.height == 1080);
    constexpr auto autoViewport = ResolveTouchScreenSize({}, {0, 1080}, viewport, render);
    CHECK(autoViewport.width == 1600 && autoViewport.height == 720);
    constexpr auto autoRender = ResolveTouchScreenSize({}, {}, {1600, -1}, render);
    CHECK(autoRender.width == 1200 && autoRender.height == 540);
    CHECK(!ResolveTouchScreenSize({}, {}, {}, {}).Valid());
}

TEST(config_touch_screen_override_roundtrip_and_invalid_pairs) {
    ModConfig cfg;
    CHECK(!cfg.screenOverride.Valid());
    CHECK(cfg.ToJson()["ui"].GetInt("screen_width", -1) == 0);
    CHECK(cfg.ToJson()["ui"].GetInt("screen_height", -1) == 0);
    CHECK(cfg.FromJson(Json::Parse(R"({"ui":{"screen_width":2400,"screen_height":1080}})")));
    const auto json = cfg.ToJson();
    ModConfig roundtrip;
    CHECK(roundtrip.FromJson(json));
    CHECK(roundtrip.screenOverride.width == 2400 && roundtrip.screenOverride.height == 1080);
    for (const char *ui : {R"({"screen_width":2400})", R"({"screen_width":2400,"screen_height":0})",
                           R"({"screen_width":-1,"screen_height":1080})",
                           R"({"screen_width":2400.5,"screen_height":1080})",
                           R"({"screen_width":1e90,"screen_height":1080})"}) {
        ModConfig invalid;
        CHECK(invalid.FromJson(Json::Parse(std::string("{\"ui\":") + ui + "}")));
        CHECK(invalid.screenOverride.width == 0 && invalid.screenOverride.height == 0);
    }
}

TEST(imgui_routes_screen_pixel_tap_to_render_pixel_pill) {
    ImGuiShared::Coordinator coordinator;
    std::vector<ImGuiShared::Touch> events;
    const auto client = coordinator.Add({&events, [](void *ctx, const ImGuiShared::Touch *touches, uint32_t count,
                                                    int32_t, int32_t, float) {
        if (count)
            static_cast<std::vector<ImGuiShared::Touch> *>(ctx)->assign(touches, touches + count);
    }, [](void *) {}});
    const ImGuiShared::Rect pill{541.0f, 12.0f, 659.0f, 48.0f};
    coordinator.SetRects(client, &pill, 1);
    coordinator.Frame(1200, 540, 1.0f / 60.0f, 2400, 1080);
    CHECK(!coordinator.ProcessTouch(2, 0, 15, 15));
    CHECK(coordinator.ProcessTouch(2, 0, 1200, 48));
    CHECK(coordinator.ProcessTouch(1, 0, 1400, 48)); // release outside is still captured
    coordinator.Frame(1200, 540, 1.0f / 60.0f, 2400, 1080);
    CHECK(events.size() == 2);
    if (events.size() == 2) {
        CHECK(events[0].type == 2 && events[0].x == 600.0f && events[0].y == 24.0f);
        CHECK(events[1].type == 1 && events[1].x == 700.0f);
    }
    const auto sample = coordinator.LastTouch();
    CHECK(sample.rawX == 1400.0f && sample.x == 700.0f && sample.scaleX == 0.5f);
    CHECK(sample.screenW == 2400 && sample.screenH == 1080 && sample.renderW == 1200 && sample.renderH == 540);
}

TEST(imgui_touch_dimensions_follow_frames_and_reset_to_raw) {
    ImGuiShared::Coordinator coordinator;
    CHECK(!coordinator.ProcessTouch(2, 0, 600.0f, 48.0f));
    CHECK(coordinator.LastTouch().x == 600.0f && coordinator.LastTouch().screenW == 0);
    coordinator.Frame(1200, 540, 1.0f / 60.0f, 2400, 1080);
    coordinator.ProcessTouch(3, 0, 600.0f, 48.0f);
    CHECK(coordinator.LastTouch().x == 300.0f && coordinator.LastTouch().y == 24.0f);
    coordinator.Frame(1600, 720, 1.0f / 60.0f, 1600, 720);
    coordinator.ProcessTouch(3, 0, 600.0f, 48.0f);
    CHECK(coordinator.LastTouch().x == 600.0f && coordinator.LastTouch().renderW == 1600);
    coordinator.Reset();
    coordinator.ProcessTouch(1, 0, 600.0f, 48.0f);
    CHECK(coordinator.LastTouch().x == 600.0f && coordinator.LastTouch().renderW == 0);
}

TEST(raw_menu_pill_reopens_after_close_without_imgui_and_corner_still_works) {
    RawMenuGesture detector;
    const auto send = [&](int action, int x, int y, double t, bool open = false) {
        return detector.Feed(action, 4, x, y, t, open, 2400, 1080);
    };
    auto down = send(2, 1200, 120, 0.0);
    CHECK(down.consume && !down.toggle);
    auto up = send(1, 1200, 120, 0.05);
    CHECK(up.consume && up.toggle);
    // Close was handled by ImGui: raw input has no dependency on its mouse state.
    CHECK(!send(2, 200, 400, 0.2, true).consume);
    CHECK(!send(1, 200, 400, 0.22, true).toggle);
    CHECK(send(2, 600, 0, 0.4).consume);
    CHECK(send(1, 600, 0, 0.45).toggle);
    CHECK(!send(2, 599, 80, 0.6).consume);
    CHECK(!send(1, 599, 80, 0.65).toggle);
    CHECK(send(2, 30, 30, 0.8).consume);
    CHECK(send(1, 30, 30, 0.85).toggle);
    CHECK(send(2, 1200, 10, 1.0).consume);
    CHECK(send(3, 1290, 10, 1.02).consume);
    CHECK(!send(1, 1200, 10, 1.05).toggle); // moved outside touch slop
}

TEST(raw_menu_triple_tap_and_both_swipe_directions_use_native_pixels) {
    RawMenuGesture detector;
    const auto send = [&](int action, int finger, int y, double t) {
        return detector.Feed(action, finger, 300 + finger, y, t, true, 0, 0).toggle;
    };
    CHECK(!send(2, 5, 300, 0.0));
    CHECK(!send(1, 5, 300, 0.05));
    CHECK(!send(2, 5, 300, 0.25));
    CHECK(!send(1, 5, 300, 0.30));
    CHECK(!send(2, 5, 300, 0.55));
    CHECK(send(1, 5, 300, 0.60)); // inclusive, no render dimensions available
    for (int i = 0; i != 3; ++i) {
        CHECK(!send(2, 5, 300, 1 + i * 0.31));
        CHECK(!send(1, 5, 300, 1.05 + i * 0.31));
    }
    for (const int dir : {1, -1}) {
        const double t = dir == 1 ? 3.0 : 4.0;
        CHECK(!send(2, 3, 400, t));
        CHECK(!send(2, 7, 410, t + 0.01));
        CHECK(!send(3, 3, 400 + dir * 60, t + 0.02));
        CHECK(!send(3, 7, 410 + dir * 59, t + 0.03));
        CHECK(send(3, 7, 410 + dir * 60, t + 0.04));
        CHECK(!send(1, 3, 400 + dir * 80, t + 0.05));
        CHECK(!send(1, 7, 410 + dir * 80, t + 0.06));
    }
}

TEST(open_menu_routes_raw_touch_to_real_imgui_button) {
    ImGuiContext *context = ImGui::CreateContext();
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1200, 540);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char *pixels = nullptr;
    int texW = 0, texH = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &texW, &texH);
    core::MenuTouchQueue queue;
    auto frame = [&]() {
        const bool trickle = io.ConfigInputTrickleEventQueue;
        if (const auto touch = queue.Pop())
            ui::FeedImGuiTouch(io, *touch, ScreenToRender(touch->x, touch->y, 2400, 1080, 1200, 540));
        ImGui::NewFrame();
        io.ConfigInputTrickleEventQueue = trickle;
        ImGui::SetNextWindowPos(ImVec2(100, 100));
        ImGui::SetNextWindowSize(ImVec2(500, 350));
        ImGui::Begin("Teleports");
        ImGui::SetCursorScreenPos(ImVec2(160, 160));
        const bool clicked = ImGui::Button("New York", ImVec2(160, 60));
        ImGui::End();
        ImGui::Render();
        return clicked;
    };
    frame();
    frame();
    CHECK(!queue.Capture(false, 2, 0, 400, 360));
    CHECK(queue.Capture(true, 2, 0, 400, 360));
    CHECK(queue.Capture(true, 2, 1, 20, 20)); // extra finger never becomes the ImGui mouse
    CHECK(queue.Capture(true, 3, 0, 420, 370));
    CHECK(queue.Capture(false, 1, 1, 20, 20));
    CHECK(queue.Capture(false, 1, 0, 420, 370)); // still captured if UI closed mid-gesture
    CHECK(!frame() && io.MouseDown[0]);
    CHECK(!frame() && io.MouseDown[0]);
    CHECK(frame() && !io.MouseDown[0]);
    CHECK(queue.Capture(true, 2, 0, 400, 360));
    CHECK(queue.Capture(true, 4, 0, 400, 360));
    CHECK(!frame() && io.MouseDown[0]);
    CHECK(!frame() && !io.MouseDown[0]);
    ImGui::DestroyContext(context);
}

TEST(road_selection_uses_table_whitelist_tags_then_largest_flat_collision) {
    auto candidate = [](int id, PropTag tag, Vec3f half, bool collidable = true) {
        ModelProto p;
        p.id = id;
        p.name = "ground_test_" + std::to_string(id);
        p.tag = tag;
        p.colHalfExtents = half;
        p.hasCollision = collidable;
        return p;
    };
    ModelCatalog cat;
    cat.AddModels({candidate(3010, PropTag::RoadFlat, {8, 12, 0.25f}),
                   candidate(3020, PropTag::Prop, {6, 10, 0.2f}),
                   candidate(3030, PropTag::Prop, {10, 10, 0.5f}),
                   candidate(3040, PropTag::Prop, {40, 40, 12}),
                   candidate(3050, PropTag::RoadFlat, {20, 20, 0.0f}, false)});
    auto pick = [](const ModelProto *p) { return p ? p->id : -1; };
    CHECK(pick(cat.PickRoad(3020, 3030)) == 3020);
    CHECK(pick(cat.PickRoad(3040, 3030)) == 3030);
    CHECK(pick(cat.PickRoad(3040, 3050)) == 3010);
    cat.Add(candidate(3010, PropTag::Prop, {8, 12, 0.25f}));
    CHECK(pick(cat.PickRoad()) == 3030);
    cat.Clear();
    CHECK(!cat.PickRoad());
}

TEST(road_rejects_postbox_dynamic_and_unverified_flat_props) {
    ModelProto slab;
    slab.id = 4010;
    slab.name = "CE_ground_test";
    slab.colHalfExtents = {5.0f, 3.0f, 0.3f};
    CHECK(IsFlatRoadCandidate(slab));

    ModelProto postbox = slab;
    postbox.id = 1429;
    postbox.name = "DYN_postbox";
    postbox.tag = PropTag::RoadFlat;
    postbox.isRoad = true;
    CHECK(!IsFlatRoadCandidate(postbox)); // even with a bogus road tag and large bounds
    postbox.name = "road_valid_but_id_too_low";
    CHECK(!IsFlatRoadCandidate(postbox));
    auto high = slab;
    high.id = 18001;
    CHECK(!IsFlatRoadCandidate(high));
    high.id = 18000;
    CHECK(IsFlatRoadCandidate(high));
    postbox.name = "DYN_roadbarrier";
    CHECK(!IsFlatRoadCandidate(postbox));

    ModelProto dynamicSlab = slab;
    dynamicSlab.id = 4011;
    dynamicSlab.name = "road_sfe_dynamic";
    dynamicSlab.isDynamic = true;
    CHECK(!IsFlatRoadCandidate(dynamicSlab));
    ModelProto small = slab;
    small.id = 4012;
    small.colHalfExtents = {2.5f, 2.5f, 0.1f}; // exactly 25 m², not greater
    CHECK(!IsFlatRoadCandidate(small));
    ModelProto prop = slab;
    prop.id = 4013;
    prop.name = "test_sheet";
    CHECK(!IsFlatRoadCandidate(prop));

    ModelCatalog cat;
    cat.AddModels({slab, postbox, dynamicSlab, small, prop});
    CHECK(cat.PickRoad(1429, 4011) && cat.PickRoad(1429, 4011)->id == slab.id);
}

TEST(spawn_validation_requires_mapped_model_rw_collision_and_object_rw) {
    struct FakeResource { int value = 42; } rw, col;
    struct FakeModel { FakeResource *m_pRwObject; FakeResource *m_pColModel; } model{&rw, &col};
    struct FakeObject { FakeResource *m_pRwObject; } object{&rw};
    CHECK(HasMappedModelResources(&model));
    CHECK(HasMappedObjectInstance(&object));
    CHECK(!HasMappedModelResources(static_cast<FakeModel *>(nullptr)));
    model.m_pColModel = nullptr;
    CHECK(!HasMappedModelResources(&model));
    model.m_pColModel = &col;
    model.m_pRwObject = nullptr;
    CHECK(!HasMappedModelResources(&model));
    model.m_pRwObject = reinterpret_cast<FakeResource *>(0x595);
    CHECK(!HasMappedModelResources(&model));
    object.m_pRwObject = nullptr;
    CHECK(!HasMappedObjectInstance(&object));
    object.m_pRwObject = reinterpret_cast<FakeResource *>(0x595);
    CHECK(!HasMappedObjectInstance(&object));
}

TEST(polygon_defers_unready_road_and_flushes_only_once) {
    class DelayedBridge : public test::MockBridge {
      public:
        bool ready = false;
        bool IsModelLoaded(int) override { return ready; }
    } bridge;
    bridge.roadModel = 1429; // a probe result absent from the verified catalogue must not be used
    TestPolygon polygon(bridge, MakeTestCatalog());
    CHECK(polygon.Enable({}, {}));
    CHECK(polygon.RoadModel() == 3001 && !polygon.HasVisuals());
    CHECK(bridge.created == 0 && bridge.flushes == 1);
    polygon.Update({}, {});
    CHECK(bridge.created == 0 && bridge.flushes == 1);
    bridge.ready = true;
    polygon.Update({}, {});
    CHECK(polygon.HasVisuals() && bridge.created > 0 && bridge.flushes == 1);
}

TEST(polygon_holds_player_when_other_tiles_exist_but_support_is_missing) {
    class PartialBridge : public test::MockBridge {
      public:
        bool supportReady = false;
        bool hit = false;
        bool HasRoadSupportAt(ObjectHandle, const Vec3f &surface) override {
            CHECK(surface.y == 20.0f && surface.z == 300.0f);
            return hit;
        }
        ObjectHandle CreateObject(const SpawnParams &params) override {
            if (!supportReady && std::abs(params.pos.y - 20.0f) < 0.01f)
                return {};
            return MockBridge::CreateObject(params);
        }
    } bridge;
    bridge.player = {10.0f, 20.0f, 40.0f};
    TestPolygon polygon(bridge, MakeTestCatalog());
    CHECK(polygon.Enable({1010, 2020}, {1000, 2000}));
    CHECK(polygon.TileCount() > 0 && polygon.WaitingForSupport());
    CHECK(bridge.player.x == 10.0f && bridge.player.y == 20.0f && bridge.player.z == 301.5f);
    CHECK(bridge.playerVelocity.x == 0 && bridge.playerVelocity.y == 0 && bridge.playerVelocity.z == 0);
    bridge.player = {40.0f, 22.0f, 290.0f};
    bridge.playerVelocity = {3, 4, -10};
    polygon.Update({1040, 2022}, {1000, 2000});
    CHECK(bridge.player.x == 10.0f && bridge.player.y == 20.0f && bridge.player.z == 301.5f);
    polygon.OnOriginShift({1200, 2000});
    CHECK(bridge.player.x == -190.0f && bridge.player.z == 301.5f);
    const auto tilesBefore = polygon.TileCount();
    bridge.supportReady = true;
    bridge.now = 2.1;
    polygon.Update({1010, 2020}, {1200, 2000});
    CHECK(polygon.WaitingForSupport() && polygon.TileCount() == tilesBefore + 1);
    bridge.hit = true;
    polygon.Update({1010, 2020}, {1200, 2000});
    CHECK(!polygon.WaitingForSupport());
}

TEST(polygon_force_requests_each_verified_candidate_once_and_reports_why_it_failed) {
    class FallbackBridge : public test::MockBridge {
      public:
        std::vector<int> roadRequests;
        bool RequestRoadModel(int id) override {
            roadRequests.push_back(id);
            return RequestModel(id, false);
        }
        bool IsRoadModelLoaded(int id) override { return id != 3001 && IsModelLoaded(id); }
        const char *RoadSpawnError() const override { return reason; }
        ObjectHandle CreateObject(const SpawnParams &params) override {
            CHECK(params.roadSlab && params.collision && !params.dynamic);
            CHECK(flushes == static_cast<int>(roadRequests.size()));
            if (params.modelId == 3095) {
                reason = "CObject::Create returned nullptr; Add not called";
                return {};
            }
            reason = nullptr;
            return MockBridge::CreateObject(params);
        }
        const char *reason = nullptr;
    } bridge;
    auto catalog = MakeTestCatalog();
    bridge.roadModel = 3001;
    for (const int id : {1225, 1271, 3095, 18450}) {
        auto proto = *catalog.Find(3001);
        proto.id = id;
        catalog.Add(proto);
        CHECK(IsFlatRoadCandidate(proto) == (id >= 3000));
    }
    TestPolygon polygon(bridge, catalog);
    CHECK(polygon.Enable({}, {}));
    CHECK(polygon.StatusLine().find("model not loaded; Add not called") != std::string::npos);
    CHECK(polygon.WaitingForSupport() && bridge.flushes == 1);
    bridge.now = 4.1;
    polygon.Update({}, {});
    CHECK(polygon.RoadModel() == 3095 && bridge.flushes == 2 && bridge.releases == 1);
    polygon.Update({}, {});
    CHECK(polygon.StatusLine().find("CObject::Create returned nullptr") != std::string::npos);
    CHECK(polygon.WaitingForSupport() && polygon.TileCount() == 0);
    bridge.now = 8.2;
    polygon.Update({}, {});
    polygon.Update({}, {});
    CHECK(polygon.RoadModel() == 18450 && bridge.flushes == 3 && bridge.releases == 2);
    CHECK(!polygon.WaitingForSupport() && polygon.TileCount() > 0);
    CHECK(polygon.StatusLine().find("Err: none") != std::string::npos);
}

TEST(interior_defers_unready_props_and_flushes_only_once) {
    class DelayedBridge : public test::MockBridge {
      public:
        bool ready = false;
        bool IsModelLoaded(int) override { return ready; }
    } bridge;
    InteriorGenerator generator(MakeTestCatalog());
    InteriorManager interior(bridge, generator);
    WorldDelta delta;
    CHECK(interior.Enter(27, {}, delta));
    CHECK(bridge.created == 0 && bridge.flushes == 1);
    interior.Update({100, 100, bridge.player.z}, delta);
    CHECK(bridge.created == 0 && bridge.flushes == 1);
    bridge.ready = true;
    interior.Update({100, 100, bridge.player.z}, delta);
    CHECK(interior.ObjectCount() > 0 && bridge.flushes == 1);
    interior.Exit(delta);
    CHECK(bridge.objects.empty());
}

TEST(shipped_model_whitelist_has_named_road_and_non_road_entries) {
    const auto root = Json::Parse(ReadFile(SrcPath("assets/model_whitelist.json")));
    CHECK(root.GetInt("version", 0) == 1);
    const auto &models = root["models"].GetArray();
    bool road = false, other = false;
    std::set<std::string> names;
    for (const auto &model : models) {
        const auto name = model.GetString("name", "");
        CHECK(!name.empty() && name.size() <= 20 && names.insert(name).second);
        const auto tag = PropTagFromName(model.GetString("tag", ""));
        CHECK(tag != PropTag::Unknown);
        road |= tag == PropTag::RoadFlat;
        other |= tag != PropTag::RoadFlat;
    }
    CHECK(road && other);
}

TEST(polygon_sets_height_and_recovers_after_origin_shift_or_fall) {
    test::MockBridge bridge;
    bridge.roadModel = -1;
    auto cat = MakeTestCatalog();
    TestPolygon::Config cfg;
    cfg.generationZ = 400.0f;
    TestPolygon polygon(bridge, cat, cfg);
    bridge.player = {10.0f, 20.0f, 40.0f};
    CHECK(polygon.Enable({1010.0, 2020.0}, {1000.0, 2000.0}));
    CHECK(polygon.RoadModel() == 3001);
    CHECK(bridge.player.x == 10.0f && bridge.player.y == 20.0f && bridge.player.z == 402.0f);
    CHECK(polygon.HasVisuals());
    for (const auto &kv : bridge.objects) {
        CHECK(kv.second.collision);
        CHECK_NEAR(kv.second.pos.z + 0.3f, 400.0f, 0.001);
    }
    for (const auto &kv : bridge.markers)
        CHECK_NEAR(kv.second.pos.z, 400.3f, 0.001);
    bridge.player = {-190.0f, 20.0f, 400.9f};
    polygon.OnOriginShift({1200.0, 2000.0});
    CHECK(bridge.player.x == -190.0f && bridge.player.z == 402.0f);
    bridge.player = {-180.0f, 20.0f, 395.0f};
    polygon.Update({1020.0, 2020.0}, {1200.0, 2000.0});
    CHECK(bridge.player.z == 395.0f);
    bridge.player.z = 394.9f;
    polygon.Update({1020.0, 2020.0}, {1200.0, 2000.0});
    CHECK(bridge.player.x == -190.0f && bridge.player.y == 20.0f && bridge.player.z == 402.0f);
}

TEST(polygon_reports_markers_only_if_road_request_fails) {
    class RejectedBridge : public test::MockBridge {
        bool RequestModel(int, bool) override { return false; }
    } bridge;
    TestPolygon polygon(bridge, MakeTestCatalog());
    CHECK(polygon.Enable({}, {}));
    CHECK(polygon.RoadModel() == 3001);
    CHECK(!polygon.HasVisuals() && polygon.MarkerCount() > 0);
    CHECK(bridge.player.z == 301.5f && polygon.WaitingForSupport());
    polygon.Disable();
    CHECK(bridge.releases == 0 && bridge.flushes == 0);
}

TEST(generation_z_offsets_chunk_objects_and_door_markers_without_changing_relief) {
    test::MockBridge low, high;
    const auto cat = MakeTestCatalog();
    ChunkGenerator lowGen(cat), highGen(cat, 450.0f);
    CHECK_NEAR(HeightField::GenerationHeightAt(100, 100) - HeightField::HeightAt(100, 100), 300.0f, 0.001);
    ChunkStreamer::Config cfg;
    cfg.radius = 0;
    cfg.maxSpawnsPerUpdate = 1000;
    ChunkStreamer a(low, lowGen, cfg), b(high, highGen, cfg);
    a.Update({100, 100}, {});
    b.Update({100, 100}, {});
    CHECK(!low.objects.empty() && low.objects.size() == high.objects.size());
    CHECK(!low.markers.empty() && low.markers.size() == high.markers.size());
    for (const auto &[handle, obj] : low.objects) {
        CHECK(obj.collision);
        CHECK_NEAR(high.objects.at(handle).pos.z - obj.pos.z, 150.0f, 0.001);
    }
    for (const auto &[id, marker] : low.markers)
        CHECK_NEAR(high.markers.at(id).pos.z - marker.pos.z, 150.0f, 0.001);
}

TEST(streamer_drops_rejected_or_timed_out_models_and_releases_only_accepted_requests) {
    class UnavailableBridge : public test::MockBridge {
      public:
        bool accept = false;
        bool RequestModel(int, bool) override { ++requests; return accept; }
    } bridge;
    ChunkGenerator gen(MakeTestCatalog());
    ChunkStreamer::Config cfg;
    cfg.radius = 0;
    ChunkStreamer streamer(bridge, gen, cfg);
    streamer.Update({100, 100}, {});
    CHECK(bridge.requests > 0 && streamer.PendingCount() == 0 && streamer.ObjectCount() == 0);
    streamer.Shutdown();
    CHECK(bridge.releases == 0);
    bridge.accept = true;
    streamer.Update({100, 100}, {});
    CHECK(streamer.PendingCount() > 0);
    bridge.now = 11.0;
    streamer.Update({100, 100}, {});
    CHECK(streamer.PendingCount() == 0 && streamer.ObjectCount() == 0);
    streamer.Shutdown();
    CHECK(bridge.releases > 0);
}

TEST(polygon_spawns_endless_road_with_200m_markers) {
    test::MockBridge bridge;
    ModelCatalog cat = MakeTestCatalog();
    ModelProto preferred = *cat.Find(3001);
    preferred.id = bridge.roadModel;
    preferred.name = "test_ground_tile";
    cat.Add(preferred);
    TestPolygon polygon(bridge, cat);
    CHECK(!polygon.Active());

    const Vec2d origin(0.0, 0.0);
    const Vec2d start(5000.0, 0.0);
    CHECK(polygon.Enable(start, origin));
    CHECK(polygon.Active());
    CHECK(polygon.HasVisuals());
    CHECK(polygon.RoadModel() == 5000); // probe wins over catalogue in the mock
    CHECK(polygon.TileCount() > 0);
    CHECK(polygon.MarkerCount() > 0);
    CHECK(bridge.markers.size() == polygon.MarkerCount());

    // Markers snap to the global 200 m grid.
    for (const auto &kv : bridge.markers) {
        const double y = static_cast<double>(kv.second.pos.y) + origin.y;
        CHECK_NEAR(std::fmod(std::fabs(y), 200.0), 0.0, 1e-3);
    }

    // Drive 3 km north: tiles stay bounded (recycled, not accumulated).
    for (int i = 0; i < 60; ++i) {
        const Vec2d pos(start.x, 50.0 * (i + 1));
        polygon.Update(pos, origin);
        CHECK(polygon.TileCount() <= 13);
    }
    CHECK(bridge.destroyed > 0);

    polygon.Disable();
    CHECK(!polygon.Active());
    CHECK(polygon.TileCount() == 0);
    CHECK(polygon.MarkerCount() == 0);
    CHECK(bridge.markers.empty());
}

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

TEST(config_roundtrip_and_clamps) {
    ModConfig cfg;
    cfg.gridRadius = 2;
    cfg.spawnCity = "Las Vegas";
    cfg.cloudSyncEnabled = false;
    cfg.generationZ = 433.5f;
    cfg.localLimit = 1000.0;

    const fs::path path = fs::temp_directory_path() / "usa1to1_config_test.json";
    std::string err;
    CHECK_MSG(cfg.SaveFile(path.string(), &err), err);
    ModConfig loaded;
    CHECK(loaded.LoadFile(path.string(), &err));
    CHECK(loaded.gridRadius == 2);
    CHECK(loaded.generationZ == 433.5f);
    CHECK(loaded.spawnCity == "Las Vegas");
    CHECK(!loaded.cloudSyncEnabled);
    CHECK(!loaded.manifestUrl.empty());
    CHECK(loaded.modelIdLimit == 20000); // stock 2.00 table size
    fs::remove(path);

    // A hand-edited file with insane values must be clamped, not trusted.
    Json root = Json::Parse(R"({"streaming":{"grid_radius":9},"floating_origin":{"local_limit":99999}})");
    ModConfig wild;
    CHECK(wild.FromJson(root));
    CHECK(wild.gridRadius == 3);
    CHECK_NEAR(wild.localLimit, 1000.0, 1e-9);

    // The model scan limit decides how far into CModelInfo we may look.
    Json small = Json::Parse(R"({"content":{"model_id_limit":10,"generation_z":-7}})");
    ModConfig clampedLow;
    CHECK(clampedLow.FromJson(small));
    CHECK(clampedLow.modelIdLimit == 400);
    CHECK(clampedLow.generationZ == 50.0f);
    Json huge = Json::Parse(R"({"content":{"model_id_limit":900000,"generation_z":7777}})");
    ModConfig clampedHigh;
    CHECK(clampedHigh.FromJson(huge));
    CHECK(clampedHigh.modelIdLimit == 26316);
    CHECK(clampedHigh.generationZ == 900.0f);
    Json ok = Json::Parse(R"({"content":{"model_id_limit":24000}})");
    ModConfig kept;
    CHECK(kept.FromJson(ok));
    CHECK(kept.modelIdLimit == 24000);
    CHECK(kept.generationZ == 300.0f);
}

TEST(config_missing_file_is_default) {
    ModConfig cfg;
    std::string err;
    CHECK(!cfg.LoadFile("/nonexistent/config.json", &err));
    CHECK(cfg.gridRadius == 2);
    CHECK(cfg.chunkSize == 200.0);
}

// ---------------------------------------------------------------------------
// Regions
// ---------------------------------------------------------------------------

TEST(regions_cover_us_cities) {
    struct Expect {
        const char *city;
        const char *code;
    };
    const Expect expected[] = {{"New York", "NY"}, {"Washington", "DC"}, {"Las Vegas", "NV"}, {"Los Angeles", "CA"}};
    for (const auto &e : expected) {
        const CityAnchor *city = FindCity(e.city);
        CHECK(city != nullptr);
        if (!city)
            continue;
        CHECK(city->x >= kUsaPlaneMinX && city->x <= kUsaPlaneMaxX);
        CHECK(city->y >= kUsaPlaneMinY && city->y <= kUsaPlaneMaxY);
        const RegionBox *box = FindRegion(city->x, city->y);
        CHECK(box != nullptr);
        if (box)
            CHECK_MSG(std::string(box->code) == e.code,
                      std::string(e.city) + " resolved to " + box->code);
    }

    // Off-map points still resolve to something sensible.
    CHECK(FindRegion(-500000.0, -500000.0) != nullptr);
    const RegionBox *ca = FindRegion(580347.6, 1063350.9);
    CHECK(ca && std::string(ca->code) == "CA");
    CHECK(!SectorLabel(*ca, 580347.6, 1063350.9).empty());
    CHECK(NearestCityLabel(580347.6, 1063350.9).find("Los Angeles") != std::string::npos);
}

TEST(latlon_projection_roundtrip) {
    const double lat = 40.7128;
    const double lon = -74.0060;
    const Vec2d p = LatLonToPlane(lat, lon);
    double lat2 = 0.0;
    double lon2 = 0.0;
    PlaneToLatLon(p.x, p.y, lat2, lon2);
    CHECK_NEAR(lat2, lat, 1e-6);
    CHECK_NEAR(lon2, lon, 1e-6);
}

// ---------------------------------------------------------------------------
// Catalogue
// ---------------------------------------------------------------------------

TEST(catalogue_parses_sample_and_filters) {
    ModelCatalog cat;
    std::string err;
    CHECK_MSG(cat.LoadFromFile(SrcPath("assets/objects_db.sample.json"), &err), err);
    CHECK(cat.Size() == 6);
    CHECK(cat.PickRoad() == nullptr); // illustrative ID 1236 is not an eligible road
    CHECK(cat.HasTag(PropTag::Building));
    CHECK(cat.HasTag(PropTag::Furniture));
    CHECK(!cat.HasTag(PropTag::Door));

    // Round-trip through our own JSON writer.
    ModelCatalog copy;
    CHECK(copy.LoadFromJson(cat.ToJson(), &err));
    CHECK(copy.Size() == cat.Size());

    const ModelCatalog empty;
    CHECK(empty.PickRoad() == nullptr);
    CHECK(empty.Size() == 0);
}

// ---------------------------------------------------------------------------
// Runner
// ---------------------------------------------------------------------------

} // namespace

// ---------------------------------------------------------------------------
// safe.hpp: the AML/JNI trust boundary
// ---------------------------------------------------------------------------
TEST(safe_pointers_reject_null_and_small_values) {
    CHECK(!core::IsSanePointer(nullptr));
    CHECK(!core::IsSanePointer(reinterpret_cast<const void *>(0x7)));
    CHECK(!core::IsSanePointer(reinterpret_cast<const void *>(0x1)));
    CHECK(!core::IsSanePointer(reinterpret_cast<const void *>(core::kMinSanePointer - 1)));
    int local = 0;
    CHECK(core::IsSanePointer(&local));
    CHECK(core::IsMappedRange(&local, sizeof(local)));
    CHECK(!core::IsMappedRange(nullptr, 4));
    CHECK(!core::IsMappedRange(reinterpret_cast<const void *>(0x7), 1));
    CHECK(!core::IsMappedRange(&local, 0));
    // A huge unmapped address must be rejected, not probed into a fault.
    CHECK(!core::IsMappedRange(reinterpret_cast<const void *>(0x7FFFFFF0u), 16));
}

TEST(safe_pointers_validate_strings_and_paths) {
    const char *good = "/sdcard/AMLMods/com.rockstargames.gtasa/USA1to1";
    CHECK(core::IsReadableCString(good));
    CHECK(core::IsSanePath(good));
    CHECK(!core::IsSanePath("relative/path"));
    CHECK(!core::IsSanePath(""));
    CHECK(!core::IsSanePath(reinterpret_cast<const char *>(0x7))); // the value that crashed on device
    CHECK(!core::IsReadableCString(reinterpret_cast<const char *>(0x7)));
    CHECK(core::SanePathOr(reinterpret_cast<const char *>(0x7), "/fallback") == "/fallback");
    CHECK(core::SanePathOr(good, "/fallback") == std::string(good));
    CHECK(core::SanePathOr(nullptr, nullptr).empty());
    CHECK(core::HexPointer(reinterpret_cast<const void *>(0x7)) == "0x7");
}

TEST(model_table_garbage_entries_are_rejected) {
    // Mirrors the device crash: ms_modelInfoPtrs[i] held the integer 10184
    // (0x27C8) instead of a pointer, and info->m_szModelName[0] faulted at
    // 0x27C8 + 8. Such entries must be rejected, not dereferenced.
    struct FakeInfo {
        void *vptr;
        int flags;
    };
    FakeInfo good{nullptr, 1};
    FakeInfo *table[4] = {nullptr, reinterpret_cast<FakeInfo *>(0x27C8), &good, nullptr};
    FakeInfo *out = nullptr;

    CHECK(!core::IsUsableTableEntry(table, 0, out, sizeof(FakeInfo))); // null slot
    CHECK(!core::IsUsableTableEntry(table, 1, out, sizeof(FakeInfo))); // 0x27C8
    CHECK(out == nullptr);
    CHECK(core::IsUsableTableEntry(table, 2, out, sizeof(FakeInfo)));
    CHECK(out == &good);
    CHECK(!core::IsUsableTableEntry(static_cast<FakeInfo **>(nullptr), 0, out, sizeof(FakeInfo)));
    CHECK(!core::IsUsableTableEntry(table, -1, out, sizeof(FakeInfo)));
    // A structure size that runs past the mapping must be rejected too.
    CHECK(!core::IsUsableTableEntry(table, 2, out, 1u << 20));
}

TEST(status_feed_keeps_recent_messages) {
    core::StatusFeed feed;
    std::string out;
    CHECK(!feed.Latest(out, 0.0));
    CHECK(feed.LastAge(0.0) > 1e8);

    feed.Push("Entering shop...", 10.0);
    CHECK(feed.Latest(out, 10.5));
    CHECK(out == "Entering shop...");
    CHECK(feed.Fresh(out, 12.0, 3.0));
    CHECK(!feed.Fresh(out, 13.5, 3.0));
    CHECK_NEAR(feed.LastAge(13.0), 3.0, 1e-9);

    for (int i = 0; i < 20; ++i)
        feed.Push("message " + std::to_string(i), 20.0 + i);
    CHECK(feed.Size() == core::StatusFeed::kCapacity);
    CHECK(feed.LastText() == "message 19");

    feed.Push("", 40.0); // empty messages are ignored
    CHECK(feed.LastText() == "message 19");

    feed.Clear();
    CHECK(feed.Size() == 0);
    CHECK(!feed.Latest(out, 41.0));
}

TEST(corner_tap_toggles_without_swipe) {
    core::CornerTapDetector tap;
    // Plain tap inside the top-left corner.
    CHECK(!tap.Feed(0, 0, 20, 20, 1.000));
    CHECK(tap.Feed(1, 0, 22, 21, 1.100));
    CHECK(tap.TriggerCount() == 1);

    // Same spot again within the cooldown: ignored.
    CHECK(!tap.Feed(0, 0, 20, 20, 1.150));
    CHECK(!tap.Feed(1, 0, 20, 20, 1.200));

    // Outside the corner zone.
    CHECK(!tap.Feed(0, 0, 300, 300, 2.0));
    CHECK(!tap.Feed(1, 0, 300, 300, 2.05));

    // Drag that leaves the zone is a camera gesture, not a tap.
    CHECK(!tap.Feed(0, 0, 20, 20, 3.0));
    CHECK(!tap.Feed(2, 0, 90, 60, 3.05));
    CHECK(!tap.Feed(2, 0, 200, 300, 3.10));
    CHECK(!tap.Feed(1, 0, 200, 300, 3.15));

    // Long press.
    CHECK(!tap.Feed(0, 0, 20, 20, 4.0));
    CHECK(!tap.Feed(1, 0, 20, 20, 4.5));

    // Second finger disqualifies the tap.
    CHECK(!tap.Feed(0, 0, 20, 20, 5.0));
    CHECK(!tap.Feed(5, 1, 400, 400, 5.01));
    CHECK(!tap.Feed(1, 0, 20, 20, 5.05));

    // A cancelled gesture never triggers.
    CHECK(!tap.Feed(0, 0, 20, 20, 6.0));
    CHECK(!tap.Feed(3, 0, 0, 0, 6.05));
    CHECK(tap.TriggerCount() == 1);

    // And a clean tap still works after all that.
    CHECK(!tap.Feed(0, 0, 30, 30, 7.0));
    CHECK(tap.Feed(1, 0, 30, 30, 7.05));
    CHECK(tap.TriggerCount() == 2);
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Log::SetSink([](LogLevel level, const std::string &msg) {
        if (level == LogLevel::Error || level == LogLevel::Warn)
            std::printf("    log: %s\n", msg.c_str());
        (void)level;
    });

    std::printf("USA 1:1 - unit tests (%zu cases)\n", Tests().size());
    for (const auto &test : Tests()) {
        g_currentTest = test.name;
        const int before = g_failures;
        test.fn();
        std::printf("%s %s\n", g_failures == before ? "  ok  " : " FAIL ", test.name);
    }
    std::printf("\n%zu checks, %d failures\n", (size_t)g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
