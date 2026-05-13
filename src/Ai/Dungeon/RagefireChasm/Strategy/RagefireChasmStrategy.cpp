#include "RagefireChasmStrategy.h"
#include "RagefireChasmMultipliers.h"

void ClassicDungeonRFCStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // RFC has no scripted boss spells. Generic combat triggers from the base
    // strategy already handle interrupts, dodges, and assist-target. Leave
    // this list empty; add per-boss triggers only when observation shows
    // need (e.g., if Jergosh's Curse of Weakness lands frequently enough to
    // be worth dispelling).
}

void ClassicDungeonRFCStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new OggleflintMultiplier(botAI));
    multipliers.push_back(new JergoshTheInvokerMultiplier(botAI));
    multipliers.push_back(new BazzalanMultiplier(botAI));
    multipliers.push_back(new TaragamanTheHungererMultiplier(botAI));
}
