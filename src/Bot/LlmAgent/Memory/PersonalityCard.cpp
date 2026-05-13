#include "Memory/PersonalityCard.h"

#include <sstream>

namespace LlmAgentPersonality {

std::string StubPersonaText(const LlmBotState& s) {
    std::ostringstream out;
    out << s.self.race << " " << s.self.character_class
        << ", level " << s.self.level
        << ", currently in " << s.location.zone << ".";
    return out.str();
}

}  // namespace LlmAgentPersonality
