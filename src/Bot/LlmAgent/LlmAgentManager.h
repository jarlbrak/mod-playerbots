#ifndef _PLAYERBOT_LLMAGENT_MANAGER_H
#define _PLAYERBOT_LLMAGENT_MANAGER_H

#include "Vendor/nlohmann_json.hpp"
#include "LlmAgentConfig.h"
#include "Selector/BotSelector.h"
#include "Cooldown/BotCooldownMap.h"
#include "EventBuffer/RecentEventBuffer.h"
#include "Telemetry/LlmCounters.h"
#include "Client/MemoryHttpClient.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <stack>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct LlmRequest {
    uint64_t bot_guid = 0;
    std::string bot_name;
    std::string body_json;        // OpenAI-shaped POST body
    nlohmann::json digest_json;   // kept for JSONL record
    std::chrono::steady_clock::time_point ts_enqueued;
};

struct LlmResult {
    uint64_t bot_guid = 0;
    std::string bot_name;
    std::string parsed_status;    // "ok" | "schema_error" | "transport_error" | "http_error" | "timeout"
    std::string raw_response;
    std::string validator_error;
    nlohmann::json parsed_goal;   // null on error
    uint64_t queue_wait_ms = 0;
    uint64_t inference_ms = 0;
    uint64_t total_latency_ms = 0;
};

class LlmAgentManager {
  public:
    LlmAgentManager() = default;
    ~LlmAgentManager() { Shutdown(); }

    LlmAgentManager(const LlmAgentManager&) = delete;
    LlmAgentManager& operator=(const LlmAgentManager&) = delete;

    static LlmAgentManager& Instance();

    void Start(LlmAgentConfig cfg);
    void Shutdown();

    bool Enabled() const { return cfg_.Enabled; }
    const LlmAgentConfig& Config() const { return cfg_; }
    BotSelector&        Selector()        { return selector_; }
    BotCooldownMap&     Cooldowns()       { return cooldowns_; }
    RecentEventBuffer&  Events()          { return events_; }
    LlmCounters&        Counters()        { return counters_; }
    MemoryHttpClient&   MemoryClient()    { return *memory_client_; }
    LlmApplyMode        ApplyMode() const { return cfg_.ApplyMode; }
    bool IsInFlight(uint64_t bot_guid) const;
    bool HasPendingResults(uint64_t bot_guid) const;

    // Returns false if the bot already has a request in flight.
    bool Enqueue(LlmRequest request);

    std::vector<LlmResult> DrainResults(uint64_t bot_guid);

  private:
    void WorkerLoop();
    void HandleRequest(LlmRequest req);
    void AppendJsonl(const std::string& line);

    LlmAgentConfig                                cfg_;
    BotSelector                                   selector_;
    BotCooldownMap                                cooldowns_;
    RecentEventBuffer                             events_;
    LlmCounters                                   counters_;
    std::unique_ptr<MemoryHttpClient>             memory_client_;
    std::atomic<bool>                             running_{false};
    std::vector<std::thread>                      workers_;

    mutable std::mutex                            queue_mu_;
    std::condition_variable                       queue_cv_;
    std::deque<LlmRequest>                        queue_;

    mutable std::mutex                            inflight_mu_;
    std::unordered_set<uint64_t>                  inflight_;

    mutable std::mutex                            results_mu_;
    std::unordered_map<uint64_t, std::stack<LlmResult>> results_;

    std::mutex                                    jsonl_mu_;
    std::ofstream                                 jsonl_;
};

#endif  // _PLAYERBOT_LLMAGENT_MANAGER_H
