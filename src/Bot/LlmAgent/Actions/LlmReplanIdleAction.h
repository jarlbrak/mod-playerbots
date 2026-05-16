#ifndef _PLAYERBOT_LLM_REPLAN_IDLE_ACTION_H
#define _PLAYERBOT_LLM_REPLAN_IDLE_ACTION_H

#include "Action.h"

class LlmReplanIdleAction : public Action {
  public:
    LlmReplanIdleAction(PlayerbotAI* ai) : Action(ai, "llm replan idle") {}
    bool Execute(Event event) override;
};

#endif
