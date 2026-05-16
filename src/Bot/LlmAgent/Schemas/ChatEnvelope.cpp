#include "Schemas/ChatEnvelope.h"
#include "Vendor/nlohmann_json.hpp"

std::variant<ParsedChatEnvelope, ParseError>
ParseChatEnvelope(const std::string& raw_json) {
    nlohmann::json j;
    try { j = nlohmann::json::parse(raw_json); }
    catch (const std::exception& e) {
        return ParseError{std::string{"top-level parse: "} + e.what()};
    }
    if (!j.is_object()) return ParseError{"envelope: top-level value is not an object"};

    if (!j.contains("utterance"))       return ParseError{"envelope: missing 'utterance'"};
    if (!j["utterance"].is_string())    return ParseError{"envelope: 'utterance' must be a string"};
    if (!j.contains("side_effects"))    return ParseError{"envelope: missing 'side_effects'"};
    if (!j["side_effects"].is_array())  return ParseError{"envelope: 'side_effects' must be an array"};

    ParsedChatEnvelope out;
    out.utterance = j["utterance"].get<std::string>();

    // Reuse Phase 4's ParseToolCalls by re-serializing the side_effects array —
    // single source of truth for per-tool inner-shape validation.
    std::string sfx_raw = j["side_effects"].dump();
    auto parsed = ParseToolCalls(sfx_raw);
    if (std::holds_alternative<ParseError>(parsed))
        return ParseError{std::string{"side_effects: "} + std::get<ParseError>(parsed).message};
    out.side_effects = std::move(std::get<std::vector<ParsedToolCall>>(parsed));
    return out;
}
