#ifndef _PLAYERBOT_LLMAGENT_TIER3_CHATBRAIN_H
#define _PLAYERBOT_LLMAGENT_TIER3_CHATBRAIN_H

#include "Vendor/nlohmann_json.hpp"
#include <cstdint>
#include <string>

#ifndef LLMAGENT_UNIT_TESTS
class PlayerbotAI;

namespace LlmAgentTier3 {

enum class EventKind : uint8_t { Whisper = 0, Invite = 1, Join = 2 };

struct ChatContext {
    EventKind   kind;
    std::string sender_name;
    uint64_t    sender_guid = 0;
    std::string sender_message;     // populated only for Whisper
};

// Returns the JSON user-message payload for a T3 LLM call.
nlohmann::json BuildT3Digest(PlayerbotAI* botAI, const ChatContext& ctx);

// Returns the OpenAI-shaped POST body (model, messages, response_format, ...).
std::string BuildT3RequestBody(PlayerbotAI* botAI, const ChatContext& ctx);

}  // namespace LlmAgentTier3
#endif  // LLMAGENT_UNIT_TESTS

#endif  // _PLAYERBOT_LLMAGENT_TIER3_CHATBRAIN_H
