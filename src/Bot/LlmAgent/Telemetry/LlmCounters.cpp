#include "Telemetry/LlmCounters.h"

#ifndef LLMAGENT_UNIT_TESTS
#include "Log.h"
#endif

#include <sstream>

void LlmCounters::IncEnqueued()              { enqueued_total_.fetch_add(1, std::memory_order_relaxed); }
void LlmCounters::IncValidatorAccepted()     { validator_accepted_.fetch_add(1, std::memory_order_relaxed); }
void LlmCounters::IncApplied()               { applied_.fetch_add(1, std::memory_order_relaxed); }
void LlmCounters::IncShadowAccepted()        { shadow_accepted_.fetch_add(1, std::memory_order_relaxed); }
void LlmCounters::IncLogAcceptedSkipped()    { log_accepted_skipped_.fetch_add(1, std::memory_order_relaxed); }
void LlmCounters::IncFallbackUsed()          { fallback_used_.fetch_add(1, std::memory_order_relaxed); }
void LlmCounters::IncAppliedThrew()          { applied_threw_.fetch_add(1, std::memory_order_relaxed); }

void LlmCounters::IncStatus(const std::string& s) {
    if      (s == "ok")               parsed_ok_.fetch_add(1, std::memory_order_relaxed);
    else if (s == "schema_error")     parsed_schema_error_.fetch_add(1, std::memory_order_relaxed);
    else if (s == "transport_error")  parsed_transport_error_.fetch_add(1, std::memory_order_relaxed);
    else if (s == "http_error")       parsed_http_error_.fetch_add(1, std::memory_order_relaxed);
    else if (s == "timeout")          parsed_timeout_.fetch_add(1, std::memory_order_relaxed);
    // unknown statuses silently dropped (bounded counter set)
}

void LlmCounters::IncValidatorRejected(const std::string& reason) {
    std::lock_guard<std::mutex> g(rejected_mu_);
    ++rejected_by_reason_[reason];
}

LlmCounters::Snapshot_t LlmCounters::Snapshot() const {
    Snapshot_t s;
    s.enqueued_total           = enqueued_total_.load(std::memory_order_relaxed);
    s.parsed_ok                = parsed_ok_.load(std::memory_order_relaxed);
    s.parsed_schema_error      = parsed_schema_error_.load(std::memory_order_relaxed);
    s.parsed_transport_error   = parsed_transport_error_.load(std::memory_order_relaxed);
    s.parsed_http_error        = parsed_http_error_.load(std::memory_order_relaxed);
    s.parsed_timeout           = parsed_timeout_.load(std::memory_order_relaxed);
    s.validator_accepted       = validator_accepted_.load(std::memory_order_relaxed);
    s.applied                  = applied_.load(std::memory_order_relaxed);
    s.shadow_accepted          = shadow_accepted_.load(std::memory_order_relaxed);
    s.log_accepted_skipped     = log_accepted_skipped_.load(std::memory_order_relaxed);
    s.fallback_used            = fallback_used_.load(std::memory_order_relaxed);
    s.applied_threw            = applied_threw_.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> g(rejected_mu_);
        s.rejected_by_reason = rejected_by_reason_;
    }
    return s;
}

void LlmCounters::DumpToLog() const {
    auto s = Snapshot();
    std::ostringstream out;
    out << "[LlmAgent counters]"
        << " enqueued=" << s.enqueued_total
        << " ok=" << s.parsed_ok
        << " schema_err=" << s.parsed_schema_error
        << " transport_err=" << s.parsed_transport_error
        << " http_err=" << s.parsed_http_error
        << " timeout=" << s.parsed_timeout
        << " val_accepted=" << s.validator_accepted
        << " applied=" << s.applied
        << " shadow_accepted=" << s.shadow_accepted
        << " log_accepted_skipped=" << s.log_accepted_skipped
        << " fallback_used=" << s.fallback_used
        << " applied_threw=" << s.applied_threw;
    for (const auto& [k, v] : s.rejected_by_reason) {
        out << " " << k << "=" << v;
    }
#ifndef LLMAGENT_UNIT_TESTS
    LOG_INFO("playerbots", "{}", out.str());
#endif
}
