#ifndef _PLAYERBOT_LLMAGENT_HOOKS_H
#define _PLAYERBOT_LLMAGENT_HOOKS_H

#include <cstdint>
#include <string>

class Player;

namespace LlmAgentHooks {

void OnWhisperReceived(Player* bot, Player* sender, const std::string& text);
void OnKill(Player* bot, const std::string& victim_name);

}  // namespace LlmAgentHooks

#endif
