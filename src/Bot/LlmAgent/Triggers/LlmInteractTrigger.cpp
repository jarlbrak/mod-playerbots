#include "Triggers/LlmInteractTrigger.h"
#include "LlmAgentManager.h"
#include "PlayerbotAI.h"
#include "Player.h"

#include <ctime>

bool LlmInteractTrigger::IsActive() {
    if (!botAI) return false;
    auto& mgr = LlmAgentManager::Instance();
    const auto& cfg = mgr.Config();
    if (!mgr.Enabled() || !cfg.Tier2_Enabled) return false;
    Player* bot = botAI->GetBot();
    if (!bot) return false;
    uint64_t guid = bot->GetGUID().GetRawValue();

    if (!mgr.Selector().IsLlmBot(guid)) return false;
    if (!mgr.Cooldowns().Eligible(guid)) return false;

    mgr.Interactions().ExpireOlderThan(
        guid, static_cast<int64_t>(time(nullptr)),
        cfg.Tier2_WhisperWindowSeconds);
    return mgr.Interactions().HasPending(guid);
}
