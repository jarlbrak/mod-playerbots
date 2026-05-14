#include "Actions/LlmInteractAction.h"
#include "LlmAgentManager.h"
#include "Tiers/Tier2_Interactive.h"
#include "Tools/ToolCatalog.h"
#include "Tools/ToolValidators.h"
#include "Tools/ToolExecutors.h"
#include "Tools/InteractionContext.h"
#include "PlayerbotAI.h"
#include "Player.h"
#include "Log.h"

#include <algorithm>
#include <chrono>
#include <type_traits>
#include <variant>

namespace {

const char* tool_call_name(const ParsedToolCall& c) {
    return std::visit([](auto const& t) -> const char* {
        using T = std::decay_t<decltype(t)>;
        if constexpr (std::is_same_v<T, AcceptPartyInviteCall>) return "accept_party_invite";
        else if constexpr (std::is_same_v<T, LeavePartyCall>)   return "leave_party";
        else if constexpr (std::is_same_v<T, AcceptQuestCall>)  return "accept_quest";
        else if constexpr (std::is_same_v<T, TurnInQuestCall>)  return "turn_in_quest";
        else if constexpr (std::is_same_v<T, SetGoalCall>)      return "set_goal";
        else if constexpr (std::is_same_v<T, VendorJunkCall>)   return "vendor_junk";
        else if constexpr (std::is_same_v<T, MemoryRememberCall>) return "memory.remember";
        else return "unknown";
    }, c);
}

}  // namespace

bool LlmInteractAction::Execute(Event /*event*/) {
    if (!botAI) return false;
    auto& mgr = LlmAgentManager::Instance();
    const auto& cfg = mgr.Config();
    if (!mgr.Enabled() || !cfg.Tier2_Enabled) return false;
    Player* bot = botAI->GetBot();
    if (!bot) return false;
    const uint64_t guid = bot->GetGUID().GetRawValue();

    bool applied_any = false;

    auto results = mgr.DrainResults(guid, /*tier=*/2);
    for (const auto& r : results) {
        if (r.parsed_status != "ok") {
            mgr.Counters().IncFallbackUsed();
            mgr.Cooldowns().Set(guid,
                std::chrono::steady_clock::now() +
                std::chrono::milliseconds(cfg.FallbackCooldownMs));
            continue;
        }
        auto parsed = ParseToolCalls(r.raw_response);
        if (std::holds_alternative<ParseError>(parsed)) {
            mgr.Counters().IncToolSchemaError();
            mgr.Cooldowns().Set(guid,
                std::chrono::steady_clock::now() +
                std::chrono::milliseconds(cfg.FallbackCooldownMs));
            continue;
        }
        const auto& calls = std::get<std::vector<ParsedToolCall>>(parsed);
        if (calls.empty()) {
            mgr.Counters().IncToolNoAction();
            mgr.Cooldowns().Set(guid,
                std::chrono::steady_clock::now() +
                std::chrono::milliseconds(cfg.FallbackCooldownMs));
            continue;
        }
        uint32_t truncate_at = std::min<uint32_t>(
            cfg.Tier2_MaxToolsPerResponse, static_cast<uint32_t>(calls.size()));
        if (calls.size() > cfg.Tier2_MaxToolsPerResponse)
            mgr.Counters().IncToolTruncated();

        InteractionContext ctx = SnapshotInteractionContext(botAI);
        for (uint32_t i = 0; i < truncate_at; ++i) {
            const auto& call = calls[i];
            mgr.Counters().IncToolReceived(tool_call_name(call));

            auto decision = std::visit([&](auto const& t) {
                return Validate(t, ctx);
            }, call);
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
        mgr.Interactions().Clear(guid);
        mgr.Cooldowns().Set(guid,
            std::chrono::steady_clock::now() +
            std::chrono::milliseconds(cfg.FallbackCooldownMs));
    }

    if (!applied_any &&
        mgr.Selector().IsLlmBot(guid) &&
        mgr.Cooldowns().Eligible(guid) &&
        !mgr.IsInFlight(guid, /*tier=*/2) &&
        mgr.Interactions().HasPending(guid))
    {
        LlmRequest req;
        req.bot_guid = guid;
        req.bot_name = bot->GetName();
        req.body_json = LlmAgentTier2::BuildT2RequestBody(botAI);
        req.digest_json = LlmAgentTier2::BuildT2Digest(botAI);
        req.tier = 2;
        mgr.Enqueue(std::move(req));
    }

    return applied_any;
}
