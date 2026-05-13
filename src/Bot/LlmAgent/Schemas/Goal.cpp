#include "Schemas/Goal.h"

namespace {

constexpr uint32_t kMinTtlMinutes = 1;
constexpr uint32_t kMaxTtlMinutes = 1440;  // 24 hours

GoalKind kind_from_string(const std::string& s, bool& ok) {
    ok = true;
    if (s == "idle")          return GoalKind::Idle;
    if (s == "go_grind")      return GoalKind::GoGrind;
    if (s == "go_camp")       return GoalKind::GoCamp;
    if (s == "wander_npc")    return GoalKind::WanderNpc;
    if (s == "wander_random") return GoalKind::WanderRandom;
    if (s == "do_quest")      return GoalKind::DoQuest;
    if (s == "travel_flight") return GoalKind::TravelFlight;
    if (s == "rest")          return GoalKind::Rest;
    if (s == "outdoor_pvp")   return GoalKind::OutdoorPvp;
    ok = false;
    return GoalKind::Idle;
}

template <typename T>
bool try_get(const nlohmann::json& j, const char* key, T& out) {
    if (!j.contains(key)) return false;
    try { out = j.at(key).get<T>(); } catch (...) { return false; }
    return true;
}

bool parse_params(GoalKind k, const nlohmann::json& p, GoalParams& out, std::string& err) {
    switch (k) {
        case GoalKind::Idle:         out = IdleParams{};         return true;
        case GoalKind::WanderRandom: out = WanderRandomParams{}; return true;
        case GoalKind::Rest:         out = RestParams{};         return true;
        case GoalKind::GoGrind: {
            GoGrindParams gp;
            if (!try_get(p, "x", gp.x) || !try_get(p, "y", gp.y) ||
                !try_get(p, "z", gp.z) || !try_get(p, "map_id", gp.map_id)) {
                err = "go_grind params require x, y, z, map_id"; return false;
            }
            out = gp; return true;
        }
        case GoalKind::GoCamp: {
            GoCampParams gp;
            if (!try_get(p, "x", gp.x) || !try_get(p, "y", gp.y) ||
                !try_get(p, "z", gp.z) || !try_get(p, "map_id", gp.map_id)) {
                err = "go_camp params require x, y, z, map_id"; return false;
            }
            out = gp; return true;
        }
        case GoalKind::WanderNpc: {
            WanderNpcParams gp;
            if (!try_get(p, "npc_guid", gp.npc_guid)) {
                err = "wander_npc params require npc_guid"; return false;
            }
            out = gp; return true;
        }
        case GoalKind::DoQuest: {
            DoQuestParams gp;
            if (!try_get(p, "quest_id", gp.quest_id)) {
                err = "do_quest params require quest_id"; return false;
            }
            try_get(p, "starting_objective_idx", gp.starting_objective_idx);
            out = gp; return true;
        }
        case GoalKind::TravelFlight: {
            TravelFlightParams gp;
            if (!try_get(p, "from_flightmaster_guid", gp.from_flightmaster_guid) ||
                !try_get(p, "destination_node_id", gp.destination_node_id)) {
                err = "travel_flight params require from_flightmaster_guid and destination_node_id";
                return false;
            }
            out = gp; return true;
        }
        case GoalKind::OutdoorPvp: {
            OutdoorPvpParams gp;
            if (!try_get(p, "capture_point_spawn_id", gp.capture_point_spawn_id)) {
                err = "outdoor_pvp params require capture_point_spawn_id"; return false;
            }
            out = gp; return true;
        }
    }
    err = "unhandled goal kind"; return false;
}

}  // namespace

const char* const kGoalSchemaJson = R"({
  "type": "object",
  "required": ["goal", "params", "reasoning", "ttl_minutes"],
  "additionalProperties": false,
  "properties": {
    "goal": {
      "type": "string",
      "enum": ["idle","go_grind","go_camp","wander_npc","wander_random","do_quest","travel_flight","rest","outdoor_pvp"]
    },
    "params": {"type": "object"},
    "reasoning": {"type": "string", "maxLength": 500},
    "ttl_minutes": {"type": "integer", "minimum": 1, "maximum": 1440}
  }
})";

std::variant<ParsedGoal, ParseError> ParseAndValidate(const std::string& raw_json) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(raw_json);
    } catch (const std::exception& e) {
        return ParseError{std::string{"json parse error: "} + e.what()};
    }
    if (!j.is_object()) return ParseError{"top-level value is not an object"};

    std::string goal_str;
    if (!try_get(j, "goal", goal_str)) return ParseError{"missing required field: goal"};

    bool kind_ok = false;
    GoalKind kind = kind_from_string(goal_str, kind_ok);
    if (!kind_ok) return ParseError{"unknown goal enum: " + goal_str};

    if (!j.contains("params") || !j.at("params").is_object())
        return ParseError{"missing or non-object params"};
    if (!j.contains("reasoning")) return ParseError{"missing required field: reasoning"};
    if (!j.contains("ttl_minutes")) return ParseError{"missing required field: ttl_minutes"};

    uint32_t ttl{};
    if (!try_get(j, "ttl_minutes", ttl))
        return ParseError{"ttl_minutes is not an integer"};
    if (ttl < kMinTtlMinutes || ttl > kMaxTtlMinutes)
        return ParseError{"ttl_minutes out of range [1, 1440]"};

    std::string reasoning;
    try_get(j, "reasoning", reasoning);
    if (reasoning.size() > 500) return ParseError{"reasoning too long"};

    GoalParams params;
    std::string err;
    if (!parse_params(kind, j.at("params"), params, err))
        return ParseError{err};

    return ParsedGoal{kind, std::move(params), std::move(reasoning), ttl};
}
