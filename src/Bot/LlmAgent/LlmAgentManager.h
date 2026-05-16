#ifndef _PLAYERBOT_LLMAGENT_MANAGER_H
#define _PLAYERBOT_LLMAGENT_MANAGER_H

#include "Vendor/nlohmann_json.hpp"
#include "LlmAgentConfig.h"
#include "Selector/BotSelector.h"
#include "Cooldown/BotCooldownMap.h"
#include "EventBuffer/RecentEventBuffer.h"
#include "EventBuffer/InteractionEventBuffer.h"
#include "Chat/WhisperBuffer.h"
#include "Chat/PersonaCache.h"
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
    uint32_t tier = 1;   // 1 = T1 (replan), 2 = T2 (interactive)

    // Phase 5.2 v9: chat-routing context captured at enqueue time so the
    // drain-phase can route the reply on the SAME channel the inbound used,
    // without re-querying the interaction buffer (which may have new entries
    // by the time inference finishes ~20-30 s later).
    uint8_t  chat_event_kind = 0;  // LlmAgentTier3::EventKind: 0=Whisper, 1=Invite, 2=Join
    uint32_t chat_type = 0;        // AC ChatMsg enum (CHAT_MSG_PARTY_LEADER=51, WHISPER=7, ...)
    std::string chat_sender_name;
    uint64_t chat_sender_guid = 0;
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
    uint32_t tier = 1;   // 1 = T1 (replan), 2 = T2 (interactive)

    // Phase 5.2 v9: captured chat context (echoed from LlmRequest) so the
    // action's drain phase can route the reply correctly.
    uint8_t  chat_event_kind = 0;
    uint32_t chat_type = 0;
    std::string chat_sender_name;
    uint64_t chat_sender_guid = 0;
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
    BotSelector&           Selector()        { return selector_; }
    BotCooldownMap&        Cooldowns()       { return cooldowns_; }
    BotCooldownMap&        T2Cooldowns()     { return t2_cooldowns_; }
    RecentEventBuffer&     Events()          { return events_; }
    InteractionEventBuffer& Interactions()  { return interactions_; }
    WhisperBuffer&         Whispers()         { return whispers_; }
    BotCooldownMap&        T3Cooldowns()      { return t3_cooldowns_; }
    LlmCounters&           Counters()        { return counters_; }
    MemoryHttpClient&      MemoryClient()    { return *memory_client_; }
#ifndef LLMAGENT_UNIT_TESTS
    PersonaCache&          Persona()          { return *persona_; }
#endif
    LlmApplyMode        ApplyMode() const { return cfg_.ApplyMode; }
    bool IsInFlight(uint64_t bot_guid, uint32_t tier = 1) const;
    bool HasPendingResults(uint64_t bot_guid, uint32_t tier = 1) const;

    // Returns false if the bot already has a request in flight for the given tier.
    bool Enqueue(LlmRequest request);

    std::vector<LlmResult> DrainResults(uint64_t bot_guid, uint32_t tier = 1);

  private:
    void WorkerLoop();
    void HandleRequest(LlmRequest req);
    void AppendJsonl(const std::string& line);

    LlmAgentConfig                                cfg_;
    BotSelector                                   selector_;
    BotCooldownMap                                cooldowns_;
    BotCooldownMap                                t2_cooldowns_;  // separate T2 cooldown
    BotCooldownMap                                t3_cooldowns_;
    WhisperBuffer                                 whispers_;
    RecentEventBuffer                             events_;
    InteractionEventBuffer                        interactions_;
    LlmCounters                                   counters_;
    std::unique_ptr<MemoryHttpClient>             memory_client_;
#ifndef LLMAGENT_UNIT_TESTS
    std::unique_ptr<PersonaCache>                 persona_;
#endif
    std::atomic<bool>                             running_{false};
    std::vector<std::thread>                      workers_;

    mutable std::mutex                            queue_mu_;
    std::condition_variable                       queue_cv_;
    std::deque<LlmRequest>                        queue_;

    mutable std::mutex                            inflight_mu_;
    std::unordered_map<uint64_t, std::unordered_set<uint32_t>>   inflight_;  // bot_guid → tiers in flight

    mutable std::mutex                            results_mu_;
    std::unordered_map<uint64_t,
                       std::unordered_map<uint32_t, std::stack<LlmResult>>>  results_;  // bot_guid → (tier → stack)

    std::mutex                                    jsonl_mu_;
    std::ofstream                                 jsonl_;
};

#endif  // _PLAYERBOT_LLMAGENT_MANAGER_H
