#include "LlmAgentConfig.h"

const char* const kDefaultSystemPrompt =
    "You are an in-world decision-maker for a World of Warcraft NPC. "
    "Given the attached state digest, choose what the character should do next. "
    "Respond with a single JSON object matching the provided schema. "
    "Pick a goal that is plausible for the character's level, location, and "
    "current objectives. Prefer continuing existing quests over starting new "
    "ones when progress is partial. Be concise in `reasoning`.";

const char* const kDefaultTier2SystemPrompt =
    "You are an in-world decision-maker for a World of Warcraft NPC. A real "
    "human player has just engaged with you (whisper, party invite, or group "
    "join). Reply with ONLY a JSON array of tool calls — no prose, no markdown. "
    "Each element has \"name\" (one of: accept_party_invite, leave_party, "
    "set_goal, memory.remember) and \"arguments\" (an object). "
    "Prefer concrete, helpful actions. When no action fits, reply with []. "
    "Examples: "
    "[{\"name\":\"accept_party_invite\",\"arguments\":{\"from\":\"Bob\"}}] · "
    "[{\"name\":\"set_goal\",\"arguments\":{\"goal\":\"rest\",\"params\":{},\"reasoning\":\"low hp\",\"ttl_minutes\":10}}] · "
    "[{\"name\":\"memory.remember\",\"arguments\":{\"text\":\"Bob asked to group up\",\"entities\":[\"Bob\"],\"salience\":0.6}}]";

const char* const kDefaultTier3SystemPromptSuffix =
    "You are this character. A real human player has just engaged with you. "
    "Look at `event_kind` and `sender_name` in the user message — those tell "
    "you what happened.\n"
    "\n"
    "Reply with ONLY a JSON object: "
    "{\"utterance\":\"<in-character reply, <=200 chars>\", \"side_effects\":[<1-3 tool calls>]}.\n"
    "\n"
    "EVENT RULES (no exceptions — pick the matching pattern):\n"
    "• event_kind=invite  → side_effects MUST include {\"name\":\"accept_party_invite\","
    "\"arguments\":{\"from\":\"<sender_name>\"}}. Optionally add set_goal or memory.remember.\n"
    "• event_kind=join    → side_effects MUST include {\"name\":\"set_goal\","
    "\"arguments\":{\"goal\":\"rest\",\"params\":{},\"reasoning\":\"joined group\",\"ttl_minutes\":5}} "
    "or a more specific goal that fits the group's context. memory.remember is optional.\n"
    "• event_kind=whisper → side_effects MUST include at least one memory.remember capturing "
    "what the sender said. If you commit to an action in the utterance, also include the matching tool.\n"
    "\n"
    "Examples of correct full envelopes:\n"
    "{\"utterance\":\"On my way, Bob.\",\"side_effects\":["
    "{\"name\":\"accept_party_invite\",\"arguments\":{\"from\":\"Bob\"}}]}\n"
    "{\"utterance\":\"I'll remember that.\",\"side_effects\":["
    "{\"name\":\"memory.remember\",\"arguments\":{\"text\":\"Bob wants to group up\","
    "\"entities\":[\"Bob\"],\"salience\":0.5}}]}\n"
    "\n"
    "Never say one thing and emit no matching action.";

LlmApplyMode ParseApplyMode(const std::string& s) {
    if (s == "apply")  return LlmApplyMode::Apply;
    if (s == "shadow") return LlmApplyMode::Shadow;
    return LlmApplyMode::Log;  // default + unknown
}
