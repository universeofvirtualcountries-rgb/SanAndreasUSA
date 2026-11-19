#include "interior.hpp"

#include "hash.hpp"
#include "log.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace usa1to1 {
namespace core {
namespace {

struct Cell {
    int x, y;
    bool operator<(const Cell &o) const { return x != o.x ? x < o.x : y < o.y; }
    bool operator==(const Cell &o) const { return x == o.x && y == o.y; }
};

Vec3f RotateOffset(float dx, float dy, float heading) {
    const float c = std::cos(heading);
    const float s = std::sin(heading);
    return {dx * c - dy * s, dx * s + dy * c, 0.0f};
}

} // namespace

InteriorGenerator::InteriorGenerator(const ModelCatalog &catalog) : m_catalog(catalog) {}

int InteriorGenerator::AreaIdForSeed(uint64_t doorSeed) {
    return 1 + static_cast<int>(Mix64(doorSeed) % static_cast<uint64_t>(kMaxAreaId));
}

float InteriorGenerator::PocketZForArea(int areaId) {
    if (areaId < 0)
        areaId = 0;
    return kInteriorBaseZ + static_cast<float>(areaId) * kInteriorPocketHeight;
}

InteriorLayout InteriorGenerator::Generate(uint64_t doorSeed, int areaId, float baseZ) const {
    InteriorLayout out;
    out.doorSeed = doorSeed;
    out.interiorSeed = InteriorSeed(doorSeed);
    out.areaId = areaId;
    out.baseZ = baseZ;

    Rng rng(out.interiorSeed);

    // --- 1. connected room set via random growth on a cell grid -------------
    const int wantedRooms = rng.RangeInt(3, 7);
    std::vector<Cell> cells;
    std::set<Cell> used;
    cells.push_back({0, 0});
    used.insert({0, 0});

    const int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    int guard = 0;
    while (static_cast<int>(cells.size()) < wantedRooms && guard++ < 256) {
        const Cell base = cells[rng.Below(static_cast<uint32_t>(cells.size()))];
        const int d = static_cast<int>(rng.Below(4));
        const Cell next{base.x + dirs[d][0], base.y + dirs[d][1]};
        if (std::abs(next.x) > 2 || std::abs(next.y) > 2)
            continue;
        if (used.count(next))
            continue;
        used.insert(next);
        cells.push_back(next);
    }

    // --- 2. rooms ------------------------------------------------------------
    out.rooms.reserve(cells.size());
    for (const auto &c : cells) {
        InteriorRoom room;
        room.cellX = c.x;
        room.cellY = c.y;
        room.width = rng.RangeF(10.0f, kCellSize - 0.5f);
        room.depth = rng.RangeF(10.0f, kCellSize - 0.5f);
        room.centerX = static_cast<float>(c.x) * kCellSize;
        room.centerY = static_cast<float>(c.y) * kCellSize;
        out.rooms.push_back(room);
    }
    // The exit room: the one closest to the entrance (cell 0,0), i.e. the room
    // the player spawns in.
    out.rooms.front().isExitRoom = true;
    out.playerSpawn = {out.rooms.front().centerX, out.rooms.front().centerY, baseZ + 0.05f};
    out.exitMarker = {out.rooms.front().centerX, out.rooms.front().centerY - out.rooms.front().depth * 0.35f,
                      baseZ + 0.2f};

    // --- 3. floor / ceiling slabs ------------------------------------------
    const auto &floors = m_catalog.ByTag(PropTag::Floor);
    if (!floors.empty()) {
        for (const auto &room : out.rooms) {
            const int nx = 2;
            for (int ix = 0; ix < nx; ++ix) {
                for (int iy = 0; iy < nx; ++iy) {
                    const ModelProto *m = floors[rng.Below(static_cast<uint32_t>(floors.size()))];
                    InteriorProp p;
                    p.modelId = m->id;
                    p.localPos = {room.centerX - room.width * 0.25f + ix * (room.width * 0.5f),
                                  room.centerY - room.depth * 0.25f + iy * (room.depth * 0.5f), baseZ};
                    p.headingRad = 0.0f;
                    p.tag = PropTag::Floor;
                    out.props.push_back(p);
                }
            }
        }
    }

    // --- 4. walls with doorways between adjacent rooms ---------------------
    const auto &walls = m_catalog.ByTag(PropTag::Wall);
    if (!walls.empty()) {
        const ModelProto *wallModel = walls.front();
        const float seg = 3.0f;
        auto hasRoom = [&](int cx, int cy) { return used.count({cx, cy}) > 0; };
        for (const auto &room : out.rooms) {
            const float left = room.centerX - room.width * 0.5f;
            const float right = room.centerX + room.width * 0.5f;
            const float bottom = room.centerY - room.depth * 0.5f;
            const float top = room.centerY + room.depth * 0.5f;

            // side: 0 south, 1 north, 2 west, 3 east
            for (int side = 0; side < 4; ++side) {
                const bool connected = (side == 0 && hasRoom(room.cellX, room.cellY - 1)) ||
                                       (side == 1 && hasRoom(room.cellX, room.cellY + 1)) ||
                                       (side == 2 && hasRoom(room.cellX - 1, room.cellY)) ||
                                       (side == 3 && hasRoom(room.cellX + 1, room.cellY));
                const bool horizontal = (side == 0 || side == 1);
                const float span = horizontal ? (right - left) : (top - bottom);
                const int count = std::max(2, static_cast<int>(span / seg));
                for (int i = 0; i < count; ++i) {
                    const float t = (i + 0.5f) / static_cast<float>(count);
                    const bool doorway = connected && i == count / 2;
                    if (doorway)
                        continue;
                    InteriorProp p;
                    p.modelId = wallModel->id;
                    p.headingRad = horizontal ? 0.0f : 1.5707963f;
                    switch (side) {
                    case 0:
                        p.localPos = {left + span * t, bottom, baseZ};
                        break;
                    case 1:
                        p.localPos = {left + span * t, top, baseZ};
                        break;
                    case 2:
                        p.localPos = {left, bottom + span * t, baseZ};
                        break;
                    default:
                        p.localPos = {right, bottom + span * t, baseZ};
                        break;
                    }
                    p.tag = PropTag::Wall;
                    out.props.push_back(p);
                }
            }
        }
    } else {
        USA_LOG_W("InteriorGenerator: no wall models in catalogue, rooms are open plan");
    }

    // --- 5. furniture -------------------------------------------------------
    const auto &furniture = m_catalog.ByTag(PropTag::Furniture);
    for (const auto &room : out.rooms) {
        const int count = rng.RangeInt(2, 4);
        for (int i = 0; i < count; ++i) {
            if (furniture.empty())
                break;
            const ModelProto *m = furniture[rng.Below(static_cast<uint32_t>(furniture.size()))];
            InteriorProp p;
            p.modelId = m->id;
            p.tag = PropTag::Furniture;
            const Vec3f off = RotateOffset(rng.RangeF(-room.width * 0.35f, room.width * 0.35f),
                                           rng.RangeF(-room.depth * 0.35f, room.depth * 0.35f),
                                           rng.RangeF(0.0f, 6.28f));
            p.localPos = {room.centerX + off.x, room.centerY + off.y, baseZ};
            p.headingRad = rng.RangeF(0.0f, 6.2831853f);
            out.props.push_back(p);
        }
    }

    // --- 6. cash register in the farthest room ------------------------------
    const InteriorRoom *registerRoom = nullptr;
    float bestDist = -1.0f;
    for (const auto &room : out.rooms) {
        const float d = std::hypot(room.centerX, room.centerY);
        if (d > bestDist) {
            bestDist = d;
            registerRoom = &room;
        }
    }
    if (registerRoom) {
        const auto &props = m_catalog.ByTag(PropTag::Prop);
        const auto &furn = m_catalog.ByTag(PropTag::Furniture);
        out.registerPos = {registerRoom->centerX, registerRoom->centerY - registerRoom->depth * 0.3f,
                           baseZ + 0.4f};
        if (!props.empty() || !furn.empty()) {
            const auto &src = !props.empty() ? props : furn;
            const ModelProto *m = src[rng.Below(static_cast<uint32_t>(src.size()))];
            InteriorProp p;
            p.modelId = m->id;
            p.localPos = out.registerPos;
            p.headingRad = 0.0f;
            p.tag = PropTag::Prop;
            out.props.push_back(p);
            out.hasRegister = true;
        }
    }

    std::set<int> models;
    for (const auto &p : out.props)
        if (p.modelId >= 0)
            models.insert(p.modelId);
    out.requiredModels.assign(models.begin(), models.end());

    return out;
}

bool InteriorGenerator::IsConnected(const InteriorLayout &layout) {
    if (layout.rooms.empty())
        return false;
    std::set<Cell> all;
    for (const auto &r : layout.rooms)
        all.insert({r.cellX, r.cellY});

    std::set<Cell> visited;
    std::vector<Cell> stack;
    stack.push_back({layout.rooms.front().cellX, layout.rooms.front().cellY});
    visited.insert(stack.front());
    const int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    while (!stack.empty()) {
        const Cell cur = stack.back();
        stack.pop_back();
        for (auto &d : dirs) {
            const Cell n{cur.x + d[0], cur.y + d[1]};
            if (!all.count(n) || visited.count(n))
                continue;
            visited.insert(n);
            stack.push_back(n);
        }
    }
    return visited.size() == all.size();
}

// ---------------------------------------------------------------------------
// InteriorManager
// ---------------------------------------------------------------------------

InteriorManager::InteriorManager(IEngineBridge &bridge, const InteriorGenerator &generator)
    : m_bridge(bridge), m_generator(generator) {}

int64_t InteriorManager::RewardForDoor(uint64_t doorSeed) { return 200 + static_cast<int64_t>(Mix64(doorSeed) % 800); }

bool InteriorManager::Enter(uint64_t doorSeed, const Vec3f &doorLocalPos, WorldDelta &delta) {
    if (m_inside)
        return false;

    const int areaId = InteriorGenerator::AreaIdForSeed(doorSeed);
    const float baseZ = InteriorGenerator::PocketZForArea(areaId);
    m_current = m_generator.Generate(doorSeed, areaId, baseZ);
    if (m_current.Empty())
        return false;

    for (int modelId : m_current.requiredModels) {
        if (modelId < 0)
            continue;
        if (m_bridge.RequestModel(modelId, false))
            m_models.push_back(modelId);
    }

    // One blocking flush during this transition; anything still unready waits
    // for later ticks, never a synchronous retry loop.
    if (!m_models.empty())
        m_bridge.FlushModelStreaming();
    for (size_t i = 0; i < m_current.props.size(); ++i) {
        const int modelId = m_current.props[i].modelId;
        if (std::find(m_models.begin(), m_models.end(), modelId) != m_models.end())
            m_pendingProps.push_back(i);
    }
    m_spawnDeadline = m_bridge.NowSeconds() + 10.0;
    SpawnPendingProps();

    m_returnPos = doorLocalPos;
    m_exitMarkerId = MarkerIdFromDoorSeed(doorSeed) ^ 0x5A5A5A5AULL;
    m_bridge.PlaceMarker(m_exitMarkerId, m_current.exitMarker, 1.0f, true);

    m_bridge.SetPlayerInterior(areaId);
    m_bridge.SetPlayerPosition(m_current.playerSpawn);
    m_inside = true;

    DoorState &st = delta.Door(doorSeed);
    ++st.visits;

    USA_LOG_I("Interior: entered door %llu -> area %d, %zu rooms, %zu props",
              static_cast<unsigned long long>(doorSeed), areaId, m_current.RoomCount(), m_objects.size());
    return true;
}

void InteriorManager::SpawnPendingProps() {
    if (!m_pendingProps.empty() && m_bridge.NowSeconds() >= m_spawnDeadline) {
        USA_LOG_W("Interior: gave up waiting for %zu pending props", m_pendingProps.size());
        m_pendingProps.clear();
    }
    for (auto it = m_pendingProps.begin(); it != m_pendingProps.end();) {
        const auto &prop = m_current.props[*it];
        if (!m_bridge.IsModelLoaded(prop.modelId)) {
            ++it;
            continue;
        }
        const SpawnParams params{prop.modelId, prop.localPos, prop.headingRad, false, true, true, m_current.doorSeed};
        const ObjectHandle handle = m_bridge.CreateObject(params);
        if (handle) {
            m_objects.push_back(handle);
        } else if (!m_bridge.IsModelLoaded(prop.modelId)) {
            ++it;
            continue;
        }
        it = m_pendingProps.erase(it);
    }
}

void InteriorManager::Exit(WorldDelta &delta) {
    (void)delta;
    if (!m_inside)
        return;
    // Put the player back in front of the door they came from.
    m_bridge.SetPlayerPosition(m_returnPos);
    m_bridge.SetPlayerInterior(0);
    Teardown();
}

void InteriorManager::Teardown() {
    for (const auto &h : m_objects)
        m_bridge.DestroyObject(h);
    m_objects.clear();
    for (int modelId : m_models)
        m_bridge.ReleaseModel(modelId);
    m_models.clear();
    m_pendingProps.clear();
    if (m_exitMarkerId) {
        m_bridge.RemoveMarker(m_exitMarkerId);
        m_exitMarkerId = 0;
    }
    m_inside = false;
    m_current = InteriorLayout{};
}

InteriorManager::Event InteriorManager::Update(const Vec3f &localPlayer, WorldDelta &delta) {
    if (!m_inside)
        return Event::None;

    // 1) Exit marker.
    const Vec3f &e = m_current.exitMarker;
    const float dx = localPlayer.x - e.x;
    const float dy = localPlayer.y - e.y;
    const float dz = localPlayer.z - e.z;
    if (std::fabs(dz) < 3.0f && (dx * dx + dy * dy) < 1.6f * 1.6f) {
        Exit(delta);
        return Event::Exited;
    }

    SpawnPendingProps();

    // 2) Cash register: walk into it and the till is yours - once per door,
    //    forever, because the delta save remembers it.
    if (m_current.hasRegister) {
        const Vec3f &r = m_current.registerPos;
        const float rx = localPlayer.x - r.x;
        const float ry = localPlayer.y - r.y;
        if ((rx * rx + ry * ry) < 1.3f * 1.3f) {
            DoorState &st = delta.Door(m_current.doorSeed);
            if (!st.robbed) {
                const int64_t reward = RewardForDoor(m_current.doorSeed);
                delta.RobDoor(m_current.doorSeed, reward);
                m_lastReward = reward;
                USA_LOG_I("Interior: cash register robbed, +$%lld", static_cast<long long>(reward));
                return Event::Robbed;
            }
        }
    }
    return Event::None;
}

} // namespace core
} // namespace usa1to1
