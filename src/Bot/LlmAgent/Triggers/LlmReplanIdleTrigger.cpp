#include "Triggers/LlmReplanIdleTrigger.h"
#include "LlmAgentManager.h"
#include "PlayerbotAI.h"
#include "NewRpgInfo.h"

bool LlmReplanIdleTrigger::IsActive() {
    if (!botAI) return false;
    auto& mgr = LlmAgentManager::Instance();
    if (!mgr.Enabled()) return false;
    Player* bot = botAI->GetBot();
    if (!bot) return false;
    uint64_t guid = bot->GetGUID().GetRawValue();

    // Always allow draining results that are already on the stack.
    if (mgr.HasPendingResults(guid)) return true;

    // Enqueue path requires sample/opt-in AND eligible cooldown AND Idle AND not in-flight.
    if (!mgr.Selector().IsLlmBot(guid)) return false;
    if (!mgr.Cooldowns().Eligible(guid)) return false;
    if (botAI->rpgInfo.GetStatus() != RPG_IDLE) return false;
    if (mgr.IsInFlight(guid)) return false;
    return true;
}
