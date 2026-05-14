#include "doctest.h"
#include "Tools/ToolCatalog.h"

TEST_CASE("ParseToolCalls accepts a single accept_party_invite") {
    const std::string raw = R"([
        {"name": "accept_party_invite", "arguments": "{\"from\":\"RealPlayerBob\"}"}
    ])";
    auto result = ParseToolCalls(raw);
    REQUIRE(std::holds_alternative<std::vector<ParsedToolCall>>(result));
    const auto& calls = std::get<std::vector<ParsedToolCall>>(result);
    REQUIRE(calls.size() == 1);
    REQUIRE(std::holds_alternative<AcceptPartyInviteCall>(calls[0]));
    CHECK(std::get<AcceptPartyInviteCall>(calls[0]).from == "RealPlayerBob");
}

TEST_CASE("ParseToolCalls accepts leave_party with empty args") {
    const std::string raw = R"([
        {"name": "leave_party", "arguments": "{}"}
    ])";
    auto result = ParseToolCalls(raw);
    REQUIRE(std::holds_alternative<std::vector<ParsedToolCall>>(result));
    const auto& calls = std::get<std::vector<ParsedToolCall>>(result);
    REQUIRE(calls.size() == 1);
    CHECK(std::holds_alternative<LeavePartyCall>(calls[0]));
}

TEST_CASE("ParseToolCalls accepts accept_quest with quest_id + npc_name") {
    const std::string raw = R"([
        {"name": "accept_quest",
         "arguments": "{\"quest_id\":502, \"from_npc_name\":\"Marshal Dughan\"}"}
    ])";
    auto result = ParseToolCalls(raw);
    REQUIRE(std::holds_alternative<std::vector<ParsedToolCall>>(result));
    const auto& c = std::get<AcceptQuestCall>(std::get<std::vector<ParsedToolCall>>(result)[0]);
    CHECK(c.quest_id == 502u);
    CHECK(c.from_npc_name == "Marshal Dughan");
}

TEST_CASE("ParseToolCalls returns multiple calls in order") {
    const std::string raw = R"([
        {"name": "accept_party_invite", "arguments": "{\"from\":\"Bob\"}"},
        {"name": "set_goal", "arguments": "{\"goal\":\"rest\",\"params\":{},\"reasoning\":\"x\",\"ttl_minutes\":5}"}
    ])";
    auto result = ParseToolCalls(raw);
    REQUIRE(std::holds_alternative<std::vector<ParsedToolCall>>(result));
    const auto& calls = std::get<std::vector<ParsedToolCall>>(result);
    REQUIRE(calls.size() == 2);
    CHECK(std::holds_alternative<AcceptPartyInviteCall>(calls[0]));
    CHECK(std::holds_alternative<SetGoalCall>(calls[1]));
}

TEST_CASE("ParseToolCalls rejects unknown tool name") {
    const std::string raw = R"([
        {"name": "wipe_database", "arguments": "{}"}
    ])";
    auto result = ParseToolCalls(raw);
    REQUIRE(std::holds_alternative<ParseError>(result));
}

TEST_CASE("ParseToolCalls rejects malformed top-level JSON") {
    auto result = ParseToolCalls("not json");
    REQUIRE(std::holds_alternative<ParseError>(result));
}

TEST_CASE("ParseToolCalls returns empty list on empty array (model declined)") {
    auto result = ParseToolCalls("[]");
    REQUIRE(std::holds_alternative<std::vector<ParsedToolCall>>(result));
    CHECK(std::get<std::vector<ParsedToolCall>>(result).empty());
}

TEST_CASE("ParseToolCalls rejects missing required field") {
    const std::string raw = R"([
        {"name": "accept_party_invite", "arguments": "{}"}
    ])";
    auto result = ParseToolCalls(raw);
    REQUIRE(std::holds_alternative<ParseError>(result));
}
