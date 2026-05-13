#ifndef _PLAYERBOT_LLMAGENT_GOAL_SCHEMA_H
#define _PLAYERBOT_LLMAGENT_GOAL_SCHEMA_H

#include "Vendor/nlohmann_json.hpp"
#include <cstdint>
#include <string>
#include <variant>

enum class GoalKind {
    Idle, GoGrind, GoCamp, WanderNpc, WanderRandom,
    DoQuest, TravelFlight, Rest, OutdoorPvp
};

struct IdleParams {};
struct GoGrindParams      { double x{}; double y{}; double z{}; int32_t map_id{}; };
struct GoCampParams       { double x{}; double y{}; double z{}; int32_t map_id{}; };
struct WanderNpcParams    { uint64_t npc_guid{}; };
struct WanderRandomParams {};
struct DoQuestParams      { uint32_t quest_id{}; int32_t starting_objective_idx{}; };
struct TravelFlightParams { uint64_t from_flightmaster_guid{}; uint32_t destination_node_id{}; };
struct RestParams         {};
struct OutdoorPvpParams   { uint32_t capture_point_spawn_id{}; };

using GoalParams = std::variant<
    IdleParams, GoGrindParams, GoCampParams, WanderNpcParams, WanderRandomParams,
    DoQuestParams, TravelFlightParams, RestParams, OutdoorPvpParams
>;

struct ParsedGoal {
    GoalKind    goal;
    GoalParams  params;
    std::string reasoning;
    uint32_t    ttl_minutes;
};

struct ParseError {
    std::string message;
};

extern const char* const kGoalSchemaJson;

std::variant<ParsedGoal, ParseError> ParseAndValidate(const std::string& raw_json);

#endif  // _PLAYERBOT_LLMAGENT_GOAL_SCHEMA_H
