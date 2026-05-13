#ifndef _PLAYERBOT_LLMAGENT_APPLY_GOAL_H
#define _PLAYERBOT_LLMAGENT_APPLY_GOAL_H

#include "Schemas/Goal.h"

class PlayerbotAI;

namespace LlmAgentApply {

// Returns true on successful apply, false on exception (and resets the bot
// to Idle so it never stays in a corrupt state).
bool ApplyGoalToRpgInfo(const ParsedGoal& g, PlayerbotAI* botAI);

}  // namespace LlmAgentApply

#endif
