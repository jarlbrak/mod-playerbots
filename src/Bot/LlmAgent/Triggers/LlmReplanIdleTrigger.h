#ifndef _PLAYERBOT_LLM_REPLAN_IDLE_TRIGGER_H
#define _PLAYERBOT_LLM_REPLAN_IDLE_TRIGGER_H

#include "Trigger.h"

class LlmReplanIdleTrigger : public Trigger {
  public:
    LlmReplanIdleTrigger(PlayerbotAI* ai) : Trigger(ai, "llm replan idle") {}
    bool IsActive() override;
};

#endif
