#ifndef _PLAYERBOT_LLMAGENT_TOOL_VALIDATORS_H
#define _PLAYERBOT_LLMAGENT_TOOL_VALIDATORS_H

#include "Tools/InteractionContext.h"
#include "Tools/ToolCatalog.h"
#include "Validator/GoalValidator.h"   // for ValidationResult
#include <string>

ValidationResult Validate(const AcceptPartyInviteCall&, const InteractionContext&);
ValidationResult Validate(const LeavePartyCall&,         const InteractionContext&);
ValidationResult Validate(const AcceptQuestCall&,        const InteractionContext&);
ValidationResult Validate(const TurnInQuestCall&,        const InteractionContext&);
ValidationResult Validate(const SetGoalCall&,            const InteractionContext&);
ValidationResult Validate(const VendorJunkCall&,         const InteractionContext&);
ValidationResult Validate(const MemoryRememberCall&,     const InteractionContext&);

#endif
