#include "Triggers/LlmChatTrigger.h"
#include "LlmAgentManager.h"
#include "Log.h"
#include "PlayerbotAI.h"
#include "Player.h"

#include <atomic>
#include <ctime>

bool LlmChatTrigger::IsActive() {
    if (!botAI) return false;
    auto& mgr = LlmAgentManager::Instance();
    const auto& cfg = mgr.Config();
    if (!mgr.Enabled() || !cfg.Tier3_Enabled) return false;
    Player* bot = botAI->GetBot();
    if (!bot) return false;
    const uint64_t guid = bot->GetGUID().GetRawValue();

    // One-shot log to confirm trigger gets dispatched at all. Remove once stable.
    static std::atomic<bool> first_call{true};
    if (first_call.exchange(false)) {
        LOG_INFO("playerbots", "[LlmAgent] LlmChatTrigger::IsActive first dispatch — bot guid={}", guid);
    }

    // Drain side: T3 results waiting.
    if (mgr.HasPendingResults(guid, /*tier*/3)) return true;

    // Enqueue side: need pending interaction + LLM-enabled + cooldown clear + not in-flight.
    if (!mgr.Selector().IsLlmBot(guid)) return false;
    if (!mgr.T3Cooldowns().Eligible(guid)) return false;
    if (mgr.IsInFlight(guid, /*tier*/3)) return false;

    mgr.Interactions().ExpireOlderThan(
        guid, static_cast<int64_t>(time(nullptr)),
        cfg.Tier3_WhisperWindowSeconds);
    return mgr.Interactions().HasPending(guid);
}
