#include "Actions/LlmChatAction.h"
#include "LlmAgentManager.h"
#include "Schemas/ChatEnvelope.h"
#include "Tools/ToolValidators.h"
#include "Tools/ToolExecutors.h"
#include "Tools/InteractionContext.h"
#include "Tiers/Tier3_ChatBrain.h"
#include "Mgr/Text/PlayerbotTextMgr.h"   // for ChatQueuedReply
#include "PlayerbotAI.h"
#include "Player.h"
#include "Group.h"
#include "ObjectAccessor.h"
#include "Log.h"
#include "SharedDefines.h"               // CHAT_MSG_WHISPER / CHAT_MSG_PARTY

#include <algorithm>
#include <chrono>
#include <ctime>
#include <type_traits>
#include <variant>

namespace {

using LlmAgentTier3::ChatContext;
using LlmAgentTier3::EventKind;

const char* event_kind_str(EventKind k) {
    switch (k) {
        case EventKind::Whisper: return "whisper";
        case EventKind::Invite:  return "invite";
        case EventKind::Join:    return "join";
    }
    return "unknown";
}

// Picks the highest-priority pending interaction for this bot. Priority:
// whisper > invite > join (whisper is the most direct human signal).
bool BuildChatContext(uint64_t bot_guid, ChatContext& out) {
    auto& mgr = LlmAgentManager::Instance();
    auto payload = mgr.Interactions().SnapshotFor(bot_guid);
    if (!payload.recent_whispers.empty()) {
        const auto& w = payload.recent_whispers.front();
        out.kind = EventKind::Whisper;
        out.sender_name = w.from_name;
        out.sender_guid = w.from_guid;
        out.sender_message = w.text;
        return true;
    }
    if (!payload.pending_invites.empty()) {
        const auto& i = payload.pending_invites.front();
        out.kind = EventKind::Invite;
        out.sender_name = i.from_name;
        out.sender_guid = i.from_guid;
        out.sender_message.clear();
        return true;
    }
    if (!payload.recent_group_joins.empty()) {
        const auto& j = payload.recent_group_joins.front();
        out.kind = EventKind::Join;
        out.sender_name = j.leader_name;
        out.sender_guid = j.leader_guid;
        out.sender_message.clear();
        return true;
    }
    return false;
}

const char* tool_call_name(const ParsedToolCall& c) {
    return std::visit([](auto const& t) -> const char* {
        using T = std::decay_t<decltype(t)>;
        if      constexpr (std::is_same_v<T, AcceptPartyInviteCall>) return "accept_party_invite";
        else if constexpr (std::is_same_v<T, LeavePartyCall>)        return "leave_party";
        else if constexpr (std::is_same_v<T, AcceptQuestCall>)       return "accept_quest";
        else if constexpr (std::is_same_v<T, TurnInQuestCall>)       return "turn_in_quest";
        else if constexpr (std::is_same_v<T, SetGoalCall>)           return "set_goal";
        else if constexpr (std::is_same_v<T, VendorJunkCall>)        return "vendor_junk";
        else if constexpr (std::is_same_v<T, MemoryRememberCall>)    return "memory.remember";
        else                                                          return "unknown";
    }, c);
}

void QueueUtterance(PlayerbotAI* botAI, const std::string& utterance,
                    const ChatContext& ctx) {
    uint32 chat_type = (ctx.kind == EventKind::Join)
        ? CHAT_MSG_PARTY
        : CHAT_MSG_WHISPER;
    uint32 guid1 = static_cast<uint32>(ctx.sender_guid & 0xFFFFFFFFULL);
    ChatQueuedReply reply(
        chat_type,
        guid1,
        /*guid2*/0,
        utterance,
        /*chanName*/std::string{},
        ctx.sender_name,
        static_cast<time_t>(time(nullptr)));
    botAI->QueueChatResponse(reply);
}

}  // namespace

