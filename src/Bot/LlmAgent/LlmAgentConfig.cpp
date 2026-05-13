#include "LlmAgentConfig.h"

const char* const kDefaultSystemPrompt =
    "You are an in-world decision-maker for a World of Warcraft NPC. "
    "Given the attached state digest, choose what the character should do next. "
    "Respond with a single JSON object matching the provided schema. "
    "Pick a goal that is plausible for the character's level, location, and "
    "current objectives. Prefer continuing existing quests over starting new "
    "ones when progress is partial. Be concise in `reasoning`.";

LlmApplyMode ParseApplyMode(const std::string& s) {
    if (s == "apply")  return LlmApplyMode::Apply;
    if (s == "shadow") return LlmApplyMode::Shadow;
    return LlmApplyMode::Log;  // default + unknown
}
