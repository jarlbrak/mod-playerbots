#include "doctest.h"
#include "Tiers/Tier0_StateDigest.h"

namespace {

LlmBotState make_grimblade() {
    LlmBotState s;
    s.self.name = "Grimblade";
    s.self.race = "orc";
    s.self.character_class = "warrior";
    s.self.spec = "arms";
    s.self.level = 37;
    s.self.hp_pct = 84;
    s.self.gold_copper = 32841;
    s.self.is_in_combat = false;
    s.self.is_resting = true;
    s.self.is_dead = false;
    s.location.map = "Eastern Kingdoms";
    s.location.zone = "Hillsbrad Foothills";
    s.location.subzone = "Tarren Mill";
    s.location.position = {123.4, -56.7, 12.1};
    s.location.near_npcs = {"Innkeeper Anchorite Truuen"};
    s.goal.current = "DoQuest";
    s.goal.progress_pct = 40;
    s.goal.elapsed_minutes = 8;
    s.goal.ttl_minutes = 22;
    s.goal.params_json = R"({"quest_id":502,"objective_idx":1})";
    s.quest_log.push_back({502, "Syndicate Assassins", "12/20"});
    s.quest_log.push_back({488, "The Killing Fields", "complete, turn in"});
    s.inventory.bag_used = "22/24";
    s.inventory.junk_value_copper = 4200;
    s.inventory.consumables = {"8x healing potion (lvl 35)"};
    s.inventory.gear_vs_level_score = 0.78;
    s.social.in_group = false;
    s.social.nearby_humans.push_back({"RealPlayerBob", 38, 18.2});
    s.social.recent_whispers.push_back({"RealPlayerBob", "wanna group?", 3});
    s.event_log = {"Killed Syndicate Footpad (+1 progress)", "RealPlayerBob whispered: wanna group?"};
    return s;
}

}  // namespace

TEST_CASE("BuildDigestJson has all top-level §6 fields") {
    auto j = BuildDigestJson(make_grimblade());
    CHECK(j.contains("self"));
    CHECK(j.contains("location"));
    CHECK(j.contains("goal"));
    CHECK(j.contains("quest_log"));
    CHECK(j.contains("inventory_highlights"));
    CHECK(j.contains("social"));
    CHECK(j.contains("event_log"));
    CHECK(j.contains("memory_hints"));
}

TEST_CASE("BuildDigestJson memory_hints is empty array in Phase 1") {
    auto j = BuildDigestJson(make_grimblade());
    CHECK(j["memory_hints"].is_array());
    CHECK(j["memory_hints"].empty());
}

TEST_CASE("BuildDigestJson self block has expected types") {
    auto j = BuildDigestJson(make_grimblade());
    CHECK(j["self"]["name"].get<std::string>() == "Grimblade");
    CHECK(j["self"]["level"].get<int>() == 37);
    CHECK(j["self"]["hp_pct"].get<int>() == 84);
    CHECK(j["self"]["is_resting"].get<bool>() == true);
}

TEST_CASE("BuildDigestJson location block includes position array") {
    auto j = BuildDigestJson(make_grimblade());
    REQUIRE(j["location"]["position"].is_array());
    CHECK(j["location"]["position"].size() == 3);
    CHECK(j["location"]["zone"].get<std::string>() == "Hillsbrad Foothills");
}

TEST_CASE("BuildDigestJson quest_log preserves order and shape") {
    auto j = BuildDigestJson(make_grimblade());
    REQUIRE(j["quest_log"].is_array());
    REQUIRE(j["quest_log"].size() == 2);
    CHECK(j["quest_log"][0]["id"].get<int>() == 502);
    CHECK(j["quest_log"][1]["title"].get<std::string>() == "The Killing Fields");
}

TEST_CASE("BuildDigestJson social.recent_whispers preserved") {
    auto j = BuildDigestJson(make_grimblade());
    REQUIRE(j["social"]["recent_whispers"].is_array());
    REQUIRE(j["social"]["recent_whispers"].size() == 1);
    CHECK(j["social"]["recent_whispers"][0]["from"].get<std::string>() == "RealPlayerBob");
}

TEST_CASE("BuildDigestJson event_log is array of strings") {
    auto j = BuildDigestJson(make_grimblade());
    REQUIRE(j["event_log"].is_array());
    CHECK(j["event_log"].size() == 2);
}

TEST_CASE("BuildDigestJson handles empty LlmBotState without crashing") {
    LlmBotState empty;
    auto j = BuildDigestJson(empty);
    CHECK(j["quest_log"].is_array());
    CHECK(j["event_log"].is_array());
    CHECK(j["social"]["nearby_humans"].is_array());
}
