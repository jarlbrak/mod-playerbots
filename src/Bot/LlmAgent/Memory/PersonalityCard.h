#ifndef _PLAYERBOT_LLMAGENT_PERSONALITY_CARD_H
#define _PLAYERBOT_LLMAGENT_PERSONALITY_CARD_H

#include "Tiers/Tier0_StateDigest.h"
#include <string>

namespace LlmAgentPersonality {

// Phase 3 stub. Returns a one-line persona derived from race/class/level/zone.
// Phase 4 will replace this with an LLM-generated card on first encounter.
std::string StubPersonaText(const LlmBotState& s);

}  // namespace LlmAgentPersonality

#endif
