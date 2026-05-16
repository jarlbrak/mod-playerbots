#include "Triggers/LlmChatTrigger.h"
#include "LlmAgentManager.h"
#include "Log.h"
#include "PlayerbotAI.h"
#include "Player.h"

#include <chrono>
#include <ctime>
#include <unordered_map>
#include <mutex>

namespace {
// Rate-limit per-bot trigger-evaluation logs to once every 5 seconds so the
// log doesn't drown the server. Phase 5.2 diagnostic only.
std::mutex g_log_mu;
std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> g_last_log;

bool should_log(uint64_t guid) {
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> g(g_log_mu);
    auto& last = g_last_log[guid];
    if (now - last < std::chrono::seconds(5)) return false;
    last = now;
    return true;
}
}  // namespace

bool LlmChatTrigger::IsActive() {
    if (!botAI) return false;
    auto& mgr = LlmAgentManager::Instance();
    const auto& cfg = mgr.Config();
    if (!mgr.Enabled() || !cfg.Tier3_Enabled) return false;
    Player* bot = botAI->GetBot();
    if (!bot) return false;
    const uint64_t guid = bot->GetGUID().GetRawValue();

    // Drain side: T3 results waiting.
    if (mgr.HasPendingResults(guid, /*tier*/3)) return true;

    // Phase 5.2 diagnostic: log the gate values for every bot when pending
    // interactions exist, so we can localize why grouped bots don't fire.
    const bool has_pending_raw = mgr.Interactions().HasPending(guid);
    if (has_pending_raw && should_log(guid)) {
        const bool is_llm_bot = mgr.Selector().IsLlmBot(guid);
        const bool cd_eligible = mgr.T3Cooldowns().Eligible(guid);
        const bool in_flight = mgr.IsInFlight(guid, /*tier*/3);
        LOG_INFO("server.loading",
                 "[LlmAgent] LlmChatTrigger gate: bot='{}' guid={} pending=1 is_llm_bot={} cd_eligible={} in_flight={}",
                 bot->GetName(), guid, is_llm_bot ? 1 : 0, cd_eligible ? 1 : 0, in_flight ? 1 : 0);
    }

    // Enqueue side: need pending interaction + LLM-enabled + cooldown clear + not in-flight.
    if (!mgr.Selector().IsLlmBot(guid)) return false;
    if (!mgr.T3Cooldowns().Eligible(guid)) return false;
    if (mgr.IsInFlight(guid, /*tier*/3)) return false;

    mgr.Interactions().ExpireOlderThan(
        guid, static_cast<int64_t>(time(nullptr)),
        cfg.Tier3_WhisperWindowSeconds);
    return mgr.Interactions().HasPending(guid);
}
