#include "Actions/LlmReplanIdleAction.h"
#include "LlmAgentManager.h"
#include "Tiers/Tier0_StateDigest.h"
#include "Schemas/Goal.h"
#include "PlayerbotAI.h"
#include "Log.h"
#include "NewRpgInfo.h"

#include <atomic>
#include <chrono>

bool LlmReplanIdleAction::Execute(Event /*event*/) {
    if (!botAI) return false;
    auto& mgr = LlmAgentManager::Instance();
    if (!mgr.Enabled()) return false;
    Player* bot = botAI->GetBot();
    if (!bot) return false;
    uint64_t guid = bot->GetGUID().GetRawValue();

    // 1. Drain any completed results.
    auto results = mgr.DrainResults(guid);
    for (const auto& r : results) {
        if (r.parsed_status == "ok" && !r.parsed_goal.is_null()) {
            LOG_INFO("playerbots",
                     "[LlmAgent] bot={} proposed={} lat={}ms",
                     r.bot_name,
                     r.parsed_goal.value("goal", std::string{"?"}),
                     r.inference_ms);
        } else if (r.parsed_status == "schema_error") {
            // schema_error is rare and interesting — never throttle.
            LOG_WARN("playerbots",
                     "[LlmAgent] bot={} status=schema_error err='{}' lat={}ms",
                     r.bot_name, r.validator_error, r.inference_ms);
        } else {
            // transport_error / http_error / timeout — throttle to 1/60s server-wide.
            using clock = std::chrono::steady_clock;
            static std::atomic<int64_t> last_warn_ms{0};
            auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              clock::now().time_since_epoch()).count();
            int64_t prev = last_warn_ms.load();
            if (now_ms - prev >= 60000 &&
                last_warn_ms.compare_exchange_strong(prev, now_ms)) {
                LOG_WARN("playerbots",
                         "[LlmAgent] bot={} status={} err='{}' lat={}ms (further warnings suppressed 60s)",
                         r.bot_name, r.parsed_status, r.validator_error, r.inference_ms);
            }
        }
    }

    // 2. Enqueue a new request if eligible.
    if (botAI->rpgInfo.GetStatus() != RPG_IDLE) return false;
    if (mgr.IsInFlight(guid)) return false;

    BotState state;
    try {
        state = SnapshotBot(botAI);
    } catch (...) {
        LOG_WARN("playerbots", "[LlmAgent] SnapshotBot threw; skipping enqueue");
        return false;
    }
    nlohmann::json digest = BuildDigestJson(state);

    const LlmAgentConfig& cfg = mgr.Config();
    nlohmann::json body;
    body["model"] = cfg.Model;
    body["messages"] = nlohmann::json::array();
    body["messages"].push_back({{"role", "system"}, {"content", cfg.SystemPrompt}});
    body["messages"].push_back({{"role", "user"},   {"content", digest.dump()}});
    body["response_format"] = {
        {"type", "json_schema"},
        {"json_schema", {{"name", "BotGoal"}, {"schema", nlohmann::json::parse(kGoalSchemaJson)}}}
    };
    body["temperature"] = 0.4;
    body["max_tokens"] = 256;

    LlmRequest req;
    req.bot_guid    = guid;
    req.bot_name    = bot->GetName();
    req.body_json   = body.dump();
    req.digest_json = std::move(digest);
    mgr.Enqueue(std::move(req));

    return false;  // do not suppress the rule-based NewRpgStatusUpdateAction
}
