#include "Validator/GoalValidator.h"

namespace {

const QuestLogContextEntry* find_quest(const BotValidationContext& ctx, uint32_t quest_id) {
    for (const auto& q : ctx.quest_log) {
        if (q.id == quest_id) return &q;
    }
    return nullptr;
}

ValidationResult check_grind_or_camp(const BotValidationContext& ctx, double x, double y, int32_t map_id) {
    if (static_cast<uint32_t>(map_id) != ctx.map_id) {
        return ValidationResult{false, "rejected_map_mismatch"};
    }
    if (x < ctx.map_min_x || x > ctx.map_max_x || y < ctx.map_min_y || y > ctx.map_max_y) {
        return ValidationResult{false, "rejected_position_out_of_bounds"};
    }
    return ValidationResult{true, ""};
}

}  // namespace

ValidationResult ValidateGoalDecision(const ParsedGoal& g, const BotValidationContext& ctx) {
    switch (g.goal) {
        case GoalKind::Idle:
        case GoalKind::WanderRandom:
        case GoalKind::Rest:
            return {true, ""};

        case GoalKind::DoQuest: {
            const auto& p = std::get<DoQuestParams>(g.params);
            const auto* q = find_quest(ctx, p.quest_id);
            if (!q)
                return {false, "rejected_quest_not_in_log"};
            if (q->status >= 2)
                return {false, "rejected_quest_already_complete"};
            if (p.starting_objective_idx < 0 || p.starting_objective_idx >= q->objective_count)
                return {false, "rejected_objective_idx_out_of_range"};
            return {true, ""};
        }

        case GoalKind::GoGrind: {
            const auto& p = std::get<GoGrindParams>(g.params);
            return check_grind_or_camp(ctx, p.x, p.y, p.map_id);
        }

        case GoalKind::GoCamp: {
            const auto& p = std::get<GoCampParams>(g.params);
            return check_grind_or_camp(ctx, p.x, p.y, p.map_id);
        }

        case GoalKind::WanderNpc: {
            const auto& p = std::get<WanderNpcParams>(g.params);
            if (ctx.nearby_creature_guids.empty()) return {true, ""};
            for (auto guid : ctx.nearby_creature_guids) {
                if (guid == p.npc_guid) return {true, ""};
            }
            return {false, "rejected_npc_not_nearby"};
        }

        case GoalKind::TravelFlight: {
            const auto& p = std::get<TravelFlightParams>(g.params);
            if (ctx.known_flight_node_ids.count(p.destination_node_id) == 0)
                return {false, "rejected_unknown_flight_node"};
            return {true, ""};
        }

        case GoalKind::OutdoorPvp: {
            const auto& p = std::get<OutdoorPvpParams>(g.params);
            if (ctx.valid_capture_point_spawn_ids.count(p.capture_point_spawn_id) == 0)
                return {false, "rejected_unknown_capture_point"};
            return {true, ""};
        }
    }
    return {false, "rejected_unhandled_goal_kind"};
}
