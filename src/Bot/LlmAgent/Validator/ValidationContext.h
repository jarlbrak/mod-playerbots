#ifndef _PLAYERBOT_LLMAGENT_VALIDATION_CONTEXT_H
#define _PLAYERBOT_LLMAGENT_VALIDATION_CONTEXT_H

#include <cstdint>
#include <unordered_set>
#include <vector>

struct QuestLogContextEntry {
    uint32_t id              = 0;
    uint32_t status          = 0;        // 0=incomplete, 1=complete-not-turned-in, 2=complete-and-turned-in
    int32_t  objective_count = 0;
};

struct BotValidationContext {
    uint32_t bot_level = 0;
    uint32_t map_id    = 0;
    double   map_min_x = -100000.0;
    double   map_max_x =  100000.0;
    double   map_min_y = -100000.0;
    double   map_max_y =  100000.0;

    std::vector<QuestLogContextEntry>  quest_log;
    std::vector<uint64_t>              nearby_creature_guids;
    std::unordered_set<uint32_t>       known_flight_node_ids;
    std::unordered_set<uint32_t>       valid_capture_point_spawn_ids;
};

#ifndef LLMAGENT_UNIT_TESTS
class PlayerbotAI;
BotValidationContext SnapshotForValidation(PlayerbotAI* botAI);
#endif

#endif
