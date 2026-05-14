#ifndef _PLAYERBOT_LLMAGENT_INTERACTION_CONTEXT_H
#define _PLAYERBOT_LLMAGENT_INTERACTION_CONTEXT_H

#include "Validator/ValidationContext.h"   // for QuestLogContextEntry
#include "EventBuffer/InteractionEventBuffer.h"
#include <cstdint>
#include <string>
#include <vector>

struct NearbyCreature {
    uint64_t guid = 0;
    std::string name;
    std::string type;
    bool        in_range_10y = false;
    uint32_t    is_quest_giver_for = 0;   // 0 = none, else quest_id
    uint32_t    is_turn_in_for     = 0;
    bool        is_vendor = false;
};

struct InteractionContext {
    uint32_t bot_level = 0;
    uint32_t map_id = 0;
    double   map_min_x = -100000.0, map_max_x = 100000.0;
    double   map_min_y = -100000.0, map_max_y = 100000.0;

    std::vector<PendingInvite>           pending_invites;
    std::vector<QuestLogContextEntry>    quest_log;
    std::vector<NearbyCreature>          nearby_creatures;
    bool                                 in_group = false;
};

#ifndef LLMAGENT_UNIT_TESTS
class PlayerbotAI;
InteractionContext SnapshotInteractionContext(PlayerbotAI* botAI);
#endif

#endif
