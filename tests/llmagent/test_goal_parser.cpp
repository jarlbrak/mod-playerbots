#include "doctest.h"
#include "Schemas/Goal.h"

TEST_CASE("ParseAndValidate accepts valid do_quest goal") {
    const std::string raw = R"({
        "goal": "do_quest",
        "params": {"quest_id": 502, "starting_objective_idx": 0},
        "reasoning": "Continue the existing quest",
        "ttl_minutes": 30
    })";
    auto result = ParseAndValidate(raw);
    REQUIRE(std::holds_alternative<ParsedGoal>(result));
    const auto& g = std::get<ParsedGoal>(result);
    CHECK(g.goal == GoalKind::DoQuest);
    CHECK(g.ttl_minutes == 30);
    REQUIRE(std::holds_alternative<DoQuestParams>(g.params));
    CHECK(std::get<DoQuestParams>(g.params).quest_id == 502u);
    CHECK(std::get<DoQuestParams>(g.params).starting_objective_idx == 0);
}

TEST_CASE("ParseAndValidate accepts valid go_grind goal") {
    const std::string raw = R"({
        "goal": "go_grind",
        "params": {"x": 100.5, "y": -50.0, "z": 12.0, "map_id": 0},
        "reasoning": "Nearby grinding spot",
        "ttl_minutes": 20
    })";
    auto result = ParseAndValidate(raw);
    REQUIRE(std::holds_alternative<ParsedGoal>(result));
    CHECK(std::get<ParsedGoal>(result).goal == GoalKind::GoGrind);
}

TEST_CASE("ParseAndValidate accepts valid rest goal") {
    const std::string raw = R"({
        "goal": "rest",
        "params": {},
        "reasoning": "Take a break",
        "ttl_minutes": 10
    })";
    auto result = ParseAndValidate(raw);
    REQUIRE(std::holds_alternative<ParsedGoal>(result));
    CHECK(std::get<ParsedGoal>(result).goal == GoalKind::Rest);
}

TEST_CASE("ParseAndValidate rejects unknown goal enum") {
    const std::string raw = R"({
        "goal": "do_pvp_arena",
        "params": {},
        "reasoning": "x",
        "ttl_minutes": 10
    })";
    auto result = ParseAndValidate(raw);
    REQUIRE(std::holds_alternative<ParseError>(result));
    CHECK(std::get<ParseError>(result).message.find("goal") != std::string::npos);
}

TEST_CASE("ParseAndValidate rejects missing required field") {
    const std::string raw = R"({
        "goal": "do_quest",
        "params": {"quest_id": 502}
    })";
    auto result = ParseAndValidate(raw);
    REQUIRE(std::holds_alternative<ParseError>(result));
}

TEST_CASE("ParseAndValidate rejects do_quest missing quest_id") {
    const std::string raw = R"({
        "goal": "do_quest",
        "params": {"starting_objective_idx": 0},
        "reasoning": "x",
        "ttl_minutes": 10
    })";
    auto result = ParseAndValidate(raw);
    REQUIRE(std::holds_alternative<ParseError>(result));
}

TEST_CASE("ParseAndValidate rejects malformed JSON") {
    const std::string raw = R"({"goal": "rest", "params":)";
    auto result = ParseAndValidate(raw);
    REQUIRE(std::holds_alternative<ParseError>(result));
}

TEST_CASE("ParseAndValidate rejects empty string") {
    auto result = ParseAndValidate("");
    REQUIRE(std::holds_alternative<ParseError>(result));
}

TEST_CASE("ParseAndValidate rejects ttl_minutes out of range") {
    const std::string raw = R"({
        "goal": "rest",
        "params": {},
        "reasoning": "x",
        "ttl_minutes": 100000
    })";
    auto result = ParseAndValidate(raw);
    REQUIRE(std::holds_alternative<ParseError>(result));
}

TEST_CASE("kGoalSchemaJson is non-empty JSON object") {
    auto j = nlohmann::json::parse(kGoalSchemaJson);
    CHECK(j.is_object());
    CHECK(j.contains("type"));
}
