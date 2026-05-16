#ifndef _PLAYERBOT_LLM_CHAT_TRIGGER_H
#define _PLAYERBOT_LLM_CHAT_TRIGGER_H

#include "Trigger.h"

class LlmChatTrigger : public Trigger {
  public:
    LlmChatTrigger(PlayerbotAI* ai) : Trigger(ai, "llm chat") {}
    bool IsActive() override;
};

#endif
