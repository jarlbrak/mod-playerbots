#include "Tiers/Tier2_Interactive.h"

#ifndef LLMAGENT_UNIT_TESTS

#include "Tiers/Tier0_StateDigest.h"
#include "Tools/ToolCatalog.h"
#include "LlmAgentManager.h"
#include "PlayerbotAI.h"
#include "Player.h"

#include <ctime>

namespace LlmAgentTier2 {

nlohmann::json BuildT2Digest(PlayerbotAI* botAI) {
    LlmBotState state = SnapshotBot(botAI);
    nlohmann::json base = BuildDigestJson(state);

    auto& mgr = LlmAgentManager::Instance();
    auto payload = mgr.Interactions().SnapshotFor(botAI->GetBot()->GetGUID().GetRawValue());

    nlohmann::json interactions = {
        {"pending_invites",     nlohmann::json::array()},
        {"recent_whispers",     nlohmann::json::array()},
        {"recent_group_joins",  nlohmann::json::array()},
    };
    const int64_t now = static_cast<int64_t>(time(nullptr));
    for (const auto& inv : payload.pending_invites)
        interactions["pending_invites"].push_back({{"from", inv.from_name}, {"ts", inv.ts}});
    for (const auto& w : payload.recent_whispers)
        interactions["recent_whispers"].push_back(
            {{"from", w.from_name}, {"text", w.text}, {"age_s", now - w.ts}});
    for (const auto& j : payload.recent_group_joins)
        interactions["recent_group_joins"].push_back(
            {{"leader", j.leader_name}, {"ts", j.ts}});

    base["interaction_context"] = interactions;
    return base;
}

std::string BuildT2RequestBody(PlayerbotAI* botAI) {
    auto& mgr = LlmAgentManager::Instance();
    const auto& cfg = mgr.Config();
    nlohmann::json digest = BuildT2Digest(botAI);

    nlohmann::json body;
    body["model"]    = cfg.Model;
    body["messages"] = nlohmann::json::array();
    body["messages"].push_back({{"role", "system"}, {"content", cfg.Tier2_SystemPrompt}});
    body["messages"].push_back({{"role", "user"},   {"content", digest.dump()}});
    body["tools"]       = nlohmann::json::parse(kToolsJsonSchema);
    body["tool_choice"] = "auto";
    body["temperature"] = 0.5;
    body["max_tokens"]  = 512;
    return body.dump();
}

}  // namespace LlmAgentTier2

#endif
