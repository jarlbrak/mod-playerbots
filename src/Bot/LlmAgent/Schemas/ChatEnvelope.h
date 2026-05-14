#ifndef _PLAYERBOT_LLMAGENT_CHAT_ENVELOPE_H
#define _PLAYERBOT_LLMAGENT_CHAT_ENVELOPE_H

#include "Schemas/Goal.h"          // for ParseError
#include "Tools/ToolCatalog.h"     // for ParsedToolCall
#include <string>
#include <variant>
#include <vector>

struct ParsedChatEnvelope {
    std::string                  utterance;
    std::vector<ParsedToolCall>  side_effects;
};

std::variant<ParsedChatEnvelope, ParseError>
ParseChatEnvelope(const std::string& raw_json);

#endif  // _PLAYERBOT_LLMAGENT_CHAT_ENVELOPE_H
