#ifndef _PLAYERBOT_LLMAGENT_TOOL_CATALOG_H
#define _PLAYERBOT_LLMAGENT_TOOL_CATALOG_H

#include "Schemas/Goal.h"   // for ParsedGoal (used by SetGoalCall)
#include "Vendor/nlohmann_json.hpp"
#include <cstdint>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

struct AcceptPartyInviteCall { std::string from; };
struct LeavePartyCall        { };
struct AcceptQuestCall       { uint32_t quest_id = 0; std::string from_npc_name; };
struct TurnInQuestCall       { uint32_t quest_id = 0; std::string to_npc_name; };
struct SetGoalCall           { ParsedGoal goal; };
struct VendorJunkCall        { std::string vendor_npc_name; };
struct MemoryRememberCall {
    std::string text;
    std::vector<std::string> entities;
    double salience = 0.0;
    std::vector<std::tuple<std::string, std::string, std::string>> relations;
};

using ParsedToolCall = std::variant<
    AcceptPartyInviteCall, LeavePartyCall, AcceptQuestCall,
    TurnInQuestCall, SetGoalCall, VendorJunkCall, MemoryRememberCall
>;

extern const char* const kToolsJsonSchema;

// Compact subset for Tier-2 social interactions (fits within a 1024-token slot).
// Omits quest/vendor tools; keeps accept_party_invite, leave_party, set_goal, memory.remember.
extern const char* const kT2ToolsJsonSchema;

// JSON Schema constraining the model's *output* to a tool-call array
// `[{"name": <enum>, "arguments": <object>}, ...]`. Used with response_format
// instead of tool_choice="auto" because Qwen 2.5 7B doesn't reliably emit
// OpenAI-format tool_calls from a tools[] descriptor — it returns conversational
// text. Constrained generation with a JSON schema forces the right shape.
extern const char* const kT2ToolCallOutputSchema;

std::variant<std::vector<ParsedToolCall>, ParseError>
ParseToolCalls(const std::string& raw_json);

#endif  // _PLAYERBOT_LLMAGENT_TOOL_CATALOG_H
