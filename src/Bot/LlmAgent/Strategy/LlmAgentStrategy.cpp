#include "Strategy/LlmAgentStrategy.h"

void LlmAgentStrategy::InitTriggers(std::vector<TriggerNode*>& triggers) {
    triggers.push_back(
        new TriggerNode(
            "llm replan idle",
            {NextAction("llm replan idle", 4.0f)}
        )
    );
}
