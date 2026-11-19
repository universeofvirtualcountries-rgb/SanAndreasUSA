#include "mission.hpp"

#include "log.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace usa1to1 {
namespace core {
namespace {

MissionStepType ParseStepType(const std::string &s) {
    if (s == "goto" || s == "go_to" || s == "reach")
        return MissionStepType::GoTo;
    if (s == "rob_door" || s == "rob")
        return MissionStepType::RobDoor;
    if (s == "wait")
        return MissionStepType::Wait;
    if (s == "talk")
        return MissionStepType::Talk;
    return MissionStepType::Unknown;
}

bool ParseStep(const Json &j, MissionStep &out, std::string *error) {
    try {
        out.type = ParseStepType(j.GetString("type", ""));
        out.text = j.GetString("text", "");
        if (const Json *target = j.Find("target")) {
            out.target.x = target->GetDouble("x", 0.0);
            out.target.y = target->GetDouble("y", 0.0);
        }
        out.radius = static_cast<float>(j.GetDouble("radius", 8.0));
        out.seconds = j.GetDouble("seconds", 0.0);
        out.doorSeed = static_cast<uint64_t>(j.GetInt("door_seed", 0));
        out.payout = j.GetInt("payout", 0);
        if (out.type == MissionStepType::Unknown) {
            if (error)
                *error = "unknown mission step type";
            return false;
        }
        return true;
    } catch (const std::exception &e) {
        if (error)
            *error = e.what();
        return false;
    }
}

MissionDef ParseMission(const Json &root, std::string *error) {
    MissionDef def;
    try {
        def.id = root.GetString("id", "");
        def.title = root.GetString("title", "");
        def.giver = root.GetString("giver", "");
        def.brief = root.GetString("brief", "");
        def.region = root.GetString("region", "");
        def.reward = root.GetInt("reward", 0);
        if (const Json *start = root.Find("start")) {
            def.start.x = start->GetDouble("x", 0.0);
            def.start.y = start->GetDouble("y", 0.0);
        }
        if (const Json *steps = root.Find("steps")) {
            if (steps->IsArray()) {
                for (const auto &s : steps->GetArray()) {
                    MissionStep step;
                    if (!ParseStep(s, step, error))
                        return {};
                    def.steps.push_back(step);
                }
            }
        }
    } catch (const std::exception &e) {
        if (error)
            *error = e.what();
        return {};
    }
    return def;
}

} // namespace

bool MissionManager::LoadFromJson(const Json &root, std::string *error) {
    MissionDef def = ParseMission(root, error);
    if (!def.Valid()) {
        if (error && error->empty())
            *error = "mission has no id or no steps";
        return false;
    }
    AddOrReplace(def);
    return true;
}

bool MissionManager::LoadFile(const std::string &path, std::string *error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error)
            *error = "cannot open " + path;
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    Json root;
    if (!Json::TryParse(ss.str(), root, error))
        return false;
    if (!LoadFromJson(root, error)) {
        if (error)
            *error = path + ": " + *error;
        return false;
    }
    return true;
}

int MissionManager::LoadDirectory(const std::string &dir, std::string *error) {
    namespace fs = std::filesystem;
    int loaded = 0;
    std::error_code ec;
    if (!fs::exists(dir, ec)) {
        if (error)
            *error = "mission dir missing: " + dir;
        return 0;
    }
    for (const auto &entry : fs::directory_iterator(dir, ec)) {
        if (ec)
            break;
        if (!entry.is_regular_file())
            continue;
        if (entry.path().extension() != ".json")
            continue;
        std::string err;
        if (LoadFile(entry.path().string(), &err))
            ++loaded;
        else
            USA_LOG_W("MissionManager: %s", err.c_str());
    }
    USA_LOG_I("MissionManager: %d missions loaded from %s", loaded, dir.c_str());
    return loaded;
}

void MissionManager::AddOrReplace(const MissionDef &def) {
    for (auto &m : m_missions) {
        if (m.id == def.id) {
            m = def;
            return;
        }
    }
    m_missions.push_back(def);
    std::sort(m_missions.begin(), m_missions.end(),
              [](const MissionDef &a, const MissionDef &b) { return a.id < b.id; });
}

