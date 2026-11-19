#include "model_catalog.hpp"

#include "log.hpp"

#include <algorithm>
#include <cctype>

namespace usa1to1 {
namespace core {
namespace {

std::string Lower(std::string s) {
    for (char &c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

Vec3f ReadVec3(const Json &v) {
    if (v.IsArray() && v.Size() >= 3)
        return {static_cast<float>(v[0].AsDouble()), static_cast<float>(v[1].AsDouble()),
                static_cast<float>(v[2].AsDouble())};
    if (v.IsObject()) // {"x":..,"y":..,"z":..}
        return {static_cast<float>(v.GetDouble("x", 0.0)), static_cast<float>(v.GetDouble("y", 0.0)),
                static_cast<float>(v.GetDouble("z", 0.0))};
    return {};
}

const char *kTagNames[] = {"unknown", "building", "tree",    "prop", "road_flat", "furniture",
                           "door",    "wall",     "floor",   "light"};

} // namespace

const char *PropTagName(PropTag tag) {
    const int i = static_cast<int>(tag);
    if (i < 0 || i >= static_cast<int>(sizeof(kTagNames) / sizeof(kTagNames[0])))
        return "unknown";
    return kTagNames[i];
}

PropTag PropTagFromName(const std::string &name) {
    const std::string n = Lower(name);
    if (n == "building" || n == "build")
        return PropTag::Building;
    if (n == "tree" || n == "vegetation" || n == "veg")
        return PropTag::Tree;
    if (n == "prop" || n == "object")
        return PropTag::Prop;
    if (n == "road_flat" || n == "road" || n == "roadflat")
        return PropTag::RoadFlat;
    if (n == "furniture" || n == "furn")
        return PropTag::Furniture;
    if (n == "door" || n == "entry")
        return PropTag::Door;
    if (n == "wall")
        return PropTag::Wall;
    if (n == "floor")
        return PropTag::Floor;
    if (n == "light" || n == "lamp" || n == "street_light")
        return PropTag::StreetLight;
    return PropTag::Unknown;
}

void ModelCatalog::Clear() {
    m_protos.clear();
    RebuildBuckets();
}

void ModelCatalog::Add(const ModelProto &proto) {
    if (proto.id < 0)
        return;
    for (auto &p : m_protos) {
        if (p.id == proto.id) {
            p = proto;
            RebuildBuckets();
            return;
        }
    }
    m_protos.push_back(proto);
    RebuildBuckets();
}

void ModelCatalog::RebuildBuckets() {
    constexpr int kBucketCount = static_cast<int>(PropTag::StreetLight) + 1;
    for (int i = 0; i < kBucketCount; ++i)
        m_buckets[i].clear();

    // Stable order: sort by id so PickByTag is reproducible across devices even
    // if the source JSON order ever changes.
    std::sort(m_protos.begin(), m_protos.end(), [](const ModelProto &a, const ModelProto &b) { return a.id < b.id; });
    for (const auto &p : m_protos) {
        const int idx = static_cast<int>(p.tag);
        if (idx >= 0 && idx < kBucketCount)
            m_buckets[idx].push_back(&p);
        if (p.isRoad && p.tag != PropTag::RoadFlat)
            m_buckets[static_cast<int>(PropTag::RoadFlat)].push_back(&p);
    }
    // De-duplicate the road bucket after the isRoad merge.
    auto &road = m_buckets[static_cast<int>(PropTag::RoadFlat)];
    std::sort(road.begin(), road.end(), [](const ModelProto *a, const ModelProto *b) { return a->id < b->id; });
    road.erase(std::unique(road.begin(), road.end()), road.end());
}

bool ModelCatalog::LoadFromJson(const Json &root, std::string *error) {
    try {
        const Json *models = root.Find("models");
        if (!models && root.IsArray())
            models = &root;
        if (!models || !models->IsArray()) {
            if (error)
                *error = "objects_db: missing 'models' array";
            return false;
        }
        Clear();
        for (const auto &m : models->GetArray()) {
            if (!m.IsObject())
                continue;
            ModelProto p;
            p.id = static_cast<int>(m.GetInt("id", -1));
            p.name = m.GetString("name", "");
            p.tag = PropTagFromName(m.GetString("tag", "unknown"));
            if (const Json *col = m.Find("col")) {
                if (const Json *half = col->Find("half"))
                    p.colHalfExtents = ReadVec3(*half);
                if (const Json *off = col->Find("offset"))
                    p.colOffset = ReadVec3(*off);
            }
            p.drawDistance = static_cast<float>(m.GetDouble("draw_distance", 200.0));
            p.isRoad = m.GetBool("road", false);
            p.hasCollision = m.GetBool("has_collision", true);
            p.isDynamic = m.GetBool("dynamic", false);
            if (p.tag == PropTag::Unknown && p.isRoad)
                p.tag = PropTag::RoadFlat;
            Add(p);
        }
        USA_LOG_I("ModelCatalog: %zu models, %zu road, %zu buildings", m_protos.size(),
                  ByTag(PropTag::RoadFlat).size(), ByTag(PropTag::Building).size());
        return true;
    } catch (const std::exception &e) {
        if (error)
            *error = e.what();
        return false;
    }
}

bool ModelCatalog::LoadFromFile(const std::string &path, std::string *error) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) {
        if (error)
            *error = "cannot open " + path;
        return false;
    }
    std::string data;
    char buf[8192];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
        data.append(buf, n);
    std::fclose(f);

