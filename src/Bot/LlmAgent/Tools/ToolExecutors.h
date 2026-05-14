#ifndef _PLAYERBOT_LLMAGENT_TOOL_EXECUTORS_H
#define _PLAYERBOT_LLMAGENT_TOOL_EXECUTORS_H

#include "Tools/ToolCatalog.h"

class PlayerbotAI;

namespace LlmAgentTools {

// Each returns true on successful apply, false on exception or precondition failure.
bool ApplyAcceptPartyInvite(const AcceptPartyInviteCall&, PlayerbotAI*);
bool ApplyLeaveParty       (const LeavePartyCall&,         PlayerbotAI*);
bool ApplyAcceptQuest      (const AcceptQuestCall&,        PlayerbotAI*);
bool ApplyTurnInQuest      (const TurnInQuestCall&,        PlayerbotAI*);
bool ApplySetGoal          (const SetGoalCall&,            PlayerbotAI*);
bool ApplyVendorJunk       (const VendorJunkCall&,         PlayerbotAI*);
bool ApplyMemoryRemember   (const MemoryRememberCall&,     PlayerbotAI*);

bool ApplyToolCall(const ParsedToolCall& call, PlayerbotAI* botAI);

}  // namespace LlmAgentTools

#endif
