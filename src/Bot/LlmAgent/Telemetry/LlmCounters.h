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
};

#endif
