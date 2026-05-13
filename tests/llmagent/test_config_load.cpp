#include "doctest.h"
#include "LlmAgentConfig.h"
#include <unordered_map>
#include <string>

namespace {

struct StubConfigSource {
    std::unordered_map<std::string, std::string> values;

    template <typename T>
    T Get(const char* key, T default_value) const {
        auto it = values.find(key);
        if (it == values.end()) return default_value;
        if constexpr (std::is_same_v<T, bool>) {
            return it->second == "1" || it->second == "true";
        } else if constexpr (std::is_same_v<T, int> || std::is_same_v<T, uint32_t>) {
            return static_cast<T>(std::stoi(it->second));
        } else {
            return it->second;
        }
    }
};

} // namespace

TEST_CASE("LlmAgentConfig defaults when nothing set") {
    StubConfigSource src;
    LlmAgentConfig cfg = LoadLlmAgentConfig(src);

    CHECK(cfg.Enabled == false);
    CHECK(cfg.Endpoint == "http://127.0.0.1:8080");
    CHECK(cfg.Model == "qwen2.5-7b-instruct-q4_k_m.gguf");
    CHECK(cfg.WorkerThreads == 4u);
    CHECK(cfg.RequestTimeoutMs == 15000u);
    CHECK(cfg.JsonlPath == "logs/llm_agent_phase1.jsonl");
    CHECK(!cfg.SystemPrompt.empty());
}

TEST_CASE("LlmAgentConfig overrides applied") {
    StubConfigSource src;
    src.values["AiPlayerbot.LlmAgent.Enabled"] = "1";
    src.values["AiPlayerbot.LlmAgent.Endpoint"] = "http://10.0.0.5:9000";
    src.values["AiPlayerbot.LlmAgent.Model"] = "custom.gguf";
    src.values["AiPlayerbot.LlmAgent.WorkerThreads"] = "8";
    src.values["AiPlayerbot.LlmAgent.RequestTimeoutMs"] = "30000";
    src.values["AiPlayerbot.LlmAgent.JsonlPath"] = "/var/log/llm.jsonl";
    src.values["AiPlayerbot.LlmAgent.SystemPrompt"] = "Custom prompt.";

    LlmAgentConfig cfg = LoadLlmAgentConfig(src);

    CHECK(cfg.Enabled == true);
    CHECK(cfg.Endpoint == "http://10.0.0.5:9000");
    CHECK(cfg.Model == "custom.gguf");
    CHECK(cfg.WorkerThreads == 8u);
    CHECK(cfg.RequestTimeoutMs == 30000u);
    CHECK(cfg.JsonlPath == "/var/log/llm.jsonl");
    CHECK(cfg.SystemPrompt == "Custom prompt.");
}

TEST_CASE("LlmAgentConfig Phase 2 defaults") {
    StubConfigSource src;
    LlmAgentConfig cfg = LoadLlmAgentConfig(src);

    CHECK(cfg.ApplyMode == LlmApplyMode::Log);
    CHECK(cfg.SamplePct == 0u);
    CHECK(cfg.SocialOptIn == true);
    CHECK(cfg.MaxCooldownMinutes == 60u);
    CHECK(cfg.FallbackCooldownMs == 300000u);
    CHECK(cfg.EventLogSize == 20u);
}

TEST_CASE("LlmAgentConfig Phase 2 overrides applied") {
    StubConfigSource src;
    src.values["AiPlayerbot.LlmAgent.ApplyMode"]          = "apply";
    src.values["AiPlayerbot.LlmAgent.SamplePct"]          = "25";
    src.values["AiPlayerbot.LlmAgent.SocialOptIn"]        = "0";
    src.values["AiPlayerbot.LlmAgent.MaxCooldownMinutes"] = "30";
    src.values["AiPlayerbot.LlmAgent.FallbackCooldownMs"] = "120000";
    src.values["AiPlayerbot.LlmAgent.EventLogSize"]       = "50";

    LlmAgentConfig cfg = LoadLlmAgentConfig(src);

    CHECK(cfg.ApplyMode == LlmApplyMode::Apply);
    CHECK(cfg.SamplePct == 25u);
    CHECK(cfg.SocialOptIn == false);
    CHECK(cfg.MaxCooldownMinutes == 30u);
    CHECK(cfg.FallbackCooldownMs == 120000u);
    CHECK(cfg.EventLogSize == 50u);
}

TEST_CASE("LlmAgentConfig ApplyMode parses shadow") {
    StubConfigSource src;
    src.values["AiPlayerbot.LlmAgent.ApplyMode"] = "shadow";
    CHECK(LoadLlmAgentConfig(src).ApplyMode == LlmApplyMode::Shadow);
}

TEST_CASE("LlmAgentConfig ApplyMode parses log") {
    StubConfigSource src;
    src.values["AiPlayerbot.LlmAgent.ApplyMode"] = "log";
    CHECK(LoadLlmAgentConfig(src).ApplyMode == LlmApplyMode::Log);
}

TEST_CASE("LlmAgentConfig ApplyMode falls back to Log on unknown value") {
    StubConfigSource src;
    src.values["AiPlayerbot.LlmAgent.ApplyMode"] = "garbage";
    CHECK(LoadLlmAgentConfig(src).ApplyMode == LlmApplyMode::Log);
}

TEST_CASE("LlmAgentConfig MemorySidecar defaults") {
    StubConfigSource src;
    LlmAgentConfig cfg = LoadLlmAgentConfig(src);
    CHECK(cfg.MemorySidecar_Endpoint == "http://127.0.0.1:8090");
    CHECK(cfg.MemorySidecar_RequestTimeoutMs == 2000u);
    CHECK(cfg.MemorySidecar_EnableWrites == true);
    CHECK(cfg.MemorySidecar_RecallTopK == 3u);
    CHECK(cfg.MemorySidecar_HintMaxChars == 1200u);
}

TEST_CASE("LlmAgentConfig MemorySidecar overrides applied") {
    StubConfigSource src;
    src.values["AiPlayerbot.MemorySidecar.Endpoint"]         = "http://10.0.0.5:9999";
    src.values["AiPlayerbot.MemorySidecar.RequestTimeoutMs"] = "1500";
    src.values["AiPlayerbot.MemorySidecar.EnableWrites"]     = "0";
    src.values["AiPlayerbot.MemorySidecar.RecallTopK"]       = "5";
    src.values["AiPlayerbot.MemorySidecar.HintMaxChars"]     = "800";

    LlmAgentConfig cfg = LoadLlmAgentConfig(src);
    CHECK(cfg.MemorySidecar_Endpoint == "http://10.0.0.5:9999");
    CHECK(cfg.MemorySidecar_RequestTimeoutMs == 1500u);
    CHECK(cfg.MemorySidecar_EnableWrites == false);
    CHECK(cfg.MemorySidecar_RecallTopK == 5u);
    CHECK(cfg.MemorySidecar_HintMaxChars == 800u);
}
