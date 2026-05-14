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

LlmApplyMode ParseApplyMode(const std::string& s) {
    if (s == "apply")  return LlmApplyMode::Apply;
    if (s == "shadow") return LlmApplyMode::Shadow;
    return LlmApplyMode::Log;  // default + unknown
}
