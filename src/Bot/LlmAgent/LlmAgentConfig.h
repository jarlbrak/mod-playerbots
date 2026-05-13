#ifndef _PLAYERBOT_LLMAGENT_CONFIG_H
#define _PLAYERBOT_LLMAGENT_CONFIG_H

#include <cstdint>
#include <string>

struct LlmAgentConfig {
    bool        Enabled          = false;
    std::string Endpoint         = "http://127.0.0.1:8080";
    std::string Model            = "qwen2.5-7b-instruct-q4_k_m.gguf";
    uint32_t    WorkerThreads    = 4;
    uint32_t    RequestTimeoutMs = 15000;
    std::string JsonlPath        = "logs/llm_agent_phase1.jsonl";
    std::string SystemPrompt;
};

extern const char* const kDefaultSystemPrompt;

// Generic loader. Source must have a templated Get<T>(const char* key, T default).
template <typename Source>
LlmAgentConfig LoadLlmAgentConfig(const Source& src) {
    LlmAgentConfig cfg;
    cfg.Enabled          = src.template Get<bool>       ("AiPlayerbot.LlmAgent.Enabled",          false);
    cfg.Endpoint         = src.template Get<std::string>("AiPlayerbot.LlmAgent.Endpoint",         std::string{"http://127.0.0.1:8080"});
    cfg.Model            = src.template Get<std::string>("AiPlayerbot.LlmAgent.Model",            std::string{"qwen2.5-7b-instruct-q4_k_m.gguf"});
    cfg.WorkerThreads    = src.template Get<uint32_t>   ("AiPlayerbot.LlmAgent.WorkerThreads",    uint32_t{4});
    cfg.RequestTimeoutMs = src.template Get<uint32_t>   ("AiPlayerbot.LlmAgent.RequestTimeoutMs", uint32_t{15000});
    cfg.JsonlPath        = src.template Get<std::string>("AiPlayerbot.LlmAgent.JsonlPath",        std::string{"logs/llm_agent_phase1.jsonl"});
    cfg.SystemPrompt     = src.template Get<std::string>("AiPlayerbot.LlmAgent.SystemPrompt",     std::string{kDefaultSystemPrompt});
    return cfg;
}

#endif  // _PLAYERBOT_LLMAGENT_CONFIG_H
