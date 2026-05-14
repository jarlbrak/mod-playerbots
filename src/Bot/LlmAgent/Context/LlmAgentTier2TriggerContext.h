#ifndef _PLAYERBOT_LLMAGENT_TIER2_TRIGGER_CONTEXT_H
#define _PLAYERBOT_LLMAGENT_TIER2_TRIGGER_CONTEXT_H

#include "NamedObjectContext.h"
#include "Triggers/LlmInteractTrigger.h"

class LlmAgentTier2TriggerContext : public NamedObjectContext<Trigger> {
  public:
    LlmAgentTier2TriggerContext() {
        creators["llm interact"] = &LlmAgentTier2TriggerContext::llm_interact;
    }
  private:
    static Trigger* llm_interact(PlayerbotAI* ai) { return new LlmInteractTrigger(ai); }
};

#endif
