#ifndef _PLAYERBOT_LLMAGENT_TIER2_ACTION_CONTEXT_H
#define _PLAYERBOT_LLMAGENT_TIER2_ACTION_CONTEXT_H

#include "NamedObjectContext.h"
#include "Actions/LlmInteractAction.h"

class LlmAgentTier2ActionContext : public NamedObjectContext<Action> {
  public:
    LlmAgentTier2ActionContext() {
        creators["llm interact"] = &LlmAgentTier2ActionContext::llm_interact;
    }
  private:
    static Action* llm_interact(PlayerbotAI* ai) { return new LlmInteractAction(ai); }
};

#endif
