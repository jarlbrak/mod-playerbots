#include "Tools/ToolExecutors.h"

#ifndef LLMAGENT_UNIT_TESTS

#include "Apply/ApplyGoal.h"
#include "LlmAgentManager.h"
#include "PlayerbotAI.h"
#include "Player.h"
#include "Group.h"
#include "ObjectMgr.h"
#include "ObjectAccessor.h"
#include "Bag.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"

#include <type_traits>
#include <variant>

namespace LlmAgentTools {

bool ApplyAcceptPartyInvite(const AcceptPartyInviteCall& c, PlayerbotAI* botAI) {
    if (!botAI) return false;
    Player* bot = botAI->GetBot();
    if (!bot || !bot->GetSession()) return false;
    try {
        // AC's HandleGroupAcceptOpcode reads from a WorldPacket; in WoTLK the
        // accept body is empty, so a default-constructed WorldPacket suffices.
        WorldPacket pkt;
        bot->GetSession()->HandleGroupAcceptOpcode(pkt);
        return true;
    } catch (...) {
        LOG_ERROR("playerbots", "[LlmAgent] ApplyAcceptPartyInvite threw (from={})", c.from);
        try {
            WorldPacket pkt;
            bot->GetSession()->HandleGroupDeclineOpcode(pkt);
        } catch (...) { /* swallow recovery exception */ }
        return false;
    }
}

bool ApplyLeaveParty(const LeavePartyCall&, PlayerbotAI* botAI) {
    if (!botAI) return false;
    Player* bot = botAI->GetBot();
    if (!bot) return false;
    try {
        bot->RemoveFromGroup();
        return true;
    } catch (...) {
        LOG_ERROR("playerbots", "[LlmAgent] ApplyLeaveParty threw");
        return false;
    }
}

bool ApplyAcceptQuest(const AcceptQuestCall& c, PlayerbotAI* botAI) {
    if (!botAI) return false;
    Player* bot = botAI->GetBot();
    if (!bot) return false;
    try {
        const Quest* q = sObjectMgr->GetQuestTemplate(c.quest_id);
        if (!q) return false;
        bot->AddQuestAndCheckCompletion(const_cast<Quest*>(q), /*questGiver=*/nullptr);
        return true;
    } catch (...) {
        LOG_ERROR("playerbots", "[LlmAgent] ApplyAcceptQuest threw (quest_id={})", c.quest_id);
        return false;
    }
}

bool ApplyTurnInQuest(const TurnInQuestCall& c, PlayerbotAI* botAI) {
    if (!botAI) return false;
    Player* bot = botAI->GetBot();
    if (!bot) return false;
    try {
        const Quest* q = sObjectMgr->GetQuestTemplate(c.quest_id);
        if (!q) return false;
        bot->RewardQuest(const_cast<Quest*>(q), /*reward=*/0, /*questGiver=*/bot);
        return true;
    } catch (...) {
        LOG_ERROR("playerbots", "[LlmAgent] ApplyTurnInQuest threw (quest_id={})", c.quest_id);
        return false;
    }
}

bool ApplySetGoal(const SetGoalCall& c, PlayerbotAI* botAI) {
    return LlmAgentApply::ApplyGoalToRpgInfo(c.goal, botAI);
}

bool ApplyVendorJunk(const VendorJunkCall&, PlayerbotAI* botAI) {
    if (!botAI) return false;
    Player* bot = botAI->GetBot();
    if (!bot) return false;
    try {
        // Iterate backpack (slots 23..38) + 4 bag slots (INVENTORY_SLOT_BAG_START..END);
        // sell grey (quality 0). Pragmatic — no real vendor packet, just credit value.
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot) {
            if (Item* it = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot)) {
                if (it->GetTemplate() && it->GetTemplate()->Quality == ITEM_QUALITY_POOR) {
                    uint32 value = it->GetTemplate()->SellPrice * it->GetCount();
                    bot->ModifyMoney(value);
                    bot->DestroyItemCount(it->GetEntry(), it->GetCount(), true);
                }
            }
        }
        for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag) {
            if (Bag* b = bot->GetBagByPos(bag)) {
                uint32 size = b->GetBagSize();
                for (uint32 s = 0; s < size; ++s) {
                    if (Item* it = b->GetItemByPos(s)) {
                        if (it->GetTemplate() && it->GetTemplate()->Quality == ITEM_QUALITY_POOR) {
                            uint32 value = it->GetTemplate()->SellPrice * it->GetCount();
                            bot->ModifyMoney(value);
                            bot->DestroyItemCount(it->GetEntry(), it->GetCount(), true);
                        }
                    }
                }
            }
        }
        return true;
    } catch (...) {
        LOG_ERROR("playerbots", "[LlmAgent] ApplyVendorJunk threw");
        return false;
    }
}

bool ApplyMemoryRemember(const MemoryRememberCall& c, PlayerbotAI* botAI) {
    if (!botAI) return false;
    Player* bot = botAI->GetBot();
    if (!bot) return false;
    return LlmAgentManager::Instance().MemoryClient().Remember(
        bot->GetGUID().GetRawValue(),
        c.text, c.entities, c.salience, c.relations);
}

bool ApplyToolCall(const ParsedToolCall& call, PlayerbotAI* botAI) {
    return std::visit([botAI](auto const& tool) -> bool {
        using T = std::decay_t<decltype(tool)>;
        if constexpr (std::is_same_v<T, AcceptPartyInviteCall>)
            return ApplyAcceptPartyInvite(tool, botAI);
        else if constexpr (std::is_same_v<T, LeavePartyCall>)
            return ApplyLeaveParty(tool, botAI);
        else if constexpr (std::is_same_v<T, AcceptQuestCall>)
            return ApplyAcceptQuest(tool, botAI);
        else if constexpr (std::is_same_v<T, TurnInQuestCall>)
            return ApplyTurnInQuest(tool, botAI);
        else if constexpr (std::is_same_v<T, SetGoalCall>)
            return ApplySetGoal(tool, botAI);
        else if constexpr (std::is_same_v<T, VendorJunkCall>)
            return ApplyVendorJunk(tool, botAI);
        else if constexpr (std::is_same_v<T, MemoryRememberCall>)
            return ApplyMemoryRemember(tool, botAI);
        else
            return false;
    }, call);
}

}  // namespace LlmAgentTools

#endif
