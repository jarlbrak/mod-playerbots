#include "Tools/InteractionContext.h"

#ifndef LLMAGENT_UNIT_TESTS

#include "LlmAgentManager.h"
#include "PlayerbotAI.h"
#include "Player.h"
#include "Map.h"
#include "QuestDef.h"
#include "ObjectMgr.h"

InteractionContext SnapshotInteractionContext(PlayerbotAI* botAI) {
    InteractionContext ctx;
    if (!botAI) return ctx;
    Player* bot = botAI->GetBot();
    if (!bot) return ctx;
    ctx.bot_level = bot->GetLevel();
    if (Map* m = bot->GetMap()) ctx.map_id = m->GetId();

    // FIXME Task 5: pull from LlmAgentManager::Interactions()
    // auto& mgr = LlmAgentManager::Instance();
    // auto payload = mgr.Interactions().SnapshotFor(bot->GetGUID().GetRawValue());
    // ctx.pending_invites = std::move(payload.pending_invites);

    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot) {
        uint32 qid = bot->GetQuestSlotQuestId(slot);
        if (!qid) continue;
        const Quest* q = sObjectMgr->GetQuestTemplate(qid);
        if (!q) continue;
        QuestLogContextEntry e;
        e.id = qid;
        QuestStatus st = bot->GetQuestStatus(qid);
        e.status = (st == QUEST_STATUS_COMPLETE) ? 1u : 0u;
        e.objective_count = 3;  // conservative
        ctx.quest_log.push_back(e);
    }

    ctx.in_group = bot->GetGroup() != nullptr;

    // nearby_creatures left empty for Phase 4; Phase 4.1 wires real grid scans.
    // accept_quest/turn_in_quest/vendor_junk hard-reject as a result, which is
    // the correct conservative posture.

    return ctx;
}

#endif
