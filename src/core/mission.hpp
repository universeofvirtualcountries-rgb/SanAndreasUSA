// JSON driven mission engine. Missions are data: no C++ recompilation is needed
// to add a new job, and CloudSync can drop fresh ones into the missions folder.
#pragma once

#include "json.hpp"
#include "types.hpp"
#include "world_delta.hpp"

#include <string>
#include <vector>

namespace usa1to1 {
namespace core {

enum class MissionStepType {
    GoTo,     // reach a global point within radius
    RobDoor,  // the door's cash register must be empty
    Wait,     // stand still / survive for N seconds
    Talk,     // reach a point and "talk" (same trigger as GoTo, different UI text)
    Unknown,
};

struct MissionStep {
    MissionStepType type = MissionStepType::Unknown;
    std::string text;
    Vec2d target{0.0, 0.0};
    float radius = 8.0f;
    double seconds = 0.0;
    uint64_t doorSeed = 0;
    int64_t payout = 0;
};

struct MissionDef {
    std::string id;
    std::string title;
    std::string giver;
    std::string brief;
    std::string region;
    Vec2d start{0.0, 0.0};
    int64_t reward = 0;
    std::vector<MissionStep> steps;

    bool Valid() const { return !id.empty() && !steps.empty(); }
};

class MissionManager {
  public:
    bool LoadFromJson(const Json &root, std::string *error = nullptr);
    bool LoadFile(const std::string &path, std::string *error = nullptr);
    // Loads every *.json file inside `dir`. Existing missions with the same id
    // are replaced (that is how CloudSync hot-swaps updated jobs).
    int LoadDirectory(const std::string &dir, std::string *error = nullptr);

    void AddOrReplace(const MissionDef &def);
    void Clear() { m_missions.clear(); }
    size_t Size() const { return m_missions.size(); }
    const std::vector<MissionDef> &All() const { return m_missions; }
    const MissionDef *Find(const std::string &id) const;

  private:
    std::vector<MissionDef> m_missions;
};

// Drives the active mission against player position + world delta.
class MissionRunner {
  public:
    enum class Result { None, StepCompleted, MissionCompleted, Failed };

    bool Start(const MissionDef &def, WorldDelta &delta);
    void Abort(WorldDelta &delta);
    Result Update(const Vec2d &playerGlobal, WorldDelta &delta, double nowSeconds);
    // Called by the door manager so RobDoor steps can complete the moment the
    // register is emptied, even if the player never returns to the door.
    void NotifyDoorRobbed(uint64_t doorSeed);

    bool IsActive() const { return m_active; }
    const MissionDef *Active() const { return m_active ? &m_def : nullptr; }
    int StepIndex() const { return m_step; }
    const MissionStep *CurrentStep() const;
    Result LastResult() const { return m_lastResult; }

  private:
    bool m_active = false;
    MissionDef m_def;
    int m_step = 0;
    double m_stepStart = 0.0;
    bool m_robedFlag = false;
    Result m_lastResult = Result::None;
};

} // namespace core
} // namespace usa1to1