bool LlmChatAction::Execute(Event /*event*/) {
    if (!botAI) return false;
    auto& mgr = LlmAgentManager::Instance();
    const auto& cfg = mgr.Config();
    if (!mgr.Enabled() || !cfg.Tier3_Enabled) return false;
    Player* bot = botAI->GetBot();
    if (!bot) return false;
    const uint64_t guid = bot->GetGUID().GetRawValue();

    bool applied_any = false;

    // ===== Drain phase =====
    auto results = mgr.DrainResults(guid, /*tier*/3);
    for (const auto& r : results) {
        if (r.parsed_status != "ok") {
            mgr.Counters().IncFallbackUsed();
            mgr.T3Cooldowns().Set(guid,
                std::chrono::steady_clock::now() +
                std::chrono::milliseconds(cfg.FallbackCooldownMs));
            continue;
        }
        auto parsed = ParseChatEnvelope(r.raw_response);
        if (std::holds_alternative<ParseError>(parsed)) {
            mgr.Counters().IncChatEnvelopeParsed("schema_error");
            mgr.T3Cooldowns().Set(guid,
                std::chrono::steady_clock::now() +
                std::chrono::milliseconds(cfg.FallbackCooldownMs));
            continue;
        }
        const auto& env = std::get<ParsedChatEnvelope>(parsed);
        mgr.Counters().IncChatEnvelopeParsed("ok");

        ChatContext ctx;
        if (!BuildChatContext(guid, ctx)) {
            mgr.Counters().IncChatSenderOffline();
            mgr.Interactions().Clear(guid);
            continue;
        }

        // Apply side_effects (Phase 4 path, unchanged).
        InteractionContext vctx = SnapshotInteractionContext(botAI);
        const uint32_t cap = static_cast<uint32_t>(env.side_effects.size());
        for (uint32_t i = 0; i < cap; ++i) {
            const auto& call = env.side_effects[i];
            mgr.Counters().IncToolReceived(tool_call_name(call));
            auto decision = std::visit([&](auto const& t){ return Validate(t, vctx); }, call);
            if (!decision.accepted) {
                mgr.Counters().IncToolRejected(decision.reject_reason);
                if (!std::holds_alternative<MemoryRememberCall>(call)) break;
                continue;
            }
            try {
                bool ok = LlmAgentTools::ApplyToolCall(call, botAI);
                if (ok) {
                    mgr.Counters().IncToolApplied(tool_call_name(call));
                    applied_any = true;
                } else {
                    mgr.Counters().IncToolThrew(tool_call_name(call));
                    break;
                }
            } catch (...) {
                mgr.Counters().IncToolThrew(tool_call_name(call));
                break;
            }
        }

        // Queue the utterance.
        if (!env.utterance.empty()) {
            try {
                QueueUtterance(botAI, env.utterance, ctx);
                mgr.Counters().IncChatUtterancesQueued();
                mgr.Whispers().Push(
                    guid, ctx.sender_guid,
                    WhisperEntry::Outgoing,
                    env.utterance,
                    static_cast<int64_t>(time(nullptr)));
            } catch (...) {
                LOG_ERROR("playerbots", "[LlmAgent] QueueUtterance threw for bot {}", guid);
            }
        }

        // Consume the interaction. Set a short nominal cooldown.
        mgr.Interactions().Clear(guid);
        mgr.T3Cooldowns().Set(guid,
            std::chrono::steady_clock::now() +
            std::chrono::milliseconds(cfg.Tier3_CooldownMs));
    }

    // ===== Enqueue phase =====
    if (!applied_any &&
        mgr.Selector().IsLlmBot(guid) &&
        mgr.T3Cooldowns().Eligible(guid) &&
        !mgr.IsInFlight(guid, /*tier*/3) &&
        mgr.Interactions().HasPending(guid))
    {
        ChatContext ctx;
        if (BuildChatContext(guid, ctx)) {
            mgr.Counters().IncChatEventKind(event_kind_str(ctx.kind));
            LlmRequest req;
            req.bot_guid    = guid;
            req.bot_name    = bot->GetName();
            req.body_json   = LlmAgentTier3::BuildT3RequestBody(botAI, ctx);
            req.digest_json = LlmAgentTier3::BuildT3Digest(botAI, ctx);
            req.tier        = 3;
            mgr.Enqueue(std::move(req));
        }
    }

    return applied_any;
}
