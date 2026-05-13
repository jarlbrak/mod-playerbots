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
