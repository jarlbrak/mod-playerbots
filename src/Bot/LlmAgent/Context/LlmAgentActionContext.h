#ifndef _PLAYERBOT_LLMAGENT_ACTION_CONTEXT_H
#define _PLAYERBOT_LLMAGENT_ACTION_CONTEXT_H

#include "NamedObjectContext.h"
#include "Actions/LlmReplanIdleAction.h"

class LlmAgentActionContext : public NamedObjectContext<Action> {
  public:
    LlmAgentActionContext() {
        creators["llm replan idle"] = &LlmAgentActionContext::llm_replan_idle;
    }
  private:
    static Action* llm_replan_idle(PlayerbotAI* ai) { return new LlmReplanIdleAction(ai); }
};

#endif
