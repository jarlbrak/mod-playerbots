#include "Validator/ValidationContext.h"

#ifndef LLMAGENT_UNIT_TESTS

#include "PlayerbotAI.h"
#include "Player.h"
#include "Map.h"
#include "QuestDef.h"

BotValidationContext SnapshotForValidation(PlayerbotAI* botAI) {
    BotValidationContext c;
    if (!botAI) return c;
    Player* bot = botAI->GetBot();
    if (!bot) return c;

    c.bot_level = bot->GetLevel();
    if (Map* m = bot->GetMap()) {
        c.map_id = m->GetId();
    }
    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot) {
        uint32 qid = bot->GetQuestSlotQuestId(slot);
        if (!qid) continue;
        const Quest* q = sObjectMgr->GetQuestTemplate(qid);
        if (!q) continue;
        QuestLogContextEntry e;
        e.id = qid;
        QuestStatus st = bot->GetQuestStatus(qid);
        e.status = (st == QUEST_STATUS_COMPLETE) ? 1u : 0u;
        e.objective_count = 3;  // conservative default; AC API for objective count varies by version
        c.quest_log.push_back(e);
    }
    // Map bounds, nearby_creature_guids, known_flight_node_ids,
    // valid_capture_point_spawn_ids: leave defaults / empty for Phase 2.
    // The validator's permissive branch covers empty nearby_creature_guids;
    // flight/capture point variants hard-reject (fallback covers those).

    return c;
}

#endif
