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

    // Trim fields not needed for social-interaction decisions to keep the
    // prompt under the llama-server per-slot context window (n_ctx / --parallel).
    digest.erase("quest_log");                           // ~50-150 tokens saved
    if (digest.contains("location") && digest["location"].contains("position"))
        digest["location"].erase("position");            // ~8 tokens saved
    if (digest.contains("social") && digest["social"].contains("recent_whispers"))
        digest["social"].erase("recent_whispers");       // duplicated in interaction_context

    nlohmann::json body;
    body["model"]    = cfg.Model;
    body["messages"] = nlohmann::json::array();
    body["messages"].push_back({{"role", "system"}, {"content", cfg.Tier2_SystemPrompt}});
    body["messages"].push_back({{"role", "user"},   {"content", digest.dump()}});
    // Constrain output to the tool-call array shape. Qwen 2.5 7B ignores
    // tools[]/tool_choice="auto" and returns plain text; response_format
    // forces a parseable JSON array of {name, arguments}.
    body["response_format"] = {
        {"type", "json_schema"},
        {"json_schema", {
            {"name", "tool_calls"},
            {"schema", nlohmann::json::parse(kT2ToolCallOutputSchema)}
        }}
    };
    body["temperature"] = 0.5;
    body["max_tokens"]  = 256;
    return body.dump();
}

}  // namespace LlmAgentTier2

#endif
