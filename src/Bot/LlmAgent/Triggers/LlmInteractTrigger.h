#ifndef _PLAYERBOT_LLM_INTERACT_TRIGGER_H
#define _PLAYERBOT_LLM_INTERACT_TRIGGER_H

#include "Trigger.h"

class LlmInteractTrigger : public Trigger {
  public:
    LlmInteractTrigger(PlayerbotAI* ai) : Trigger(ai, "llm interact") {}
    bool IsActive() override;
};

#endif
