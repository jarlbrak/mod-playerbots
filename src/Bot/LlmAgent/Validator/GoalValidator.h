#ifndef _PLAYERBOT_LLMAGENT_GOAL_VALIDATOR_H
#define _PLAYERBOT_LLMAGENT_GOAL_VALIDATOR_H

#include "Schemas/Goal.h"
#include "Validator/ValidationContext.h"
#include <string>

struct ValidationResult {
    bool        accepted = false;
    std::string reject_reason;
};

ValidationResult ValidateGoalDecision(const ParsedGoal& g, const BotValidationContext& ctx);

#endif
