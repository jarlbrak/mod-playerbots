#ifndef _PLAYERBOT_LLMAGENT_STRATEGY_H
#define _PLAYERBOT_LLMAGENT_STRATEGY_H

#include "Strategy.h"

class LlmAgentStrategy : public Strategy {
  public:
    LlmAgentStrategy(PlayerbotAI* ai) : Strategy(ai) {}
    std::string const getName() override { return "llm agent"; }
    std::vector<NextAction> getDefaultActions() override { return {}; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    void InitMultipliers(std::vector<Multiplier*>&) override {}
};

#endif
