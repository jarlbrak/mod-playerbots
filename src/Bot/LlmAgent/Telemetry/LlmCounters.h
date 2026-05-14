#ifndef _PLAYERBOT_LLMAGENT_LLM_COUNTERS_H
#define _PLAYERBOT_LLMAGENT_LLM_COUNTERS_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

class LlmCounters {
  public:
    struct Snapshot_t {
        uint64_t enqueued_total = 0;
        uint64_t parsed_ok = 0;
        uint64_t parsed_schema_error = 0;
        uint64_t parsed_transport_error = 0;
        uint64_t parsed_http_error = 0;
        uint64_t parsed_timeout = 0;
        uint64_t validator_accepted = 0;
        uint64_t applied = 0;
        uint64_t shadow_accepted = 0;
        uint64_t log_accepted_skipped = 0;
        uint64_t fallback_used = 0;
        uint64_t applied_threw = 0;
        std::unordered_map<std::string, uint64_t> rejected_by_reason;
        std::unordered_map<std::string, uint64_t> tool_received;
        std::unordered_map<std::string, uint64_t> tool_applied;
        std::unordered_map<std::string, uint64_t> tool_rejected_by_reason;
        std::unordered_map<std::string, uint64_t> tool_threw;
        uint64_t tool_no_action = 0;
        uint64_t tool_truncated = 0;
        uint64_t tool_schema_error = 0;

        // Phase 5: T3 chat brain.
        std::unordered_map<std::string, uint64_t> chat_envelope_parsed;  // "ok" | "schema_error" | "missing_utterance"
        std::unordered_map<std::string, uint64_t> chat_event_kind;       // "whisper" | "invite" | "join"
        uint64_t chat_utterances_queued = 0;
        uint64_t chat_sender_offline    = 0;
    };

    void IncEnqueued();
    void IncStatus(const std::string& parsed_status);
    void IncValidatorAccepted();
    void IncValidatorRejected(const std::string& reason);
    void IncApplied();
    void IncShadowAccepted();
    void IncLogAcceptedSkipped();
    void IncFallbackUsed();
    void IncAppliedThrew();

    void IncToolReceived(const std::string& name);
    void IncToolApplied(const std::string& name);
    void IncToolRejected(const std::string& reason);
    void IncToolThrew(const std::string& name);
    void IncToolNoAction();
    void IncToolTruncated();
    void IncToolSchemaError();

    void IncChatEnvelopeParsed(const std::string& status);
    void IncChatEventKind(const std::string& kind);
    void IncChatUtterancesQueued();
    void IncChatSenderOffline();

    Snapshot_t Snapshot() const;
    void DumpToLog() const;

  private:
    std::atomic<uint64_t> enqueued_total_{0};
    std::atomic<uint64_t> parsed_ok_{0};
    std::atomic<uint64_t> parsed_schema_error_{0};
    std::atomic<uint64_t> parsed_transport_error_{0};
    std::atomic<uint64_t> parsed_http_error_{0};
    std::atomic<uint64_t> parsed_timeout_{0};
    std::atomic<uint64_t> validator_accepted_{0};
    std::atomic<uint64_t> applied_{0};
    std::atomic<uint64_t> shadow_accepted_{0};
    std::atomic<uint64_t> log_accepted_skipped_{0};
    std::atomic<uint64_t> fallback_used_{0};
    std::atomic<uint64_t> applied_threw_{0};

    mutable std::mutex                             rejected_mu_;
    std::unordered_map<std::string, uint64_t>      rejected_by_reason_;

    mutable std::mutex                             tool_mu_;
    std::unordered_map<std::string, uint64_t>      tool_received_;
    std::unordered_map<std::string, uint64_t>      tool_applied_;
    std::unordered_map<std::string, uint64_t>      tool_rejected_;
    std::unordered_map<std::string, uint64_t>      tool_threw_;
    std::atomic<uint64_t>                          tool_no_action_{0};
    std::atomic<uint64_t>                          tool_truncated_{0};
    std::atomic<uint64_t>                          tool_schema_error_{0};

    mutable std::mutex                             chat_mu_;
    std::unordered_map<std::string, uint64_t>      chat_envelope_parsed_;
    std::unordered_map<std::string, uint64_t>      chat_event_kind_;
    std::atomic<uint64_t>                          chat_utterances_queued_{0};
    std::atomic<uint64_t>                          chat_sender_offline_{0};
};

#endif