const MissionDef *MissionManager::Find(const std::string &id) const {
    for (const auto &m : m_missions)
        if (m.id == id)
            return &m;
    return nullptr;
}

// ---------------------------------------------------------------------------
// MissionRunner
// ---------------------------------------------------------------------------

bool MissionRunner::Start(const MissionDef &def, WorldDelta &delta) {
    if (!def.Valid())
        return false;
    const MissionState *state = delta.FindMission(def.id);
    if (state && state->status == "completed")
        return false; // already done, forever (until the delta is deleted)
    m_def = def;
    m_step = state && state->status == "active" ? state->step : 0;
    if (m_step < 0 || m_step >= static_cast<int>(m_def.steps.size()))
        m_step = 0;
    m_active = true;
    m_robedFlag = false;
    m_stepStart = 0.0;
    m_lastResult = Result::None;
    delta.SetMissionStatus(def.id, "active", m_step);
    USA_LOG_I("MissionRunner: started '%s' at step %d", def.id.c_str(), m_step);
    return true;
}

void MissionRunner::Abort(WorldDelta &delta) {
    if (!m_active)
        return;
    delta.SetMissionStatus(m_def.id, "available", 0);
    m_active = false;
    m_lastResult = Result::Failed;
}

void MissionRunner::NotifyDoorRobbed(uint64_t doorSeed) {
    if (!m_active)
        return;
    const MissionStep *step = CurrentStep();
    if (!step || step->type != MissionStepType::RobDoor)
        return;
    // doorSeed == 0 means "any cash register in the world".
    if (step->doorSeed == 0 || step->doorSeed == doorSeed)
        m_robedFlag = true;
}

const MissionStep *MissionRunner::CurrentStep() const {
    if (!m_active || m_step < 0 || m_step >= static_cast<int>(m_def.steps.size()))
        return nullptr;
    return &m_def.steps[static_cast<size_t>(m_step)];
}

MissionRunner::Result MissionRunner::Update(const Vec2d &playerGlobal, WorldDelta &delta, double nowSeconds) {
    m_lastResult = Result::None;
    if (!m_active)
        return Result::None;
    if (m_stepStart == 0.0)
        m_stepStart = nowSeconds;

    const MissionStep *step = CurrentStep();
    if (!step) {
        m_active = false;
        return Result::None;
    }

    bool done = false;
    switch (step->type) {
    case MissionStepType::GoTo:
    case MissionStepType::Talk:
        done = playerGlobal.Distance(step->target) <= static_cast<double>(step->radius);
        break;
    case MissionStepType::RobDoor: {
        if (m_robedFlag) {
            done = true;
            break;
        }
        if (step->doorSeed == 0) {
            done = false; // must be reported by the door manager
            break;
        }
        const DoorState *door = delta.FindDoor(step->doorSeed);
        done = door && door->robbed;
        break;
    }
    case MissionStepType::Wait: {
        // Waiting only counts while the player is near the step target (or the
        // target is unset, in which case anywhere counts).
        const bool near = step->target.LengthSqr() == 0.0 ||
                          playerGlobal.Distance(step->target) <= static_cast<double>(step->radius);
        done = near && (nowSeconds - m_stepStart) >= step->seconds;
        break;
    }
    case MissionStepType::Unknown:
        break;
    }

    if (!done)
        return Result::None;

    if (step->payout != 0)
        delta.money += step->payout;

    ++m_step;
    m_stepStart = nowSeconds;
    m_robedFlag = false;

    if (m_step >= static_cast<int>(m_def.steps.size())) {
        m_active = false;
        delta.money += m_def.reward;
        delta.SetMissionStatus(m_def.id, "completed", static_cast<int>(m_def.steps.size()));
        m_lastResult = Result::MissionCompleted;
        USA_LOG_I("MissionRunner: '%s' completed, +$%lld", m_def.id.c_str(),
                  static_cast<long long>(m_def.reward));
        return m_lastResult;
    }

    delta.SetMissionStatus(m_def.id, "active", m_step);
    m_lastResult = Result::StepCompleted;
    USA_LOG_I("MissionRunner: '%s' -> step %d", m_def.id.c_str(), m_step);
    return m_lastResult;
}

} // namespace core
} // namespace usa1to1
