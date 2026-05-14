#include "Tiers/Tier3_ChatBrain.h"

#ifndef LLMAGENT_UNIT_TESTS

#include "Tiers/Tier0_StateDigest.h"
#include "Tools/ToolCatalog.h"
#include "LlmAgentManager.h"
#include "Chat/PersonaCache.h"
#include "Chat/WhisperBuffer.h"
#include "PlayerbotAI.h"
#include "Player.h"

#include <ctime>

namespace LlmAgentTier3 {

namespace {

const char* event_kind_str(EventKind k) {
    switch (k) {
        case EventKind::Whisper: return "whisper";
        case EventKind::Invite:  return "invite";
        case EventKind::Join:    return "join";
    }
    return "unknown";
}

}  // namespace

nlohmann::json BuildT3Digest(PlayerbotAI* botAI, const ChatContext& ctx) {
    LlmBotState state = SnapshotBot(botAI);
    nlohmann::json digest = BuildDigestJson(state);

    // Strip fields T3 doesn't need (mirrors Phase 4 T2 trim — keeps prompt small).
    digest.erase("quest_log");
    if (digest.contains("location") && digest["location"].contains("position"))
        digest["location"].erase("position");
    if (digest.contains("social") && digest["social"].contains("recent_whispers"))
        digest["social"].erase("recent_whispers");
    digest.erase("interaction_context");  // T2 field; T3 supplies its own

    digest["event_kind"]     = event_kind_str(ctx.kind);
    digest["sender_name"]    = ctx.sender_name;
    digest["sender_message"] = ctx.sender_message;

    auto& mgr = LlmAgentManager::Instance();
    const auto& cfg = mgr.Config();
    const uint64_t bot_guid = botAI->GetBot()->GetGUID().GetRawValue();
    const int64_t now = static_cast<int64_t>(time(nullptr));

    nlohmann::json history = nlohmann::json::array();
    auto entries = mgr.Whispers().SnapshotFor(
        bot_guid, ctx.sender_guid, now,
        cfg.Tier3_WhisperWindowSeconds, cfg.Tier3_DialogueHistorySize);
    for (const auto& e : entries) {
        history.push_back({
            {"direction", e.direction == WhisperEntry::Incoming ? "in" : "out"},
            {"text",      e.text},
            {"age_s",     now - e.ts}
        });
    }
    digest["dialogue_history"] = history;

    nlohmann::json mem_hints = nlohmann::json::array();
    if (!ctx.sender_name.empty()) {
        auto hints = mgr.MemoryClient().RecallAbout(
            bot_guid, ctx.sender_name, /*hops*/2, /*top_k*/3);
        for (const auto& h : hints) mem_hints.push_back(h);
    }
    digest["memory_about_sender"] = mem_hints;

    return digest;
}

std::string BuildT3RequestBody(PlayerbotAI* botAI, const ChatContext& ctx) {
    auto& mgr = LlmAgentManager::Instance();
    const auto& cfg = mgr.Config();
    const uint64_t bot_guid = botAI->GetBot()->GetGUID().GetRawValue();

    nlohmann::json digest = BuildT3Digest(botAI, ctx);

    std::string persona = mgr.Persona().Get(bot_guid);
    std::string suffix = cfg.Tier3_SystemPromptSuffix.empty()
        ? cfg.Tier3_BuiltInSystemPromptSuffix
        : cfg.Tier3_SystemPromptSuffix;
    std::string system_msg = persona.empty() ? suffix : (persona + "\n\n" + suffix);

    nlohmann::json body;
    body["model"]    = cfg.Model;
    body["messages"] = nlohmann::json::array();
    body["messages"].push_back({{"role", "system"}, {"content", system_msg}});
    body["messages"].push_back({{"role", "user"},   {"content", digest.dump()}});
    body["response_format"] = {
        {"type", "json_schema"},
        {"json_schema", {
            {"name", "chat_envelope"},
            {"schema", nlohmann::json::parse(kT3OutputSchema)}
        }}
    };
    body["temperature"] = 0.7;
    body["max_tokens"]  = 300;
    return body.dump();
}

}  // namespace LlmAgentTier3

#endif  // LLMAGENT_UNIT_TESTS
