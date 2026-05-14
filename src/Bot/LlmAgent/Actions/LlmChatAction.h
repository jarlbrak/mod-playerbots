#ifndef _PLAYERBOT_LLM_CHAT_ACTION_H
#define _PLAYERBOT_LLM_CHAT_ACTION_H

#include "Action.h"

class LlmChatAction : public Action {
  public:
    LlmChatAction(PlayerbotAI* ai) : Action(ai, "llm chat") {}
    bool Execute(Event event) override;
};

#endif
