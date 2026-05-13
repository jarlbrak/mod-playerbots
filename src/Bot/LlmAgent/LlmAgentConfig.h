#ifndef _PLAYERBOT_LLMAGENT_CONFIG_H
#define _PLAYERBOT_LLMAGENT_CONFIG_H

#include <cstdint>
#include <string>

enum class LlmApplyMode { Log, Shadow, Apply };

struct LlmAgentConfig {
    bool         Enabled            = false;
    std::string  Endpoint           = "http://127.0.0.1:8080";
    std::string  Model              = "qwen2.5-7b-instruct-q4_k_m.gguf";
    uint32_t     WorkerThreads      = 4;
    uint32_t     RequestTimeoutMs   = 15000;
    std::string  JsonlPath          = "logs/llm_agent_phase1.jsonl";
    std::string  SystemPrompt;

    // Phase 2
    LlmApplyMode ApplyMode          = LlmApplyMode::Log;
    uint32_t     SamplePct          = 0;
    bool         SocialOptIn        = true;
    uint32_t     MaxCooldownMinutes = 60;
    uint32_t     FallbackCooldownMs = 300000;
    uint32_t     EventLogSize       = 20;
};

extern const char* const kDefaultSystemPrompt;

LlmApplyMode ParseApplyMode(const std::string& s);

// Generic loader. Source must have a templated Get<T>(const char* key, T default).
template <typename Source>
LlmAgentConfig LoadLlmAgentConfig(const Source& src) {
    LlmAgentConfig cfg;
    cfg.Enabled            = src.template Get<bool>       ("AiPlayerbot.LlmAgent.Enabled",            false);
    cfg.Endpoint           = src.template Get<std::string>("AiPlayerbot.LlmAgent.Endpoint",           std::string{"http://127.0.0.1:8080"});
    cfg.Model              = src.template Get<std::string>("AiPlayerbot.LlmAgent.Model",              std::string{"qwen2.5-7b-instruct-q4_k_m.gguf"});
    cfg.WorkerThreads      = src.template Get<uint32_t>   ("AiPlayerbot.LlmAgent.WorkerThreads",      uint32_t{4});
    cfg.RequestTimeoutMs   = src.template Get<uint32_t>   ("AiPlayerbot.LlmAgent.RequestTimeoutMs",   uint32_t{15000});
    cfg.JsonlPath          = src.template Get<std::string>("AiPlayerbot.LlmAgent.JsonlPath",          std::string{"logs/llm_agent_phase1.jsonl"});
    cfg.SystemPrompt       = src.template Get<std::string>("AiPlayerbot.LlmAgent.SystemPrompt",       std::string{kDefaultSystemPrompt});

    cfg.ApplyMode          = ParseApplyMode(src.template Get<std::string>("AiPlayerbot.LlmAgent.ApplyMode", std::string{"log"}));
    cfg.SamplePct          = src.template Get<uint32_t>("AiPlayerbot.LlmAgent.SamplePct",          uint32_t{0});
    cfg.SocialOptIn        = src.template Get<bool>    ("AiPlayerbot.LlmAgent.SocialOptIn",        true);
    cfg.MaxCooldownMinutes = src.template Get<uint32_t>("AiPlayerbot.LlmAgent.MaxCooldownMinutes", uint32_t{60});
    cfg.FallbackCooldownMs = src.template Get<uint32_t>("AiPlayerbot.LlmAgent.FallbackCooldownMs", uint32_t{300000});
    cfg.EventLogSize       = src.template Get<uint32_t>("AiPlayerbot.LlmAgent.EventLogSize",       uint32_t{20});
    return cfg;
}

#endif  // _PLAYERBOT_LLMAGENT_CONFIG_H
