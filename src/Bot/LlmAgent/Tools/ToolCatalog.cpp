#include "Tools/ToolCatalog.h"

namespace {

template <typename T>
bool try_get(const nlohmann::json& j, const char* key, T& out) {
    if (!j.contains(key)) return false;
    try { out = j.at(key).get<T>(); } catch (...) { return false; }
    return true;
}

std::variant<ParsedToolCall, std::string>
parse_one(const std::string& name, const nlohmann::json& args) {
    if (name == "accept_party_invite") {
        AcceptPartyInviteCall c;
        if (!try_get(args, "from", c.from)) return std::string{"missing 'from'"};
        return ParsedToolCall{c};
    }
    if (name == "leave_party") {
        return ParsedToolCall{LeavePartyCall{}};
    }
    if (name == "accept_quest") {
        AcceptQuestCall c;
        if (!try_get(args, "quest_id", c.quest_id))         return std::string{"missing 'quest_id'"};
        if (!try_get(args, "from_npc_name", c.from_npc_name))
            return std::string{"missing 'from_npc_name'"};
        return ParsedToolCall{c};
    }
    if (name == "turn_in_quest") {
        TurnInQuestCall c;
        if (!try_get(args, "quest_id", c.quest_id))    return std::string{"missing 'quest_id'"};
        if (!try_get(args, "to_npc_name", c.to_npc_name))
            return std::string{"missing 'to_npc_name'"};
        return ParsedToolCall{c};
    }
    if (name == "set_goal") {
        SetGoalCall c;
        auto parsed = ParseAndValidate(args.dump());
        if (!std::holds_alternative<ParsedGoal>(parsed))
            return std::string{"set_goal arguments did not parse as a valid goal"};
        c.goal = std::get<ParsedGoal>(parsed);
        return ParsedToolCall{c};
    }
    if (name == "vendor_junk") {
        VendorJunkCall c;
        if (!try_get(args, "vendor_npc_name", c.vendor_npc_name))
            return std::string{"missing 'vendor_npc_name'"};
        return ParsedToolCall{c};
    }
    if (name == "memory.remember") {
        MemoryRememberCall c;
        if (!try_get(args, "text", c.text))         return std::string{"missing 'text'"};
        if (!try_get(args, "salience", c.salience)) return std::string{"missing 'salience'"};
        if (args.contains("entities")) {
            for (const auto& e : args["entities"]) c.entities.push_back(e.get<std::string>());
        }
        if (args.contains("relations")) {
            for (const auto& r : args["relations"]) {
                c.relations.emplace_back(
                    r.value("src", std::string{}),
                    r.value("rel", std::string{}),
                    r.value("dst", std::string{})
                );
            }
        }
        return ParsedToolCall{c};
    }
    return std::string{"unknown tool name: " + name};
}

}  // namespace

const char* const kToolsJsonSchema = R"([
  {"type":"function","function":{
    "name":"accept_party_invite",
    "description":"Accept a pending party invite from a real player.",
    "parameters":{"type":"object","required":["from"],"additionalProperties":false,
      "properties":{"from":{"type":"string","description":"Inviter's character name."}}}}},
  {"type":"function","function":{
    "name":"leave_party",
    "description":"Leave the current party/group.",
    "parameters":{"type":"object","required":[],"additionalProperties":false,"properties":{}}}},
  {"type":"function","function":{
    "name":"accept_quest",
    "description":"Accept a quest from a named NPC. NPC must be in range.",
    "parameters":{"type":"object","required":["quest_id","from_npc_name"],"additionalProperties":false,
      "properties":{
        "quest_id":{"type":"integer","minimum":1},
        "from_npc_name":{"type":"string"}}}}},
  {"type":"function","function":{
    "name":"turn_in_quest",
    "description":"Turn in a completed quest at a named NPC.",
    "parameters":{"type":"object","required":["quest_id","to_npc_name"],"additionalProperties":false,
      "properties":{
        "quest_id":{"type":"integer","minimum":1},
        "to_npc_name":{"type":"string"}}}}},
  {"type":"function","function":{
    "name":"set_goal",
    "description":"Change the bot's high-level goal (rest, do_quest, go_grind, etc).",
    "parameters":{"type":"object","required":["goal","params","reasoning","ttl_minutes"],
      "additionalProperties":false,
      "properties":{
        "goal":{"type":"string","enum":["idle","go_grind","go_camp","wander_npc","wander_random","do_quest","travel_flight","rest","outdoor_pvp"]},
        "params":{"type":"object"},
        "reasoning":{"type":"string","maxLength":500},
        "ttl_minutes":{"type":"integer","minimum":1,"maximum":1440}}}}},
  {"type":"function","function":{
    "name":"vendor_junk",
    "description":"Sell low-value items at a named vendor NPC.",
    "parameters":{"type":"object","required":["vendor_npc_name"],"additionalProperties":false,
      "properties":{"vendor_npc_name":{"type":"string"}}}}},
  {"type":"function","function":{
    "name":"memory.remember",
    "description":"Write a structured memory for later recall. Tag entities + relations.",
    "parameters":{"type":"object","required":["text","entities","salience"],"additionalProperties":false,
      "properties":{
        "text":{"type":"string","maxLength":500},
        "entities":{"type":"array","items":{"type":"string"}},
        "salience":{"type":"number","minimum":0,"maximum":1},
        "relations":{"type":"array","items":{"type":"object",
          "required":["src","rel","dst"],"additionalProperties":false,
          "properties":{"src":{"type":"string"},"rel":{"type":"string"},"dst":{"type":"string"}}}}}}}}
])";

