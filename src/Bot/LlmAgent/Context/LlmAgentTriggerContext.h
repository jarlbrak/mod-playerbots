#ifndef _PLAYERBOT_LLMAGENT_TRIGGER_CONTEXT_H
#define _PLAYERBOT_LLMAGENT_TRIGGER_CONTEXT_H

#include "NamedObjectContext.h"
#include "Triggers/LlmReplanIdleTrigger.h"

class LlmAgentTriggerContext : public NamedObjectContext<Trigger> {
  public:
    LlmAgentTriggerContext() {
        creators["llm replan idle"] = &LlmAgentTriggerContext::llm_replan_idle;
    }
  private:
    static Trigger* llm_replan_idle(PlayerbotAI* ai) { return new LlmReplanIdleTrigger(ai); }
};

#endif
