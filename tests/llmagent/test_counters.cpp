#include "doctest.h"
#include "Telemetry/LlmCounters.h"
#include <atomic>
#include <thread>
#include <vector>

TEST_CASE("LlmCounters starts at zero") {
    LlmCounters c;
    auto s = c.Snapshot();
    CHECK(s.enqueued_total == 0);
    CHECK(s.parsed_ok == 0);
    CHECK(s.applied == 0);
}

TEST_CASE("LlmCounters IncEnqueued increments") {
    LlmCounters c;
    for (int i = 0; i < 7; ++i) c.IncEnqueued();
    CHECK(c.Snapshot().enqueued_total == 7);
}

TEST_CASE("LlmCounters parsed-status routes to correct bucket") {
    LlmCounters c;
    c.IncStatus("ok");
    c.IncStatus("ok");
    c.IncStatus("schema_error");
    c.IncStatus("timeout");
    c.IncStatus("garbage_unknown");  // unknown is silently dropped
    auto s = c.Snapshot();
    CHECK(s.parsed_ok == 2);
    CHECK(s.parsed_schema_error == 1);
    CHECK(s.parsed_timeout == 1);
}

TEST_CASE("LlmCounters atomic under contention") {
    LlmCounters c;
    constexpr int N = 8;
    constexpr int PER = 1000;
    std::vector<std::thread> threads;
    for (int t = 0; t < N; ++t) {
        threads.emplace_back([&]{
            for (int i = 0; i < PER; ++i) {
                c.IncEnqueued();
                c.IncApplied();
            }
        });
    }
    for (auto& th : threads) th.join();
    auto s = c.Snapshot();
    CHECK(s.enqueued_total == N * PER);
    CHECK(s.applied == N * PER);
}

TEST_CASE("LlmCounters outcome counters cover all four buckets") {
    LlmCounters c;
    c.IncApplied();
    c.IncShadowAccepted();
    c.IncLogAcceptedSkipped();
    c.IncFallbackUsed();
    c.IncFallbackUsed();
    auto s = c.Snapshot();
    CHECK(s.applied == 1);
    CHECK(s.shadow_accepted == 1);
    CHECK(s.log_accepted_skipped == 1);
    CHECK(s.fallback_used == 2);
}

TEST_CASE("LlmCounters rejected_by_reason accumulates per reason") {
    LlmCounters c;
    c.IncValidatorRejected("rejected_quest_not_in_log");
    c.IncValidatorRejected("rejected_quest_not_in_log");
    c.IncValidatorRejected("rejected_map_mismatch");
    auto s = c.Snapshot();
    CHECK(s.rejected_by_reason["rejected_quest_not_in_log"] == 2);
    CHECK(s.rejected_by_reason["rejected_map_mismatch"] == 1);
}

TEST_CASE("LlmCounters tool counters increment per tool name") {
    LlmCounters c;
    c.IncToolReceived("set_goal");
    c.IncToolReceived("set_goal");
    c.IncToolApplied("set_goal");
    c.IncToolApplied("accept_party_invite");
    c.IncToolRejected("rejected_npc_not_in_range");
    c.IncToolThrew("vendor_junk");
    c.IncToolNoAction();
    c.IncToolTruncated();
    c.IncToolSchemaError();

    auto s = c.Snapshot();
    CHECK(s.tool_received["set_goal"] == 2);
    CHECK(s.tool_applied["set_goal"] == 1);
    CHECK(s.tool_applied["accept_party_invite"] == 1);
    CHECK(s.tool_rejected_by_reason["rejected_npc_not_in_range"] == 1);
    CHECK(s.tool_threw["vendor_junk"] == 1);
    CHECK(s.tool_no_action == 1);
    CHECK(s.tool_truncated == 1);
    CHECK(s.tool_schema_error == 1);
}

TEST_CASE("LlmCounters tool counters thread-safe under contention") {
    LlmCounters c;
    constexpr int N = 4;
    constexpr int PER = 1000;
    std::vector<std::thread> threads;
    for (int t = 0; t < N; ++t) {
        threads.emplace_back([&]{
            for (int i = 0; i < PER; ++i) {
                c.IncToolReceived("set_goal");
                c.IncToolApplied("set_goal");
            }
        });
    }
    for (auto& th : threads) th.join();
    auto s = c.Snapshot();
    CHECK(s.tool_received["set_goal"] == N * PER);
    CHECK(s.tool_applied["set_goal"] == N * PER);
}

TEST_CASE("LlmCounters chat counters increment per status + event_kind") {
    LlmCounters c;
    c.IncChatEnvelopeParsed("ok");
    c.IncChatEnvelopeParsed("schema_error");
    c.IncChatEnvelopeParsed("ok");
    c.IncChatUtterancesQueued();
    c.IncChatEventKind("whisper");
    c.IncChatEventKind("invite");
    c.IncChatEventKind("whisper");
    c.IncChatSenderOffline();

    auto s = c.Snapshot();
    CHECK(s.chat_envelope_parsed["ok"] == 2);
    CHECK(s.chat_envelope_parsed["schema_error"] == 1);
    CHECK(s.chat_utterances_queued == 1);
    CHECK(s.chat_event_kind["whisper"] == 2);
    CHECK(s.chat_event_kind["invite"] == 1);
    CHECK(s.chat_sender_offline == 1);
}

TEST_CASE("LlmCounters chat counters thread-safe") {
    LlmCounters c;
    constexpr int N = 4;
    constexpr int PER = 500;
    std::vector<std::thread> threads;
    for (int t = 0; t < N; ++t)
        threads.emplace_back([&]{
            for (int i = 0; i < PER; ++i) {
                c.IncChatEnvelopeParsed("ok");
                c.IncChatUtterancesQueued();
                c.IncChatEventKind("whisper");
            }
        });
    for (auto& th : threads) th.join();
    auto s = c.Snapshot();
    CHECK(s.chat_envelope_parsed["ok"] == N * PER);
    CHECK(s.chat_utterances_queued == N * PER);
    CHECK(s.chat_event_kind["whisper"] == N * PER);
}

TEST_CASE("LlmCounters chat Snapshot includes empty map when nothing set") {
    LlmCounters c;
    auto s = c.Snapshot();
    CHECK(s.chat_envelope_parsed.empty());
    CHECK(s.chat_event_kind.empty());
    CHECK(s.chat_utterances_queued == 0);
    CHECK(s.chat_sender_offline == 0);
}
