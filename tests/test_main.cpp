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
#include "core/mission.hpp"
#include "core/model_catalog.hpp"
#include "core/regions.hpp"
#include "core/test_polygon.hpp"
#include "core/touch_gesture.hpp"
#include "core/world_delta.hpp"
#include "net/cloud_sync.hpp"

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

TEST(polygon_spawns_endless_road_with_200m_markers) {
    test::MockBridge bridge;
    const ModelCatalog cat = MakeTestCatalog();
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
    cfg.localLimit = 1000.0;

    const fs::path path = fs::temp_directory_path() / "usa1to1_config_test.json";
    std::string err;
    CHECK_MSG(cfg.SaveFile(path.string(), &err), err);
    ModConfig loaded;
    CHECK(loaded.LoadFile(path.string(), &err));
    CHECK(loaded.gridRadius == 2);
    CHECK(loaded.spawnCity == "Las Vegas");
    CHECK(!loaded.cloudSyncEnabled);
    CHECK(!loaded.manifestUrl.empty());
    fs::remove(path);

    // A hand-edited file with insane values must be clamped, not trusted.
    Json root = Json::Parse(R"({"streaming":{"grid_radius":9},"floating_origin":{"local_limit":99999}})");
    ModConfig wild;
    CHECK(wild.FromJson(root));
    CHECK(wild.gridRadius == 3);
    CHECK_NEAR(wild.localLimit, 1000.0, 1e-9);
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
    const ModelProto *road = cat.PickRoad();
    CHECK(road != nullptr);
    if (road)
        CHECK(road->id == 1236);
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