    Json root;
    if (!Json::TryParse(data, root, error))
        return false;
    return LoadFromJson(root, error);
}

Json ModelCatalog::ToJson() const {
    Json root = Json::Object();
    root.Set("version", 1);
    root.Set("generated_by", "USA1to1/runtime");
    Json arr = Json::MakeArray();
    for (const auto &p : m_protos) {
        Json m = Json::Object();
        m.Set("id", p.id);
        m.Set("name", p.name);
        m.Set("tag", PropTagName(p.tag));
        Json col = Json::Object();
        Json half = Json::MakeArray();
        half.Push(static_cast<double>(p.colHalfExtents.x));
        half.Push(static_cast<double>(p.colHalfExtents.y));
        half.Push(static_cast<double>(p.colHalfExtents.z));
        col.Set("half", half);
        Json off = Json::MakeArray();
        off.Push(static_cast<double>(p.colOffset.x));
        off.Push(static_cast<double>(p.colOffset.y));
        off.Push(static_cast<double>(p.colOffset.z));
        col.Set("offset", off);
        m.Set("col", col);
        m.Set("draw_distance", static_cast<double>(p.drawDistance));
        m.Set("road", p.isRoad);
        m.Set("has_collision", p.hasCollision);
        m.Set("dynamic", p.isDynamic);
        arr.Push(m);
    }
    root.Set("models", arr);
    return root;
}

const ModelProto *ModelCatalog::Find(int id) const {
    for (const auto &p : m_protos)
        if (p.id == id)
            return &p;
    return nullptr;
}

const std::vector<const ModelProto *> &ModelCatalog::ByTag(PropTag tag) const {
    static const std::vector<const ModelProto *> kEmpty;
    const int idx = static_cast<int>(tag);
    if (idx < 0 || idx > static_cast<int>(PropTag::StreetLight))
        return kEmpty;
    return m_buckets[idx];
}

const ModelProto *ModelCatalog::PickByTag(PropTag tag, uint64_t rngValue) const {
    const auto &bucket = ByTag(tag);
    if (bucket.empty())
        return nullptr;
    return bucket[static_cast<size_t>(rngValue % bucket.size())];
}

const ModelProto *ModelCatalog::PickRoad() const {
    const auto &roads = ByTag(PropTag::RoadFlat);
    if (roads.empty())
        return nullptr;
    return roads.front();
}

ModelCatalog ModelCatalog::MakeFallback() {
    // Intentionally empty: the adapter fills a live catalogue by scanning
    // CModelInfo at runtime, and tools/parse_ide.py can enrich it later.
    // Inventing model ids here would spawn the wrong shit in someone's game.
    return ModelCatalog();
}

} // namespace core
} // namespace usa1to1