// Compact schema for Tier-2 (social-only): accept_party_invite, leave_party,
// set_goal, memory.remember.  Omits accept_quest, turn_in_quest, vendor_junk
// to keep prompt under the llama-server per-slot context window.
const char* const kT2ToolsJsonSchema = R"([
  {"type":"function","function":{
    "name":"accept_party_invite",
    "description":"Accept a pending party invite.",
    "parameters":{"type":"object","required":["from"],
      "properties":{"from":{"type":"string"}}}}},
  {"type":"function","function":{
    "name":"leave_party",
    "description":"Leave the current party.",
    "parameters":{"type":"object","required":[],"properties":{}}}},
  {"type":"function","function":{
    "name":"set_goal",
    "description":"Change bot goal.",
    "parameters":{"type":"object","required":["goal","params","reasoning","ttl_minutes"],
      "properties":{
        "goal":{"type":"string","enum":["idle","go_grind","go_camp","wander_npc","wander_random","do_quest","travel_flight","rest","outdoor_pvp"]},
        "params":{"type":"object"},
        "reasoning":{"type":"string"},
        "ttl_minutes":{"type":"integer","minimum":1,"maximum":1440}}}}},
  {"type":"function","function":{
    "name":"memory.remember",
    "description":"Store a memory.",
    "parameters":{"type":"object","required":["text","entities","salience"],
      "properties":{
        "text":{"type":"string"},
        "entities":{"type":"array","items":{"type":"string"}},
        "salience":{"type":"number"},
        "relations":{"type":"array","items":{"type":"object",
          "required":["src","rel","dst"],
          "properties":{"src":{"type":"string"},"rel":{"type":"string"},"dst":{"type":"string"}}}}}}}}
])";

#define LLM_TOOL_CALL_ONEOF_BODY \
    "{\"type\":\"object\",\"required\":[\"name\",\"arguments\"],\"additionalProperties\":false," \
    "\"properties\":{" \
      "\"name\":{\"const\":\"accept_party_invite\"}," \
      "\"arguments\":{\"type\":\"object\",\"required\":[\"from\"],\"additionalProperties\":false," \
        "\"properties\":{\"from\":{\"type\":\"string\"}}}}}," \
    "{\"type\":\"object\",\"required\":[\"name\",\"arguments\"],\"additionalProperties\":false," \
    "\"properties\":{" \
      "\"name\":{\"const\":\"leave_party\"}," \
      "\"arguments\":{\"type\":\"object\",\"additionalProperties\":false}}}," \
    "{\"type\":\"object\",\"required\":[\"name\",\"arguments\"],\"additionalProperties\":false," \
    "\"properties\":{" \
      "\"name\":{\"const\":\"set_goal\"}," \
      "\"arguments\":{\"type\":\"object\"," \
        "\"required\":[\"goal\",\"params\",\"reasoning\",\"ttl_minutes\"]," \
        "\"additionalProperties\":false," \
        "\"properties\":{" \
          "\"goal\":{\"type\":\"string\",\"enum\":[\"idle\",\"rest\",\"go_grind\",\"wander_npc\",\"wander_random\",\"do_quest\"]}," \
          "\"params\":{\"type\":\"object\"}," \
          "\"reasoning\":{\"type\":\"string\",\"maxLength\":200}," \
          "\"ttl_minutes\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":1440}}}}}," \
    "{\"type\":\"object\",\"required\":[\"name\",\"arguments\"],\"additionalProperties\":false," \
    "\"properties\":{" \
      "\"name\":{\"const\":\"memory.remember\"}," \
      "\"arguments\":{\"type\":\"object\"," \
        "\"required\":[\"text\",\"entities\",\"salience\"]," \
        "\"additionalProperties\":false," \
        "\"properties\":{" \
          "\"text\":{\"type\":\"string\",\"maxLength\":300}," \
          "\"entities\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}," \
          "\"salience\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1}}}}}"

const char* const kToolCallOneOf = LLM_TOOL_CALL_ONEOF_BODY;

const char* const kT2ToolCallOutputSchema =
    "{\"type\":\"array\",\"minItems\":1,\"maxItems\":1,"
    "\"items\":{\"oneOf\":[" LLM_TOOL_CALL_ONEOF_BODY "]}}";

const char* const kT3OutputSchema =
    "{\"type\":\"object\","
    "\"required\":[\"utterance\",\"side_effects\"],"
    "\"additionalProperties\":false,"
    "\"properties\":{"
      "\"utterance\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":200},"
      "\"side_effects\":{\"type\":\"array\",\"maxItems\":3,"
        "\"items\":{\"oneOf\":[" LLM_TOOL_CALL_ONEOF_BODY "]}}}"
    "}";

#undef LLM_TOOL_CALL_ONEOF_BODY

std::variant<std::vector<ParsedToolCall>, ParseError>
ParseToolCalls(const std::string& raw_json) {
    nlohmann::json j;
    try { j = nlohmann::json::parse(raw_json); }
    catch (const std::exception& e) { return ParseError{std::string{"top-level parse: "} + e.what()}; }
    if (!j.is_array()) return ParseError{"top-level not an array"};

    std::vector<ParsedToolCall> out;
    for (const auto& entry : j) {
        std::string name;
        if (!try_get(entry, "name", name)) return ParseError{"tool call missing 'name'"};

        nlohmann::json args = nlohmann::json::object();
        if (entry.contains("arguments")) {
            if (entry["arguments"].is_string()) {
                try { args = nlohmann::json::parse(entry["arguments"].get<std::string>()); }
                catch (...) { return ParseError{"failed to parse 'arguments' as JSON for " + name}; }
            } else if (entry["arguments"].is_object()) {
                args = entry["arguments"];
            }
        }

        auto one = parse_one(name, args);
        if (std::holds_alternative<std::string>(one))
            return ParseError{std::get<std::string>(one)};
        out.push_back(std::get<ParsedToolCall>(one));
    }
    return out;
}
