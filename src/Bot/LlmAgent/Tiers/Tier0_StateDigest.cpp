#include "Tiers/Tier0_StateDigest.h"

nlohmann::json BuildDigestJson(const BotState& s) {
    nlohmann::json j;

    j["self"] = {
        {"name",          s.self.name},
        {"race",          s.self.race},
        {"class",         s.self.character_class},
        {"spec",          s.self.spec},
        {"level",         s.self.level},
        {"hp_pct",        s.self.hp_pct},
        {"mana_pct",      s.self.mana_pct < 0 ? nlohmann::json(nullptr) : nlohmann::json(s.self.mana_pct)},
        {"gold_copper",   s.self.gold_copper},
        {"is_in_combat",  s.self.is_in_combat},
        {"is_resting",    s.self.is_resting},
        {"is_dead",       s.self.is_dead},
    };

    j["location"] = {
        {"map",       s.location.map},
        {"zone",      s.location.zone},
        {"subzone",   s.location.subzone},
        {"position",  s.location.position},
        {"near_npcs", s.location.near_npcs},
    };

    // goal.params is a verbatim JSON string; parse so the digest doesn't
    // contain a string that itself contains JSON.
    nlohmann::json goal_params = nlohmann::json::object();
    if (!s.goal.params_json.empty()) {
        try { goal_params = nlohmann::json::parse(s.goal.params_json); }
        catch (...) { goal_params = nlohmann::json::object(); }
    }
    j["goal"] = {
        {"current",         s.goal.current},
        {"params",          goal_params},
        {"progress_pct",    s.goal.progress_pct},
        {"elapsed_minutes", s.goal.elapsed_minutes},
        {"ttl_minutes",     s.goal.ttl_minutes},
    };

    j["quest_log"] = nlohmann::json::array();
    for (const auto& q : s.quest_log) {
        j["quest_log"].push_back({{"id", q.id}, {"title", q.title}, {"progress", q.progress}});
    }

    j["inventory_highlights"] = {
        {"bag_used",            s.inventory.bag_used},
        {"junk_value_copper",   s.inventory.junk_value_copper},
        {"consumables",         s.inventory.consumables},
        {"gear_vs_level_score", s.inventory.gear_vs_level_score},
    };

    nlohmann::json humans = nlohmann::json::array();
    for (const auto& h : s.social.nearby_humans) {
        humans.push_back({{"name", h.name}, {"level", h.level}, {"distance", h.distance}});
    }
    nlohmann::json whispers = nlohmann::json::array();
    for (const auto& w : s.social.recent_whispers) {
        whispers.push_back({{"from", w.from}, {"text", w.text}, {"age_s", w.age_s}});
    }
    j["social"] = {
        {"in_group",         s.social.in_group},
        {"group_members",    s.social.group_members},
        {"guild",            s.social.guild.empty() ? nlohmann::json(nullptr) : nlohmann::json(s.social.guild)},
        {"nearby_humans",    humans},
        {"recent_whispers",  whispers},
    };

    j["event_log"] = s.event_log;
    j["memory_hints"] = nlohmann::json::array();  // Phase 1: always empty

    return j;
}
