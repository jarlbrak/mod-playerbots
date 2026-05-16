#include "doctest.h"
#include "Validator/GoalValidator.h"
#include "Validator/ValidationContext.h"
#include "Schemas/Goal.h"

namespace {
BotValidationContext make_ctx() {
    BotValidationContext c;
    c.bot_level = 37;
    c.map_id    = 0;
    c.map_min_x = -10000.0; c.map_max_x = 10000.0;
    c.map_min_y = -10000.0; c.map_max_y = 10000.0;
    c.quest_log.push_back({502, /*status=*/0, /*objective_count=*/3});
    c.quest_log.push_back({488, /*status=*/2, /*objective_count=*/1});  // turned in
    c.nearby_creature_guids = {1001, 1002, 1003};
    c.known_flight_node_ids = {77, 88};
    c.valid_capture_point_spawn_ids = {200};
    return c;
}

ParsedGoal goal_idle() { return ParsedGoal{GoalKind::Idle, IdleParams{}, "x", 5}; }
ParsedGoal goal_rest() { return ParsedGoal{GoalKind::Rest, RestParams{}, "x", 5}; }
ParsedGoal goal_wander_random() { return ParsedGoal{GoalKind::WanderRandom, WanderRandomParams{}, "x", 5}; }
}  // namespace

TEST_CASE("ValidateGoalDecision Idle is always accepted") {
    auto r = ValidateGoalDecision(goal_idle(), make_ctx());
    CHECK(r.accepted == true);
}

TEST_CASE("ValidateGoalDecision Rest is always accepted") {
    auto r = ValidateGoalDecision(goal_rest(), make_ctx());
    CHECK(r.accepted == true);
}

TEST_CASE("ValidateGoalDecision WanderRandom is always accepted") {
    auto r = ValidateGoalDecision(goal_wander_random(), make_ctx());
    CHECK(r.accepted == true);
}

TEST_CASE("ValidateGoalDecision DoQuest accepts existing incomplete quest") {
    ParsedGoal g{GoalKind::DoQuest, DoQuestParams{502, 0}, "x", 10};
    auto r = ValidateGoalDecision(g, make_ctx());
    CHECK(r.accepted == true);
}

TEST_CASE("ValidateGoalDecision DoQuest rejects quest not in log") {
    ParsedGoal g{GoalKind::DoQuest, DoQuestParams{999, 0}, "x", 10};
    auto r = ValidateGoalDecision(g, make_ctx());
    CHECK(r.accepted == false);
    CHECK(r.reject_reason == "rejected_quest_not_in_log");
}

TEST_CASE("ValidateGoalDecision DoQuest rejects already-completed quest") {
    ParsedGoal g{GoalKind::DoQuest, DoQuestParams{488, 0}, "x", 10};
    auto r = ValidateGoalDecision(g, make_ctx());
    CHECK(r.accepted == false);
    CHECK(r.reject_reason == "rejected_quest_already_complete");
}

TEST_CASE("ValidateGoalDecision DoQuest rejects objective_idx out of range") {
    ParsedGoal g{GoalKind::DoQuest, DoQuestParams{502, 99}, "x", 10};
    auto r = ValidateGoalDecision(g, make_ctx());
    CHECK(r.accepted == false);
    CHECK(r.reject_reason == "rejected_objective_idx_out_of_range");
}

TEST_CASE("ValidateGoalDecision GoGrind accepts in-bounds same-map position") {
    ParsedGoal g{GoalKind::GoGrind, GoGrindParams{100.0, 200.0, 0.0, 0}, "x", 5};
    auto r = ValidateGoalDecision(g, make_ctx());
    CHECK(r.accepted == true);
}

TEST_CASE("ValidateGoalDecision GoGrind rejects wrong map") {
    ParsedGoal g{GoalKind::GoGrind, GoGrindParams{100.0, 200.0, 0.0, 530}, "x", 5};
    auto r = ValidateGoalDecision(g, make_ctx());
    CHECK(r.accepted == false);
    CHECK(r.reject_reason == "rejected_map_mismatch");
}

TEST_CASE("ValidateGoalDecision GoGrind rejects out-of-bounds position") {
    ParsedGoal g{GoalKind::GoGrind, GoGrindParams{99999.0, 0.0, 0.0, 0}, "x", 5};
    auto r = ValidateGoalDecision(g, make_ctx());
    CHECK(r.accepted == false);
    CHECK(r.reject_reason == "rejected_position_out_of_bounds");
}

TEST_CASE("ValidateGoalDecision GoCamp follows the same rules as GoGrind") {
    ParsedGoal good{GoalKind::GoCamp, GoCampParams{100.0, 200.0, 0.0, 0}, "x", 5};
    ParsedGoal bad {GoalKind::GoCamp, GoCampParams{100.0, 200.0, 0.0, 530}, "x", 5};
    CHECK(ValidateGoalDecision(good, make_ctx()).accepted == true);
    CHECK(ValidateGoalDecision(bad,  make_ctx()).accepted == false);
}

TEST_CASE("ValidateGoalDecision WanderNpc accepts nearby npc") {
    ParsedGoal g{GoalKind::WanderNpc, WanderNpcParams{1002}, "x", 5};
    auto r = ValidateGoalDecision(g, make_ctx());
    CHECK(r.accepted == true);
}

TEST_CASE("ValidateGoalDecision WanderNpc rejects far npc") {
    ParsedGoal g{GoalKind::WanderNpc, WanderNpcParams{9999}, "x", 5};
    auto r = ValidateGoalDecision(g, make_ctx());
    CHECK(r.accepted == false);
    CHECK(r.reject_reason == "rejected_npc_not_nearby");
}

TEST_CASE("ValidateGoalDecision WanderNpc is permissive when nearby list empty") {
    ParsedGoal g{GoalKind::WanderNpc, WanderNpcParams{9999}, "x", 5};
    BotValidationContext c = make_ctx();
    c.nearby_creature_guids.clear();
    auto r = ValidateGoalDecision(g, c);
    CHECK(r.accepted == true);
}

TEST_CASE("ValidateGoalDecision TravelFlight accepts known destination") {
    ParsedGoal g{GoalKind::TravelFlight, TravelFlightParams{0, 77}, "x", 30};
    auto r = ValidateGoalDecision(g, make_ctx());
    CHECK(r.accepted == true);
}

TEST_CASE("ValidateGoalDecision TravelFlight rejects unknown destination") {
    ParsedGoal g{GoalKind::TravelFlight, TravelFlightParams{0, 999}, "x", 30};
    auto r = ValidateGoalDecision(g, make_ctx());
    CHECK(r.accepted == false);
    CHECK(r.reject_reason == "rejected_unknown_flight_node");
}

TEST_CASE("ValidateGoalDecision OutdoorPvp accepts known capture point") {
    ParsedGoal g{GoalKind::OutdoorPvp, OutdoorPvpParams{200}, "x", 30};
    auto r = ValidateGoalDecision(g, make_ctx());
    CHECK(r.accepted == true);
}

TEST_CASE("ValidateGoalDecision OutdoorPvp rejects unknown capture point") {
    ParsedGoal g{GoalKind::OutdoorPvp, OutdoorPvpParams{999}, "x", 30};
    auto r = ValidateGoalDecision(g, make_ctx());
    CHECK(r.accepted == false);
    CHECK(r.reject_reason == "rejected_unknown_capture_point");
}
