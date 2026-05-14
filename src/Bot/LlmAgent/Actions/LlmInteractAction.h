#ifndef _PLAYERBOT_LLM_INTERACT_ACTION_H
#define _PLAYERBOT_LLM_INTERACT_ACTION_H

#include "Action.h"

class LlmInteractAction : public Action {
  public:
    LlmInteractAction(PlayerbotAI* ai) : Action(ai, "llm interact") {}
    bool Execute(Event event) override;
};

#endif
