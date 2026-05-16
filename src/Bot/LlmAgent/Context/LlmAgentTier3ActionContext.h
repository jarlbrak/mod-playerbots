#ifndef _PLAYERBOT_LLMAGENT_TIER3_ACTION_CONTEXT_H
#define _PLAYERBOT_LLMAGENT_TIER3_ACTION_CONTEXT_H

#include "NamedObjectContext.h"
#include "Actions/LlmChatAction.h"

class LlmAgentTier3ActionContext : public NamedObjectContext<Action> {
  public:
    LlmAgentTier3ActionContext() {
        creators["llm chat"] = &LlmAgentTier3ActionContext::llm_chat;
    }
  private:
    static Action* llm_chat(PlayerbotAI* ai) { return new LlmChatAction(ai); }
};

#endif
