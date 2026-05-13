#include "Triggers/LlmReplanIdleTrigger.h"
#include "LlmAgentManager.h"
#include "Playerbots/PlayerbotAI.h"
#include "NewRpgInfo.h"

bool LlmReplanIdleTrigger::IsActive() {
    if (!botAI) return false;
    auto& mgr = LlmAgentManager::Instance();
    if (!mgr.Enabled()) return false;
    Player* bot = botAI->GetBot();
    if (!bot) return false;
    uint64_t guid = bot->GetGUID().GetRawValue();

    if (mgr.HasPendingResults(guid)) return true;

    if (botAI->rpgInfo.GetStatus() != RPG_IDLE) return false;
    if (mgr.IsInFlight(guid)) return false;
    return true;
}
