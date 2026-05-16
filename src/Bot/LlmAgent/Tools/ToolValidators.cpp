#include "Tools/ToolValidators.h"
#include "Validator/GoalValidator.h"

namespace {

const NearbyCreature* find_nearby(const InteractionContext& ctx, const std::string& name) {
    for (const auto& c : ctx.nearby_creatures) {
        if (c.name == name) return &c;
    }
    return nullptr;
}

const QuestLogContextEntry* find_quest(const InteractionContext& ctx, uint32_t qid) {
    for (const auto& q : ctx.quest_log) {
        if (q.id == qid) return &q;
    }
    return nullptr;
}

}  // namespace

ValidationResult Validate(const AcceptPartyInviteCall& c, const InteractionContext& ctx) {
    if (ctx.pending_invites.empty()) return {false, "rejected_no_pending_invite"};
    for (const auto& inv : ctx.pending_invites) {
        if (inv.from_name == c.from) return {true, ""};
    }
    return {false, "rejected_invite_from_unknown"};
}

ValidationResult Validate(const LeavePartyCall&, const InteractionContext& ctx) {
    if (!ctx.in_group) return {false, "rejected_not_in_group"};
    return {true, ""};
}

ValidationResult Validate(const AcceptQuestCall& c, const InteractionContext& ctx) {
    const auto* npc = find_nearby(ctx, c.from_npc_name);
    if (!npc || !npc->in_range_10y) return {false, "rejected_npc_not_in_range"};
    if (npc->is_quest_giver_for != c.quest_id) return {false, "rejected_npc_not_quest_giver"};
    if (find_quest(ctx, c.quest_id) != nullptr) return {false, "rejected_quest_already_in_log"};
    return {true, ""};
}

ValidationResult Validate(const TurnInQuestCall& c, const InteractionContext& ctx) {
    const auto* q = find_quest(ctx, c.quest_id);
    if (!q) return {false, "rejected_quest_not_in_log"};
    if (q->status != 1) return {false, "rejected_quest_not_complete"};
    const auto* npc = find_nearby(ctx, c.to_npc_name);
    if (!npc || !npc->in_range_10y) return {false, "rejected_npc_not_in_range"};
    if (npc->is_turn_in_for != c.quest_id) return {false, "rejected_npc_not_turn_in_target"};
    return {true, ""};
}

ValidationResult Validate(const SetGoalCall& c, const InteractionContext& ctx) {
    BotValidationContext vctx;
    vctx.bot_level = ctx.bot_level;
    vctx.map_id = ctx.map_id;
    vctx.map_min_x = ctx.map_min_x; vctx.map_max_x = ctx.map_max_x;
    vctx.map_min_y = ctx.map_min_y; vctx.map_max_y = ctx.map_max_y;
    vctx.quest_log = ctx.quest_log;
    return ValidateGoalDecision(c.goal, vctx);
}

ValidationResult Validate(const VendorJunkCall& c, const InteractionContext& ctx) {
    const auto* npc = find_nearby(ctx, c.vendor_npc_name);
    if (!npc || !npc->in_range_10y) return {false, "rejected_npc_not_in_range"};
    if (!npc->is_vendor) return {false, "rejected_npc_not_vendor"};
    return {true, ""};
}

ValidationResult Validate(const MemoryRememberCall& c, const InteractionContext&) {
    if (c.text.size() > 500) return {false, "rejected_text_too_long"};
    if (c.salience < 0.0 || c.salience > 1.0) return {false, "rejected_salience_out_of_range"};
    return {true, ""};
}
