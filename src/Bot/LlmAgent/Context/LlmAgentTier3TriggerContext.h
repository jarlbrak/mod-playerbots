#ifndef _PLAYERBOT_LLMAGENT_TIER3_TRIGGER_CONTEXT_H
#define _PLAYERBOT_LLMAGENT_TIER3_TRIGGER_CONTEXT_H

#include "NamedObjectContext.h"
#include "Triggers/LlmChatTrigger.h"

class LlmAgentTier3TriggerContext : public NamedObjectContext<Trigger> {
  public:
    LlmAgentTier3TriggerContext() {
        creators["llm chat"] = &LlmAgentTier3TriggerContext::llm_chat;
    }
  private:
    static Trigger* llm_chat(PlayerbotAI* ai) { return new LlmChatTrigger(ai); }
};

#endif
