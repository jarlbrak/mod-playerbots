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
    {
        std::lock_guard<std::mutex> g(tool_mu_);
        s.tool_received = tool_received_;
        s.tool_applied = tool_applied_;
        s.tool_rejected_by_reason = tool_rejected_;
        s.tool_threw = tool_threw_;
    }
    s.tool_no_action = tool_no_action_.load(std::memory_order_relaxed);
    s.tool_truncated = tool_truncated_.load(std::memory_order_relaxed);
    s.tool_schema_error = tool_schema_error_.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> g(chat_mu_);
        s.chat_envelope_parsed = chat_envelope_parsed_;
        s.chat_event_kind = chat_event_kind_;
    }
    s.chat_utterances_queued = chat_utterances_queued_.load(std::memory_order_relaxed);
    s.chat_sender_offline    = chat_sender_offline_.load(std::memory_order_relaxed);
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
    out << " tool_no_action=" << s.tool_no_action
        << " tool_truncated=" << s.tool_truncated
        << " tool_schema_error=" << s.tool_schema_error;
    for (const auto& [k, v] : s.tool_received) {
        out << " tool_recv[" << k << "]=" << v;
    }
    for (const auto& [k, v] : s.tool_applied) {
        out << " tool_applied[" << k << "]=" << v;
    }
    out << " chat_utterances_queued=" << s.chat_utterances_queued
        << " chat_sender_offline="    << s.chat_sender_offline;
    for (const auto& [k, v] : s.chat_envelope_parsed)
        out << " chat_envelope[" << k << "]=" << v;
    for (const auto& [k, v] : s.chat_event_kind)
        out << " chat_event_kind[" << k << "]=" << v;
#ifndef LLMAGENT_UNIT_TESTS
    // Was "playerbots" — that channel doesn't route to Server.log on this AC
    // build (Phase 4 → Phase 5 finding). Use "server.loading" so DumpToLog
    // output actually surfaces on Shutdown.
    LOG_INFO("server.loading", "{}", out.str());
#endif
}

void LlmCounters::IncToolReceived(const std::string& name) {
    std::lock_guard<std::mutex> g(tool_mu_);
    ++tool_received_[name];
}
void LlmCounters::IncToolApplied(const std::string& name) {
    std::lock_guard<std::mutex> g(tool_mu_);
    ++tool_applied_[name];
}
void LlmCounters::IncToolRejected(const std::string& reason) {
    std::lock_guard<std::mutex> g(tool_mu_);
    ++tool_rejected_[reason];
}
void LlmCounters::IncToolThrew(const std::string& name) {
    std::lock_guard<std::mutex> g(tool_mu_);
    ++tool_threw_[name];
}
void LlmCounters::IncToolNoAction()    { tool_no_action_.fetch_add(1, std::memory_order_relaxed); }
void LlmCounters::IncToolTruncated()   { tool_truncated_.fetch_add(1, std::memory_order_relaxed); }
void LlmCounters::IncToolSchemaError() { tool_schema_error_.fetch_add(1, std::memory_order_relaxed); }

void LlmCounters::IncChatEnvelopeParsed(const std::string& status) {
    std::lock_guard<std::mutex> g(chat_mu_);
    ++chat_envelope_parsed_[status];
}
void LlmCounters::IncChatEventKind(const std::string& kind) {
    std::lock_guard<std::mutex> g(chat_mu_);
    ++chat_event_kind_[kind];
}
void LlmCounters::IncChatUtterancesQueued() {
    chat_utterances_queued_.fetch_add(1, std::memory_order_relaxed);
}
void LlmCounters::IncChatSenderOffline() {
    chat_sender_offline_.fetch_add(1, std::memory_order_relaxed);
}
