#include "doctest.h"
#include "Schemas/ChatEnvelope.h"

TEST_CASE("ParseChatEnvelope happy path: utterance + one side_effect") {
    const std::string raw = R"({
        "utterance": "Sure, give me a sec.",
        "side_effects": [
            {"name": "accept_party_invite", "arguments": {"from": "Bob"}}
        ]
    })";
    auto result = ParseChatEnvelope(raw);
    REQUIRE(std::holds_alternative<ParsedChatEnvelope>(result));
    const auto& env = std::get<ParsedChatEnvelope>(result);
    CHECK(env.utterance == "Sure, give me a sec.");
    REQUIRE(env.side_effects.size() == 1);
    CHECK(std::holds_alternative<AcceptPartyInviteCall>(env.side_effects[0]));
    CHECK(std::get<AcceptPartyInviteCall>(env.side_effects[0]).from == "Bob");
}

TEST_CASE("ParseChatEnvelope happy path: empty side_effects") {
    const std::string raw = R"({"utterance": "hi", "side_effects": []})";
    auto result = ParseChatEnvelope(raw);
    REQUIRE(std::holds_alternative<ParsedChatEnvelope>(result));
    const auto& env = std::get<ParsedChatEnvelope>(result);
    CHECK(env.utterance == "hi");
    CHECK(env.side_effects.empty());
}

TEST_CASE("ParseChatEnvelope rejects missing utterance") {
    const std::string raw = R"({"side_effects": []})";
    auto result = ParseChatEnvelope(raw);
    REQUIRE(std::holds_alternative<ParseError>(result));
    CHECK(std::get<ParseError>(result).message.find("utterance") != std::string::npos);
}

TEST_CASE("ParseChatEnvelope rejects utterance not a string") {
    const std::string raw = R"({"utterance": 42, "side_effects": []})";
    auto result = ParseChatEnvelope(raw);
    REQUIRE(std::holds_alternative<ParseError>(result));
}

TEST_CASE("ParseChatEnvelope rejects side_effects not an array") {
    const std::string raw = R"({"utterance": "hi", "side_effects": {}})";
    auto result = ParseChatEnvelope(raw);
    REQUIRE(std::holds_alternative<ParseError>(result));
    CHECK(std::get<ParseError>(result).message.find("side_effects") != std::string::npos);
}

TEST_CASE("ParseChatEnvelope rejects malformed JSON") {
    auto result = ParseChatEnvelope("not json");
    REQUIRE(std::holds_alternative<ParseError>(result));
}

TEST_CASE("ParseChatEnvelope rejects side_effect element failing inner schema") {
    // accept_party_invite without 'from' field
    const std::string raw = R"({
        "utterance": "ok",
        "side_effects": [{"name": "accept_party_invite", "arguments": {}}]
    })";
    auto result = ParseChatEnvelope(raw);
    REQUIRE(std::holds_alternative<ParseError>(result));
    CHECK(std::get<ParseError>(result).message.find("from") != std::string::npos);
}
