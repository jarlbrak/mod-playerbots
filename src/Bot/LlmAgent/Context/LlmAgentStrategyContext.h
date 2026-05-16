#ifndef _PLAYERBOT_LLMAGENT_STRATEGY_CONTEXT_H
#define _PLAYERBOT_LLMAGENT_STRATEGY_CONTEXT_H

#include "NamedObjectContext.h"
#include "Strategy/LlmAgentStrategy.h"

class LlmAgentStrategyContext : public NamedObjectContext<Strategy> {
  public:
    LlmAgentStrategyContext() {
        creators["llm agent"] = &LlmAgentStrategyContext::llm_agent;
    }
  private:
    static Strategy* llm_agent(PlayerbotAI* ai) { return new LlmAgentStrategy(ai); }
};

#endif
