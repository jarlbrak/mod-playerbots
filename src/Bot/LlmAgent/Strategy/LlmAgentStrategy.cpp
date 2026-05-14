#include "Strategy/LlmAgentStrategy.h"

void LlmAgentStrategy::InitTriggers(std::vector<TriggerNode*>& triggers) {
    // Relevance 15.0f > NewRpgStrategy's "new rpg status update" at 11.0f so
    // our action runs first on each Idle tick. We return false from Execute,
    // which lets status_update run next and perform the rule-based transition.
    // This preserves the Phase 1 invariant: the existing path runs unconditionally.
    triggers.push_back(
        new TriggerNode(
            "llm replan idle",
            {NextAction("llm replan idle", 15.0f)}
        )
    );
    triggers.push_back(
        new TriggerNode(
            "llm interact",
            {NextAction("llm interact", 16.0f)}   // > T1's 15.0f so T2 outranks T1 when a human is engaged
        )
    );
}
