#ifndef _PLAYERBOT_LLMAGENT_TIER0_DIGEST_H
#define _PLAYERBOT_LLMAGENT_TIER0_DIGEST_H

#include "Vendor/nlohmann_json.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

struct BotSelf {
    std::string name;
    std::string race;
    std::string character_class;  // "class" is a C++ keyword
    std::string spec;
    int32_t  level = 0;
    int32_t  hp_pct = 100;
    int32_t  mana_pct = -1;       // -1 = nullable (mapped to JSON null)
    int64_t  gold_copper = 0;
    bool     is_in_combat = false;
    bool     is_resting = false;
    bool     is_dead = false;
};

struct BotLocation {
    std::string map;
    std::string zone;
    std::string subzone;
    std::array<double, 3> position {0.0, 0.0, 0.0};
    std::vector<std::string> near_npcs;
};

struct BotGoal {
    std::string current;          // e.g. "Idle", "DoQuest"
    std::string params_json;      // verbatim JSON string of params
    int32_t  progress_pct = 0;
    int32_t  elapsed_minutes = 0;
    int32_t  ttl_minutes = 0;
};

struct QuestLogEntry {
    uint32_t    id = 0;
    std::string title;
    std::string progress;
};

struct InventoryHighlights {
    std::string bag_used;
    int64_t  junk_value_copper = 0;
    std::vector<std::string> consumables;
    double   gear_vs_level_score = 0.0;
};

struct NearbyHuman {
    std::string name;
    int32_t  level = 0;
    double   distance = 0.0;
};

struct RecentWhisper {
    std::string from;
    std::string text;
    int32_t  age_s = 0;
};

struct BotSocial {
    bool in_group = false;
    std::vector<std::string> group_members;
    std::string guild;            // empty = JSON null
    std::vector<NearbyHuman>  nearby_humans;
    std::vector<RecentWhisper> recent_whispers;
};

struct BotState {
    BotSelf                     self;
    BotLocation                 location;
    BotGoal                     goal;
    std::vector<QuestLogEntry>  quest_log;
    InventoryHighlights         inventory;
    BotSocial                   social;
    std::vector<std::string>    event_log;
};

nlohmann::json BuildDigestJson(const BotState& s);

#ifndef LLMAGENT_UNIT_TESTS
class PlayerbotAI;
BotState  SnapshotBot(PlayerbotAI* botAI);  // touches game state; worldserver-thread only
nlohmann::json BuildDigest(PlayerbotAI* botAI);  // convenience: snapshot + build
#endif  // LLMAGENT_UNIT_TESTS

#endif  // _PLAYERBOT_LLMAGENT_TIER0_DIGEST_H
