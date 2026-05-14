#ifndef _PLAYERBOT_LLMAGENT_TIER2_INTERACTIVE_H
#define _PLAYERBOT_LLMAGENT_TIER2_INTERACTIVE_H

#include "Vendor/nlohmann_json.hpp"

#ifndef LLMAGENT_UNIT_TESTS
class PlayerbotAI;

namespace LlmAgentTier2 {

// Returns the user-message JSON for a T2 LLM call.
nlohmann::json BuildT2Digest(PlayerbotAI* botAI);

// Returns the OpenAI-shaped request body (model, messages, tools, tool_choice, temp, max_tokens).
std::string BuildT2RequestBody(PlayerbotAI* botAI);

}  // namespace LlmAgentTier2
#endif

#endif
