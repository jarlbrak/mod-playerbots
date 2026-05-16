#include "Actions/LlmReplanIdleAction.h"
#include "LlmAgentManager.h"
#include "Tiers/Tier0_StateDigest.h"
#include "Schemas/Goal.h"
#include "Validator/GoalValidator.h"
#include "Validator/ValidationContext.h"
#include "Apply/ApplyGoal.h"
#include "PlayerbotAI.h"
#include "Log.h"
#include "NewRpgInfo.h"

#include <chrono>

namespace {
GoalKind kind_from_json_str(const std::string& s) {
    if (s == "idle")          return GoalKind::Idle;
    if (s == "go_grind")      return GoalKind::GoGrind;
    if (s == "go_camp")       return GoalKind::GoCamp;
    if (s == "wander_npc")    return GoalKind::WanderNpc;
    if (s == "wander_random") return GoalKind::WanderRandom;
    if (s == "do_quest")      return GoalKind::DoQuest;
    if (s == "travel_flight") return GoalKind::TravelFlight;
    if (s == "rest")          return GoalKind::Rest;
    if (s == "outdoor_pvp")   return GoalKind::OutdoorPvp;
    return GoalKind::Idle;
}
}  // namespace

bool LlmReplanIdleAction::Execute(Event /*event*/) {
    if (!botAI) return false;
    auto& mgr = LlmAgentManager::Instance();
    if (!mgr.Enabled()) return false;
    Player* bot = botAI->GetBot();
    if (!bot) return false;
    const uint64_t guid = bot->GetGUID().GetRawValue();
    const LlmAgentConfig& cfg = mgr.Config();

    bool applied_this_tick = false;

    // 1. Drain results — validate, apply / shadow / log per ApplyMode.
    auto results = mgr.DrainResults(guid);
    for (const auto& r : results) {
        if (r.parsed_status != "ok") {
            mgr.Counters().IncFallbackUsed();
            mgr.Cooldowns().Set(
                guid,
                std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(cfg.FallbackCooldownMs));
            continue;
        }

        // Re-parse the raw_response back into a typed ParsedGoal.
        ParsedGoal goal;
        auto parsed = ParseAndValidate(r.raw_response);
        if (std::holds_alternative<ParsedGoal>(parsed)) {
            goal = std::get<ParsedGoal>(parsed);
        } else {
            mgr.Counters().IncFallbackUsed();
            mgr.Cooldowns().Set(
                guid,
                std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(cfg.FallbackCooldownMs));
            continue;
        }

        BotValidationContext vctx = SnapshotForValidation(botAI);
        ValidationResult vr = ValidateGoalDecision(goal, vctx);

        if (vr.accepted) mgr.Counters().IncValidatorAccepted();
        else             mgr.Counters().IncValidatorRejected(vr.reject_reason);

        if (vr.accepted && cfg.ApplyMode == LlmApplyMode::Apply) {
            bool ok = LlmAgentApply::ApplyGoalToRpgInfo(goal, botAI);
            if (ok) {
                mgr.Counters().IncApplied();
                applied_this_tick = true;
                auto cool = std::chrono::steady_clock::now() +
                            std::chrono::minutes(
                                std::min<uint32_t>(goal.ttl_minutes,
                                                   cfg.MaxCooldownMinutes));
                mgr.Cooldowns().Set(guid, cool);
            } else {
                mgr.Counters().IncAppliedThrew();
                mgr.Cooldowns().Set(
                    guid,
                    std::chrono::steady_clock::now() +
                      std::chrono::milliseconds(cfg.FallbackCooldownMs));
            }
        } else if (vr.accepted && cfg.ApplyMode == LlmApplyMode::Shadow) {
            mgr.Counters().IncShadowAccepted();
            LOG_INFO("playerbots",
                     "[LlmAgent shadow] bot={} would_have_applied={} lat={}ms",
                     r.bot_name,
                     r.parsed_goal.value("goal", std::string{"?"}),
                     r.inference_ms);
            auto cool = std::chrono::steady_clock::now() +
                        std::chrono::minutes(
                            std::min<uint32_t>(goal.ttl_minutes,
                                               cfg.MaxCooldownMinutes));
            mgr.Cooldowns().Set(guid, cool);
        } else if (vr.accepted && cfg.ApplyMode == LlmApplyMode::Log) {
            mgr.Counters().IncLogAcceptedSkipped();
            mgr.Cooldowns().Set(
                guid,
                std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(cfg.FallbackCooldownMs));
        } else {
            mgr.Counters().IncFallbackUsed();
            mgr.Cooldowns().Set(
                guid,
                std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(cfg.FallbackCooldownMs));
        }
    }

    // 2. Enqueue if eligible AND we didn't already apply this tick.
    if (!applied_this_tick &&
        mgr.Selector().IsLlmBot(guid) &&
        mgr.Cooldowns().Eligible(guid) &&
        botAI->rpgInfo.GetStatus() == RPG_IDLE &&
        !mgr.IsInFlight(guid))
    {
        LlmBotState state;
        try {
            state = SnapshotBot(botAI);
        } catch (...) {
            LOG_WARN("playerbots", "[LlmAgent] SnapshotBot threw; skipping enqueue");
            return applied_this_tick;
        }
        nlohmann::json digest = BuildDigestJson(state);

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
    }

    return applied_this_tick;  // true suppresses status_update for this tick
}
