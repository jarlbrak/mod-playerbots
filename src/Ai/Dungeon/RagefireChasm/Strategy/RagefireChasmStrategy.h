#ifndef _PLAYERBOT_CLASSICDUNGEONRFCSTRATEGY_H
#define _PLAYERBOT_CLASSICDUNGEONRFCSTRATEGY_H

#include "Multiplier.h"
#include "AiObjectContext.h"
#include "Strategy.h"

class ClassicDungeonRFCStrategy : public Strategy
{
public:
    ClassicDungeonRFCStrategy(PlayerbotAI* ai) : Strategy(ai) {}
    virtual std::string const getName() override { return "classic-rfc"; }
    virtual void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    virtual void InitMultipliers(std::vector<Multiplier*>& multipliers) override;
};

#endif
