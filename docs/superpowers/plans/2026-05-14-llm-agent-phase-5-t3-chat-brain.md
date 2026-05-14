# LLM Agent Phase 5 (T3 Chat Brain) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add Tier-3 chat brain — bots reply to whispers, invites, and group-joins with one combined LLM call returning `{utterance, side_effects}`. The utterance goes through AzerothCore's existing `chatReplies` queue; the side-effects flow through Phase 4's existing `ParseToolCalls`/`Validate`/`ApplyToolCall` pipeline.

**Architecture:** New tier alongside T1/T2. Phase 4's plumbing is already tier-aware (commit `a2a6d16d` made `IsInFlight`/`DrainResults` take a tier arg; commit `c7f35ec7` made the worker skip the T1 goal parser for tier≠1). Phase 5 swaps the strategy's T2 trigger node for a T3 trigger node, adds new `Tier3_ChatBrain`/`LlmChatTrigger`/`LlmChatAction` files, adds two pieces of in-memory state (`WhisperBuffer` for per-`(bot, sender)` dialogue history and `PersonaCache` for the persona text), and reuses every Phase 4 validator + executor unchanged.

**Tech Stack:** C++17, doctest (vendored), nlohmann/json (vendored), cpp-httplib (vendored), AzerothCore worldserver bindings, llama.cpp `--response-format json_schema`, podman/Quadlet on Heimdal.

**Spec:** `docs/superpowers/specs/2026-05-13-llm-agent-phase-5-t3-chat-brain-design.md` (commit `3590baaa` on main).

---

## Task 0: Branch + cherry-pick spec

**Files:**
- (none modified — git only)

- [ ] **Step 1: Create branch off Phase 4 tip**

```bash
cd /Users/tbrack/Documents/Projects/playerbots-dev/mod-playerbots
git fetch origin
git checkout -b claude/llm-agent-phase-5-t3-chat-brain origin/claude/llm-agent-phase-4-t2-interactive
git log --oneline -3
```

Expected: HEAD on Phase 4 tip (`c63d5c80` or descendant of `b6abf92c` "Phase 4 T2 smoke results").

- [ ] **Step 2: Cherry-pick the Phase 5 design spec from main**

```bash
git cherry-pick 3590baaa
git log --oneline -3
```

Expected: top commit is the cherry-pick (`docs(llm-agent): add Phase 5 T3 chat brain design spec`), new SHA on the branch.

- [ ] **Step 3: Push branch**

```bash
git push -u origin claude/llm-agent-phase-5-t3-chat-brain
```

Expected: branch created on remote.

- [ ] **Step 4: Verify unit-test baseline still green**

```bash
cmake --build build/llmagent_tests && ./build/llmagent_tests/llmagent_unit_tests | tail -3
```

Expected: `137 | 137 passed | 0 failed`.

---

## Task 1: `WhisperBuffer`

**Files:**
- Create: `src/Bot/LlmAgent/Chat/WhisperBuffer.h`
- Create: `src/Bot/LlmAgent/Chat/WhisperBuffer.cpp`
- Create: `tests/llmagent/test_whisper_buffer.cpp`
- Modify: `tests/llmagent/CMakeLists.txt`

TDD.

- [ ] **Step 1: Write `tests/llmagent/test_whisper_buffer.cpp`**

```cpp
#include "doctest.h"
#include "Chat/WhisperBuffer.h"

#include <thread>
#include <vector>

TEST_CASE("WhisperBuffer push then snapshot returns newest first") {
    WhisperBuffer buf;
    buf.Push(1, 2, WhisperEntry::Incoming, "hello",       1000);
    buf.Push(1, 2, WhisperEntry::Outgoing, "hi there",    1001);
    buf.Push(1, 2, WhisperEntry::Incoming, "want group?", 1002);

    auto snap = buf.SnapshotFor(1, 2, /*now*/1100, /*window*/3600, /*max_n*/10);
    REQUIRE(snap.size() == 3);
    CHECK(snap[0].text == "want group?");
    CHECK(snap[0].direction == WhisperEntry::Incoming);
    CHECK(snap[2].text == "hello");
}

TEST_CASE("WhisperBuffer SnapshotFor respects max_n") {
    WhisperBuffer buf;
    for (int i = 0; i < 10; ++i)
        buf.Push(1, 2, WhisperEntry::Incoming, "msg" + std::to_string(i), 1000 + i);

    auto snap = buf.SnapshotFor(1, 2, /*now*/2000, /*window*/3600, /*max_n*/3);
    REQUIRE(snap.size() == 3);
    CHECK(snap[0].text == "msg9");
    CHECK(snap[2].text == "msg7");
}

TEST_CASE("WhisperBuffer SnapshotFor skips entries older than window") {
    WhisperBuffer buf;
    buf.Push(1, 2, WhisperEntry::Incoming, "old",   1000);
    buf.Push(1, 2, WhisperEntry::Incoming, "fresh", 2000);

    auto snap = buf.SnapshotFor(1, 2, /*now*/2100, /*window*/200, /*max_n*/10);
    REQUIRE(snap.size() == 1);
    CHECK(snap[0].text == "fresh");
}

TEST_CASE("WhisperBuffer ExpireOlderThan drops stale globally") {
    WhisperBuffer buf;
    buf.Push(1, 2, WhisperEntry::Incoming, "very old",  500);
    buf.Push(1, 3, WhisperEntry::Incoming, "also old",  600);
    buf.Push(1, 2, WhisperEntry::Incoming, "fresh",     2000);

    buf.ExpireOlderThan(/*now*/2100, /*window*/200);
    CHECK(buf.SnapshotFor(1, 2, 2100, 3600, 10).size() == 1);
    CHECK(buf.SnapshotFor(1, 3, 2100, 3600, 10).empty());
}

TEST_CASE("WhisperBuffer Clear targets a single pair") {
    WhisperBuffer buf;
    buf.Push(1, 2, WhisperEntry::Incoming, "x", 1000);
    buf.Push(1, 3, WhisperEntry::Incoming, "y", 1000);

    buf.Clear(1, 2);
    CHECK(buf.SnapshotFor(1, 2, 2000, 3600, 10).empty());
    CHECK(buf.SnapshotFor(1, 3, 2000, 3600, 10).size() == 1);
}

TEST_CASE("WhisperBuffer ClearAll empties everything") {
    WhisperBuffer buf;
    buf.Push(1, 2, WhisperEntry::Incoming, "a", 1000);
    buf.Push(4, 5, WhisperEntry::Incoming, "b", 1000);

    buf.ClearAll();
    CHECK(buf.SnapshotFor(1, 2, 2000, 3600, 10).empty());
    CHECK(buf.SnapshotFor(4, 5, 2000, 3600, 10).empty());
}

TEST_CASE("WhisperBuffer concurrent pushes are race-free") {
    WhisperBuffer buf;
    constexpr int N = 4;
    constexpr int PER = 500;
    std::vector<std::thread> threads;
    for (int t = 0; t < N; ++t) {
        threads.emplace_back([&, t]{
            for (int i = 0; i < PER; ++i)
                buf.Push(1, 2, WhisperEntry::Incoming,
                         "msg-" + std::to_string(t) + "-" + std::to_string(i),
                         1000 + i);
        });
    }
    for (auto& th : threads) th.join();
    auto snap = buf.SnapshotFor(1, 2, /*now*/3000, /*window*/3600, /*max_n*/100000);
    CHECK(snap.size() == static_cast<size_t>(N * PER));
}
```

- [ ] **Step 2: Add test + impl source to `tests/llmagent/CMakeLists.txt`**

Append `test_whisper_buffer.cpp` to the test source list and `${LLMAGENT_DIR}/Chat/WhisperBuffer.cpp` to the source list (alongside the other LLMAGENT_DIR sources).

- [ ] **Step 3: Run — expect FAIL**

```bash
cmake --build build/llmagent_tests 2>&1 | tail -5
```

Expected: cmake error "Cannot find source file: .../Chat/WhisperBuffer.cpp".

- [ ] **Step 4: Write `src/Bot/LlmAgent/Chat/WhisperBuffer.h`**

```cpp
#ifndef _PLAYERBOT_LLMAGENT_WHISPER_BUFFER_H
#define _PLAYERBOT_LLMAGENT_WHISPER_BUFFER_H

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

struct WhisperEntry {
    enum Direction : uint8_t { Incoming = 0, Outgoing = 1 };
    Direction   direction = Incoming;
    std::string text;
    int64_t     ts = 0;
};

class WhisperBuffer {
  public:
    void Push(uint64_t bot_guid, uint64_t sender_guid,
              WhisperEntry::Direction dir, std::string text, int64_t ts);

    // Returns last up-to-max_n entries newest-first whose ts > now-window.
    std::vector<WhisperEntry> SnapshotFor(uint64_t bot_guid, uint64_t sender_guid,
                                          int64_t now, int64_t window_seconds,
                                          std::size_t max_n) const;

    void ExpireOlderThan(int64_t now, int64_t window_seconds);
    void Clear(uint64_t bot_guid, uint64_t sender_guid);
    void ClearAll();

  private:
    using Key = std::pair<uint64_t, uint64_t>;

    mutable std::mutex                       mu_;
    std::map<Key, std::deque<WhisperEntry>>  by_pair_;
};

#endif  // _PLAYERBOT_LLMAGENT_WHISPER_BUFFER_H
```

- [ ] **Step 5: Write `src/Bot/LlmAgent/Chat/WhisperBuffer.cpp`**

```cpp
#include "Chat/WhisperBuffer.h"

#include <algorithm>

void WhisperBuffer::Push(uint64_t bot_guid, uint64_t sender_guid,
                         WhisperEntry::Direction dir, std::string text, int64_t ts) {
    std::lock_guard<std::mutex> g(mu_);
    auto& dq = by_pair_[{bot_guid, sender_guid}];
    dq.push_back(WhisperEntry{dir, std::move(text), ts});
}

std::vector<WhisperEntry>
WhisperBuffer::SnapshotFor(uint64_t bot_guid, uint64_t sender_guid,
                           int64_t now, int64_t window_seconds,
                           std::size_t max_n) const {
    std::vector<WhisperEntry> out;
    std::lock_guard<std::mutex> g(mu_);
    auto it = by_pair_.find({bot_guid, sender_guid});
    if (it == by_pair_.end()) return out;
    const int64_t cutoff = now - window_seconds;
    for (auto rit = it->second.rbegin(); rit != it->second.rend(); ++rit) {
        if (rit->ts <= cutoff) break;        // older than window — stop (deque is ts-ordered)
        out.push_back(*rit);
        if (out.size() == max_n) break;
    }
    return out;
}

void WhisperBuffer::ExpireOlderThan(int64_t now, int64_t window_seconds) {
    std::lock_guard<std::mutex> g(mu_);
    const int64_t cutoff = now - window_seconds;
    for (auto it = by_pair_.begin(); it != by_pair_.end(); /*advance below*/) {
        auto& dq = it->second;
        dq.erase(std::remove_if(dq.begin(), dq.end(),
                                [cutoff](const WhisperEntry& e){ return e.ts <= cutoff; }),
                 dq.end());
        if (dq.empty()) it = by_pair_.erase(it);
        else            ++it;
    }
}

void WhisperBuffer::Clear(uint64_t bot_guid, uint64_t sender_guid) {
    std::lock_guard<std::mutex> g(mu_);
    by_pair_.erase({bot_guid, sender_guid});
}

void WhisperBuffer::ClearAll() {
    std::lock_guard<std::mutex> g(mu_);
    by_pair_.clear();
}
```

- [ ] **Step 6: Build + run — expect PASS**

```bash
cmake --build build/llmagent_tests && ./build/llmagent_tests/llmagent_unit_tests | tail -3
```

Expected: `143 | 143 passed | 0 failed` (137 prior + 6 new + 0 — wait, the concurrent-push case is the 7th. So 137 + 7 = 144).

Adjust the line accordingly. The header here is 144.

- [ ] **Step 7: Commit**

```bash
git add src/Bot/LlmAgent/Chat/WhisperBuffer.h \
        src/Bot/LlmAgent/Chat/WhisperBuffer.cpp \
        tests/llmagent/test_whisper_buffer.cpp \
        tests/llmagent/CMakeLists.txt
git commit -m "feat(llm-agent): WhisperBuffer for per-(bot, sender) dialogue history

Bounded sliding window keyed by (bot_guid, sender_guid). SnapshotFor
returns newest-first up to max_n, filtered by window_seconds.
ExpireOlderThan drops stale entries globally. Mutex-protected for
the rare cross-thread access (worldserver-tick + future async hooks)."
```

---

## Task 2: `PersonaCache`

**Files:**
- Create: `src/Bot/LlmAgent/Chat/PersonaCache.h`
- Create: `src/Bot/LlmAgent/Chat/PersonaCache.cpp`
- Create: `tests/llmagent/test_persona_cache.cpp`
- Modify: `tests/llmagent/CMakeLists.txt`

TDD. `PersonaCache` wraps a fetcher callback (`std::function<std::optional<std::string>(uint64_t)>`) so tests can inject a fake without spinning up the sidecar.

- [ ] **Step 1: Write `tests/llmagent/test_persona_cache.cpp`**

```cpp
#include "doctest.h"
#include "Chat/PersonaCache.h"

#include <optional>

namespace {
struct FakeClock {
    int64_t now = 1000;
    int64_t operator()() const { return now; }
};
}  // namespace

TEST_CASE("PersonaCache miss fetches once") {
    int fetches = 0;
    FakeClock clock;
    PersonaCache cache(
        [&](uint64_t /*guid*/) -> std::optional<std::string> {
            ++fetches;
            return std::string{"persona text"};
        },
        /*ttl_seconds*/600,
        [&]{ return clock.now; });

    CHECK(cache.Get(42) == "persona text");
    CHECK(fetches == 1);
}

TEST_CASE("PersonaCache hit avoids refetch within TTL") {
    int fetches = 0;
    FakeClock clock;
    PersonaCache cache(
        [&](uint64_t) -> std::optional<std::string> {
            ++fetches;
            return std::string{"v1"};
        },
        /*ttl*/600,
        [&]{ return clock.now; });

    CHECK(cache.Get(42) == "v1");
    clock.now += 500;
    CHECK(cache.Get(42) == "v1");
    CHECK(fetches == 1);
}

TEST_CASE("PersonaCache TTL expiry triggers refetch") {
    int fetches = 0;
    std::string current = "v1";
    FakeClock clock;
    PersonaCache cache(
        [&](uint64_t) -> std::optional<std::string> {
            ++fetches;
            return current;
        },
        /*ttl*/600,
        [&]{ return clock.now; });

    CHECK(cache.Get(42) == "v1");
    clock.now += 601;
    current = "v2";
    CHECK(cache.Get(42) == "v2");
    CHECK(fetches == 2);
}

TEST_CASE("PersonaCache fetcher returning nullopt yields empty string and caches it") {
    int fetches = 0;
    FakeClock clock;
    PersonaCache cache(
        [&](uint64_t) -> std::optional<std::string> {
            ++fetches;
            return std::nullopt;
        },
        /*ttl*/600,
        [&]{ return clock.now; });

    CHECK(cache.Get(42).empty());
    CHECK(cache.Get(42).empty());
    CHECK(fetches == 1);  // empty-string result is still cached for TTL
}
```

- [ ] **Step 2: Add test + impl source to `tests/llmagent/CMakeLists.txt`**

Append `test_persona_cache.cpp` and `${LLMAGENT_DIR}/Chat/PersonaCache.cpp`.

- [ ] **Step 3: Run — expect FAIL**

```bash
cmake --build build/llmagent_tests 2>&1 | tail -5
```

Expected: cmake error "Cannot find source file: .../Chat/PersonaCache.cpp".

- [ ] **Step 4: Write `src/Bot/LlmAgent/Chat/PersonaCache.h`**

```cpp
#ifndef _PLAYERBOT_LLMAGENT_PERSONA_CACHE_H
#define _PLAYERBOT_LLMAGENT_PERSONA_CACHE_H

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

class PersonaCache {
  public:
    using Fetcher = std::function<std::optional<std::string>(uint64_t bot_guid)>;
    using ClockFn = std::function<int64_t()>;          // returns unix seconds

    PersonaCache(Fetcher fetcher, int64_t ttl_seconds, ClockFn clock);

    // Returns cached value, or fetches and caches on miss/TTL expiry.
    // If fetcher returns nullopt, an empty string is cached (sticky-empty).
    std::string Get(uint64_t bot_guid);

    void Invalidate(uint64_t bot_guid);
    void ClearAll();

  private:
    struct Entry { std::string text; int64_t fetched_at = 0; };

    Fetcher          fetcher_;
    int64_t          ttl_seconds_;
    ClockFn          clock_;
    mutable std::mutex                          mu_;
    std::unordered_map<uint64_t, Entry>         by_bot_;
};

#endif  // _PLAYERBOT_LLMAGENT_PERSONA_CACHE_H
```

- [ ] **Step 5: Write `src/Bot/LlmAgent/Chat/PersonaCache.cpp`**

```cpp
#include "Chat/PersonaCache.h"

PersonaCache::PersonaCache(Fetcher fetcher, int64_t ttl_seconds, ClockFn clock)
    : fetcher_(std::move(fetcher)),
      ttl_seconds_(ttl_seconds),
      clock_(std::move(clock)) {}

std::string PersonaCache::Get(uint64_t bot_guid) {
    const int64_t now = clock_();
    {
        std::lock_guard<std::mutex> g(mu_);
        auto it = by_bot_.find(bot_guid);
        if (it != by_bot_.end() && (now - it->second.fetched_at) < ttl_seconds_)
            return it->second.text;
    }
    std::string text;
    if (auto fetched = fetcher_(bot_guid))
        text = std::move(*fetched);
    {
        std::lock_guard<std::mutex> g(mu_);
        by_bot_[bot_guid] = Entry{text, now};
    }
    return text;
}

void PersonaCache::Invalidate(uint64_t bot_guid) {
    std::lock_guard<std::mutex> g(mu_);
    by_bot_.erase(bot_guid);
}

void PersonaCache::ClearAll() {
    std::lock_guard<std::mutex> g(mu_);
    by_bot_.clear();
}
```

- [ ] **Step 6: Build + run — expect PASS**

```bash
cmake --build build/llmagent_tests && ./build/llmagent_tests/llmagent_unit_tests | tail -3
```

Expected: 144 + 4 = 148 cases pass.

- [ ] **Step 7: Commit**

```bash
git add src/Bot/LlmAgent/Chat/PersonaCache.h \
        src/Bot/LlmAgent/Chat/PersonaCache.cpp \
        tests/llmagent/test_persona_cache.cpp \
        tests/llmagent/CMakeLists.txt
git commit -m "feat(llm-agent): PersonaCache wraps MemoryHttpClient::GetPersonality

Per-bot in-process cache with TTL + injectable fetcher (so unit tests
don't hit the sidecar). Sticky-empty: nullopt fetcher result caches
an empty string for the TTL, so 'sidecar down' doesn't generate a
fetch storm."
```

---

## Task 3: Extract shared `kToolCallOneOf` + add `kT3OutputSchema`

**Files:**
- Modify: `src/Bot/LlmAgent/Tools/ToolCatalog.h`
- Modify: `src/Bot/LlmAgent/Tools/ToolCatalog.cpp`

Phase 4 already defines a 4-branch `oneOf` inside `kT2ToolCallOutputSchema` (commit `ac9233cc`). This task extracts those four branch literals into a separate `kToolCallOneOf` string constant; both T2 and the new T3 schema reference it.

No new unit tests — existing tool-catalog tests verify the schema still parses; we'll also add a single existence-check.

- [ ] **Step 1: Add one assertion to `tests/llmagent/test_tool_catalog.cpp`**

Append:

```cpp
TEST_CASE("kT3OutputSchema parses and constrains utterance + side_effects") {
    auto j = nlohmann::json::parse(kT3OutputSchema);
    REQUIRE(j["type"] == "object");
    auto req = j["required"];
    bool has_utt = false, has_sfx = false;
    for (const auto& r : req) {
        if (r == "utterance")    has_utt = true;
        if (r == "side_effects") has_sfx = true;
    }
    CHECK(has_utt);
    CHECK(has_sfx);
    CHECK(j["properties"]["utterance"]["maxLength"] == 200);
    CHECK(j["properties"]["side_effects"]["maxItems"] == 3);
}
```

- [ ] **Step 2: Run — expect FAIL**

```bash
cmake --build build/llmagent_tests 2>&1 | tail -5
```

Expected: error `kT3OutputSchema` undeclared.

- [ ] **Step 3: Add declaration to `src/Bot/LlmAgent/Tools/ToolCatalog.h`**

After the existing `extern const char* const kT2ToolsJsonSchema;` and `kT2ToolCallOutputSchema` lines, add:

```cpp
// String constant containing the four oneOf branches (accept_party_invite,
// leave_party, set_goal, memory.remember) used by both kT2ToolCallOutputSchema
// and kT3OutputSchema. Embedded inline in those constants via string
// concatenation at link time (no runtime parsing).
extern const char* const kToolCallOneOf;

// Phase 5: T3 output envelope schema. Constrains the model's response to
// {"utterance": <=200-char string, "side_effects": <=3 tool calls}.
extern const char* const kT3OutputSchema;
```

- [ ] **Step 4: Refactor `src/Bot/LlmAgent/Tools/ToolCatalog.cpp`**

Replace the existing `kT2ToolCallOutputSchema` definition. Find the block that currently looks like:

```cpp
const char* const kT2ToolCallOutputSchema = R"({
  "type": "array",
  "minItems": 1,
  "maxItems": 1,
  "items": {
    "oneOf": [
      {"type":"object","required":["name","arguments"], ... /* 4 branches */ ]
  }
})";
```

Replace with:

```cpp
const char* const kToolCallOneOf = R"(
    {"type":"object","required":["name","arguments"],"additionalProperties":false,
     "properties":{
       "name":{"const":"accept_party_invite"},
       "arguments":{"type":"object","required":["from"],"additionalProperties":false,
         "properties":{"from":{"type":"string"}}}}},
    {"type":"object","required":["name","arguments"],"additionalProperties":false,
     "properties":{
       "name":{"const":"leave_party"},
       "arguments":{"type":"object","additionalProperties":false}}},
    {"type":"object","required":["name","arguments"],"additionalProperties":false,
     "properties":{
       "name":{"const":"set_goal"},
       "arguments":{"type":"object",
         "required":["goal","params","reasoning","ttl_minutes"],
         "additionalProperties":false,
         "properties":{
           "goal":{"type":"string","enum":["idle","rest","go_grind","wander_npc","wander_random","do_quest"]},
           "params":{"type":"object"},
           "reasoning":{"type":"string","maxLength":200},
           "ttl_minutes":{"type":"integer","minimum":1,"maximum":1440}}}}},
    {"type":"object","required":["name","arguments"],"additionalProperties":false,
     "properties":{
       "name":{"const":"memory.remember"},
       "arguments":{"type":"object",
         "required":["text","entities","salience"],
         "additionalProperties":false,
         "properties":{
           "text":{"type":"string","maxLength":300},
           "entities":{"type":"array","items":{"type":"string"}},
           "salience":{"type":"number","minimum":0,"maximum":1}}}}}
)";

const char* const kT2ToolCallOutputSchema = R"({
  "type": "array",
  "minItems": 1,
  "maxItems": 1,
  "items": {
    "oneOf": [)" /* kToolCallOneOf injected at runtime via std::string concat in builder */ R"(
    ]
  }
})";

const char* const kT3OutputSchema = R"({
  "type": "object",
  "required": ["utterance", "side_effects"],
  "additionalProperties": false,
  "properties": {
    "utterance":    {"type":"string", "minLength":1, "maxLength":200},
    "side_effects": {
      "type":"array",
      "maxItems":3,
      "items": {"oneOf": [)" /* kToolCallOneOf injected at runtime */ R"(
      ]}
    }
  }
})";
```

Wait — the concat-at-runtime comment above is wrong; raw-string concatenation here would leave empty `"oneOf": []` literals. To keep things simple and link-time, embed `kToolCallOneOf` directly into both schemas by writing them with string-literal concatenation (adjacent string literals are concatenated by the compiler). Replace the previous block with this version:

```cpp
#define LLM_TOOL_CALL_ONEOF_BODY \
    "{\"type\":\"object\",\"required\":[\"name\",\"arguments\"],\"additionalProperties\":false," \
    "\"properties\":{" \
      "\"name\":{\"const\":\"accept_party_invite\"}," \
      "\"arguments\":{\"type\":\"object\",\"required\":[\"from\"],\"additionalProperties\":false," \
        "\"properties\":{\"from\":{\"type\":\"string\"}}}}}," \
    "{\"type\":\"object\",\"required\":[\"name\",\"arguments\"],\"additionalProperties\":false," \
    "\"properties\":{" \
      "\"name\":{\"const\":\"leave_party\"}," \
      "\"arguments\":{\"type\":\"object\",\"additionalProperties\":false}}}," \
    "{\"type\":\"object\",\"required\":[\"name\",\"arguments\"],\"additionalProperties\":false," \
    "\"properties\":{" \
      "\"name\":{\"const\":\"set_goal\"}," \
      "\"arguments\":{\"type\":\"object\"," \
        "\"required\":[\"goal\",\"params\",\"reasoning\",\"ttl_minutes\"]," \
        "\"additionalProperties\":false," \
        "\"properties\":{" \
          "\"goal\":{\"type\":\"string\",\"enum\":[\"idle\",\"rest\",\"go_grind\",\"wander_npc\",\"wander_random\",\"do_quest\"]}," \
          "\"params\":{\"type\":\"object\"}," \
          "\"reasoning\":{\"type\":\"string\",\"maxLength\":200}," \
          "\"ttl_minutes\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":1440}}}}}," \
    "{\"type\":\"object\",\"required\":[\"name\",\"arguments\"],\"additionalProperties\":false," \
    "\"properties\":{" \
      "\"name\":{\"const\":\"memory.remember\"}," \
      "\"arguments\":{\"type\":\"object\"," \
        "\"required\":[\"text\",\"entities\",\"salience\"]," \
        "\"additionalProperties\":false," \
        "\"properties\":{" \
          "\"text\":{\"type\":\"string\",\"maxLength\":300}," \
          "\"entities\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}," \
          "\"salience\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1}}}}}"

const char* const kToolCallOneOf = LLM_TOOL_CALL_ONEOF_BODY;

const char* const kT2ToolCallOutputSchema =
    "{\"type\":\"array\",\"minItems\":1,\"maxItems\":1,"
    "\"items\":{\"oneOf\":[" LLM_TOOL_CALL_ONEOF_BODY "]}}";

const char* const kT3OutputSchema =
    "{\"type\":\"object\","
    "\"required\":[\"utterance\",\"side_effects\"],"
    "\"additionalProperties\":false,"
    "\"properties\":{"
      "\"utterance\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":200},"
      "\"side_effects\":{\"type\":\"array\",\"maxItems\":3,"
        "\"items\":{\"oneOf\":[" LLM_TOOL_CALL_ONEOF_BODY "]}}}"
    "}";

#undef LLM_TOOL_CALL_ONEOF_BODY
```

The macro keeps the schema body in one place (DRY) without runtime concatenation. Plain C++ adjacent-string-literal concatenation glues them at compile time.

- [ ] **Step 5: Build + run — expect PASS**

```bash
cmake --build build/llmagent_tests && ./build/llmagent_tests/llmagent_unit_tests | tail -3
```

Expected: 148 + 1 = 149 cases pass. The pre-existing `kT2ToolCallOutputSchema` tests (1 maxItems check, etc.) keep working because the macro-expanded JSON matches the original character-for-character (modulo whitespace).

- [ ] **Step 6: Commit**

```bash
git add src/Bot/LlmAgent/Tools/ToolCatalog.h \
        src/Bot/LlmAgent/Tools/ToolCatalog.cpp \
        tests/llmagent/test_tool_catalog.cpp
git commit -m "refactor(llm-agent): extract kToolCallOneOf; add kT3OutputSchema

Phase 4's per-tool oneOf branches now live in a single LLM_TOOL_CALL_ONEOF_BODY
macro. kT2ToolCallOutputSchema (Phase 4) and kT3OutputSchema (Phase 5)
both reference it via adjacent-literal concatenation — one source of truth
for the four tool argument schemas.

kT3OutputSchema constrains the model's T3 response to
{utterance: string<=200, side_effects: array<=3 of <oneOf 4 tools>>}."
```

---

## Task 4: `ChatEnvelope` POD + `ParseChatEnvelope`

**Files:**
- Create: `src/Bot/LlmAgent/Schemas/ChatEnvelope.h`
- Create: `src/Bot/LlmAgent/Schemas/ChatEnvelope.cpp`
- Create: `tests/llmagent/test_chat_envelope_parser.cpp`
- Modify: `tests/llmagent/CMakeLists.txt`

TDD.

- [ ] **Step 1: Write `tests/llmagent/test_chat_envelope_parser.cpp`**

```cpp
#include "doctest.h"
#include "Schemas/ChatEnvelope.h"

TEST_CASE("ParseChatEnvelope happy path: utterance + one side_effect") {
    const std::string raw = R"({
        "utterance": "Sure, give me a sec.",
        "side_effects": [
            {"name": "accept_party_invite", "arguments": {"from": "Bob"}}
        ]
    })";
    auto result = ParseChatEnvelope(raw);
    REQUIRE(std::holds_alternative<ParsedChatEnvelope>(result));
    const auto& env = std::get<ParsedChatEnvelope>(result);
    CHECK(env.utterance == "Sure, give me a sec.");
    REQUIRE(env.side_effects.size() == 1);
    CHECK(std::holds_alternative<AcceptPartyInviteCall>(env.side_effects[0]));
    CHECK(std::get<AcceptPartyInviteCall>(env.side_effects[0]).from == "Bob");
}

TEST_CASE("ParseChatEnvelope happy path: empty side_effects") {
    const std::string raw = R"({"utterance": "hi", "side_effects": []})";
    auto result = ParseChatEnvelope(raw);
    REQUIRE(std::holds_alternative<ParsedChatEnvelope>(result));
    const auto& env = std::get<ParsedChatEnvelope>(result);
    CHECK(env.utterance == "hi");
    CHECK(env.side_effects.empty());
}

TEST_CASE("ParseChatEnvelope rejects missing utterance") {
    const std::string raw = R"({"side_effects": []})";
    auto result = ParseChatEnvelope(raw);
    REQUIRE(std::holds_alternative<ParseError>(result));
    CHECK(std::get<ParseError>(result).message.find("utterance") != std::string::npos);
}

TEST_CASE("ParseChatEnvelope rejects utterance not a string") {
    const std::string raw = R"({"utterance": 42, "side_effects": []})";
    auto result = ParseChatEnvelope(raw);
    REQUIRE(std::holds_alternative<ParseError>(result));
}

TEST_CASE("ParseChatEnvelope rejects side_effects not an array") {
    const std::string raw = R"({"utterance": "hi", "side_effects": {}})";
    auto result = ParseChatEnvelope(raw);
    REQUIRE(std::holds_alternative<ParseError>(result));
    CHECK(std::get<ParseError>(result).message.find("side_effects") != std::string::npos);
}

TEST_CASE("ParseChatEnvelope rejects malformed JSON") {
    auto result = ParseChatEnvelope("not json");
    REQUIRE(std::holds_alternative<ParseError>(result));
}

TEST_CASE("ParseChatEnvelope rejects side_effect element failing inner schema") {
    // accept_party_invite without 'from' field
    const std::string raw = R"({
        "utterance": "ok",
        "side_effects": [{"name": "accept_party_invite", "arguments": {}}]
    })";
    auto result = ParseChatEnvelope(raw);
    REQUIRE(std::holds_alternative<ParseError>(result));
    CHECK(std::get<ParseError>(result).message.find("from") != std::string::npos);
}
```

- [ ] **Step 2: Add test + impl source to `tests/llmagent/CMakeLists.txt`**

Append `test_chat_envelope_parser.cpp` and `${LLMAGENT_DIR}/Schemas/ChatEnvelope.cpp`.

- [ ] **Step 3: Run — expect FAIL**

```bash
cmake --build build/llmagent_tests 2>&1 | tail -5
```

Expected: cmake "Cannot find source file: .../Schemas/ChatEnvelope.cpp".

- [ ] **Step 4: Write `src/Bot/LlmAgent/Schemas/ChatEnvelope.h`**

```cpp
#ifndef _PLAYERBOT_LLMAGENT_CHAT_ENVELOPE_H
#define _PLAYERBOT_LLMAGENT_CHAT_ENVELOPE_H

#include "Schemas/Goal.h"          // for ParseError
#include "Tools/ToolCatalog.h"     // for ParsedToolCall
#include <string>
#include <variant>
#include <vector>

struct ParsedChatEnvelope {
    std::string                  utterance;
    std::vector<ParsedToolCall>  side_effects;
};

std::variant<ParsedChatEnvelope, ParseError>
ParseChatEnvelope(const std::string& raw_json);

#endif  // _PLAYERBOT_LLMAGENT_CHAT_ENVELOPE_H
```

- [ ] **Step 5: Write `src/Bot/LlmAgent/Schemas/ChatEnvelope.cpp`**

```cpp
#include "Schemas/ChatEnvelope.h"
#include "Vendor/nlohmann_json.hpp"

std::variant<ParsedChatEnvelope, ParseError>
ParseChatEnvelope(const std::string& raw_json) {
    nlohmann::json j;
    try { j = nlohmann::json::parse(raw_json); }
    catch (const std::exception& e) {
        return ParseError{std::string{"top-level parse: "} + e.what()};
    }
    if (!j.is_object()) return ParseError{"envelope: top-level value is not an object"};

    if (!j.contains("utterance"))       return ParseError{"envelope: missing 'utterance'"};
    if (!j["utterance"].is_string())    return ParseError{"envelope: 'utterance' must be a string"};
    if (!j.contains("side_effects"))    return ParseError{"envelope: missing 'side_effects'"};
    if (!j["side_effects"].is_array())  return ParseError{"envelope: 'side_effects' must be an array"};

    ParsedChatEnvelope out;
    out.utterance = j["utterance"].get<std::string>();

    // Reuse ParseToolCalls by re-serializing the side_effects array — keeps
    // the per-tool inner-shape validation in one place.
    std::string sfx_raw = j["side_effects"].dump();
    auto parsed = ParseToolCalls(sfx_raw);
    if (std::holds_alternative<ParseError>(parsed))
        return ParseError{std::string{"side_effects: "} + std::get<ParseError>(parsed).message};
    out.side_effects = std::move(std::get<std::vector<ParsedToolCall>>(parsed));
    return out;
}
```

- [ ] **Step 6: Build + run — expect PASS**

```bash
cmake --build build/llmagent_tests && ./build/llmagent_tests/llmagent_unit_tests | tail -3
```

Expected: 149 + 7 = 156 cases pass.

- [ ] **Step 7: Commit**

```bash
git add src/Bot/LlmAgent/Schemas/ChatEnvelope.h \
        src/Bot/LlmAgent/Schemas/ChatEnvelope.cpp \
        tests/llmagent/test_chat_envelope_parser.cpp \
        tests/llmagent/CMakeLists.txt
git commit -m "feat(llm-agent): ParsedChatEnvelope + ParseChatEnvelope

POD {utterance, side_effects: vector<ParsedToolCall>} + free parser
returning variant<ParsedChatEnvelope, ParseError>. Reuses Phase 4's
ParseToolCalls for the side_effects array — one source of truth for
per-tool argument validation."
```

---

## Task 5: `LlmCounters` Phase 5 buckets

**Files:**
- Modify: `src/Bot/LlmAgent/Telemetry/LlmCounters.h`
- Modify: `src/Bot/LlmAgent/Telemetry/LlmCounters.cpp`
- Modify: `tests/llmagent/test_counters.cpp`

- [ ] **Step 1: Append 3 cases to `tests/llmagent/test_counters.cpp`**

```cpp
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
```

- [ ] **Step 2: Run — expect FAIL**

```bash
cmake --build build/llmagent_tests 2>&1 | tail -10
```

Expected: undeclared `IncChatEnvelopeParsed`, missing `Snapshot_t.chat_*` fields.

- [ ] **Step 3: Extend `src/Bot/LlmAgent/Telemetry/LlmCounters.h`**

Inside `struct Snapshot_t`, append:

```cpp
        // Phase 5: T3 chat brain.
        std::unordered_map<std::string, uint64_t> chat_envelope_parsed;  // "ok" | "schema_error" | "missing_utterance"
        std::unordered_map<std::string, uint64_t> chat_event_kind;       // "whisper" | "invite" | "join"
        uint64_t chat_utterances_queued = 0;
        uint64_t chat_sender_offline    = 0;
```

Inside `class LlmCounters` public section, append:

```cpp
    void IncChatEnvelopeParsed(const std::string& status);
    void IncChatEventKind(const std::string& kind);
    void IncChatUtterancesQueued();
    void IncChatSenderOffline();
```

Inside the private section, append:

```cpp
    mutable std::mutex                             chat_mu_;
    std::unordered_map<std::string, uint64_t>      chat_envelope_parsed_;
    std::unordered_map<std::string, uint64_t>      chat_event_kind_;
    std::atomic<uint64_t>                          chat_utterances_queued_{0};
    std::atomic<uint64_t>                          chat_sender_offline_{0};
```

- [ ] **Step 4: Extend `src/Bot/LlmAgent/Telemetry/LlmCounters.cpp`**

Append the four methods:

```cpp
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
```

In `Snapshot()`, before `return s;`, append:

```cpp
    {
        std::lock_guard<std::mutex> g(chat_mu_);
        s.chat_envelope_parsed = chat_envelope_parsed_;
        s.chat_event_kind = chat_event_kind_;
    }
    s.chat_utterances_queued = chat_utterances_queued_.load(std::memory_order_relaxed);
    s.chat_sender_offline    = chat_sender_offline_.load(std::memory_order_relaxed);
```

In `DumpToLog()`, before the closing `}`, append:

```cpp
    out << " chat_utterances_queued=" << s.chat_utterances_queued
        << " chat_sender_offline="    << s.chat_sender_offline;
    for (const auto& [k, v] : s.chat_envelope_parsed)
        out << " chat_envelope[" << k << "]=" << v;
    for (const auto& [k, v] : s.chat_event_kind)
        out << " chat_event_kind[" << k << "]=" << v;
```

- [ ] **Step 5: Build + run — expect PASS**

```bash
cmake --build build/llmagent_tests && ./build/llmagent_tests/llmagent_unit_tests | tail -3
```

Expected: 156 + 3 = 159 cases pass.

- [ ] **Step 6: Commit**

```bash
git add src/Bot/LlmAgent/Telemetry/LlmCounters.h \
        src/Bot/LlmAgent/Telemetry/LlmCounters.cpp \
        tests/llmagent/test_counters.cpp
git commit -m "feat(llm-agent): LlmCounters Phase 5 chat buckets

chat_envelope_parsed[status], chat_event_kind[kind],
chat_utterances_queued, chat_sender_offline. DumpToLog extended
to include the new totals."
```

---

## Task 6: `LlmAgentConfig` Tier3 keys

**Files:**
- Modify: `src/Bot/LlmAgent/LlmAgentConfig.h`
- Modify: `src/Bot/LlmAgent/LlmAgentConfig.cpp`
- Modify: `tests/llmagent/test_config_load.cpp`

- [ ] **Step 1: Append 2 cases to `tests/llmagent/test_config_load.cpp`**

```cpp
TEST_CASE("LlmAgentConfig Tier3 defaults") {
    StubConfigSource src;
    LlmAgentConfig cfg = LoadLlmAgentConfig(src);
    CHECK(cfg.Tier3_Enabled == true);
    CHECK(cfg.Tier3_CooldownMs == 5000u);
    CHECK(cfg.Tier3_DialogueHistorySize == 6u);
    CHECK(cfg.Tier3_WhisperWindowSeconds == 600u);
    CHECK(cfg.Tier3_MaxUtteranceChars == 200u);
    CHECK(cfg.Tier3_SystemPromptSuffix.empty());
    CHECK(cfg.Tier3_PersonaCacheTtlSeconds == 600u);
    CHECK(!cfg.Tier3_BuiltInSystemPromptSuffix.empty());
}

TEST_CASE("LlmAgentConfig Tier3 overrides applied") {
    StubConfigSource src;
    src.values["AiPlayerbot.LlmAgent.Tier3.Enabled"] = "0";
    src.values["AiPlayerbot.LlmAgent.Tier3.CooldownMs"] = "1500";
    src.values["AiPlayerbot.LlmAgent.Tier3.DialogueHistorySize"] = "10";
    src.values["AiPlayerbot.LlmAgent.Tier3.WhisperWindowSeconds"] = "300";
    src.values["AiPlayerbot.LlmAgent.Tier3.MaxUtteranceChars"] = "120";
    src.values["AiPlayerbot.LlmAgent.Tier3.SystemPromptSuffix"] = "custom suffix";
    src.values["AiPlayerbot.LlmAgent.Tier3.PersonaCacheTtlSeconds"] = "60";

    LlmAgentConfig cfg = LoadLlmAgentConfig(src);
    CHECK(cfg.Tier3_Enabled == false);
    CHECK(cfg.Tier3_CooldownMs == 1500u);
    CHECK(cfg.Tier3_DialogueHistorySize == 10u);
    CHECK(cfg.Tier3_WhisperWindowSeconds == 300u);
    CHECK(cfg.Tier3_MaxUtteranceChars == 120u);
    CHECK(cfg.Tier3_SystemPromptSuffix == "custom suffix");
    CHECK(cfg.Tier3_PersonaCacheTtlSeconds == 60u);
}
```

- [ ] **Step 2: Run — expect FAIL**

```bash
cmake --build build/llmagent_tests 2>&1 | tail -10
```

Expected: undeclared `Tier3_*` fields.

- [ ] **Step 3: Extend `src/Bot/LlmAgent/LlmAgentConfig.h`**

In the `LlmAgentConfig` struct, after the Phase 4 Tier2 block, append:

```cpp
    // Phase 5 — Tier 3 chat brain
    bool        Tier3_Enabled                  = true;
    uint32_t    Tier3_CooldownMs               = 5000;     // per-bot nominal cooldown
    uint32_t    Tier3_DialogueHistorySize      = 6;        // last N whispers per (bot, sender)
    uint32_t    Tier3_WhisperWindowSeconds     = 600;      // age out dialogue at 10 min
    uint32_t    Tier3_MaxUtteranceChars        = 200;
    std::string Tier3_SystemPromptSuffix;                  // empty = use built-in
    uint32_t    Tier3_PersonaCacheTtlSeconds   = 600;
    // Built-in suffix appended when Tier3_SystemPromptSuffix is empty.
    // Initialized from kDefaultTier3SystemPromptSuffix in LoadLlmAgentConfig.
    std::string Tier3_BuiltInSystemPromptSuffix;
```

After the existing `extern const char* const kDefaultTier2SystemPrompt;`, add:

```cpp
extern const char* const kDefaultTier3SystemPromptSuffix;
```

Inside `LoadLlmAgentConfig` after the Phase 4 Tier2 loader lines, append:

```cpp
    cfg.Tier3_Enabled                = src.template Get<bool>       ("AiPlayerbot.LlmAgent.Tier3.Enabled",                true);
    cfg.Tier3_CooldownMs             = src.template Get<uint32_t>   ("AiPlayerbot.LlmAgent.Tier3.CooldownMs",             uint32_t{5000});
    cfg.Tier3_DialogueHistorySize    = src.template Get<uint32_t>   ("AiPlayerbot.LlmAgent.Tier3.DialogueHistorySize",    uint32_t{6});
    cfg.Tier3_WhisperWindowSeconds   = src.template Get<uint32_t>   ("AiPlayerbot.LlmAgent.Tier3.WhisperWindowSeconds",   uint32_t{600});
    cfg.Tier3_MaxUtteranceChars      = src.template Get<uint32_t>   ("AiPlayerbot.LlmAgent.Tier3.MaxUtteranceChars",      uint32_t{200});
    cfg.Tier3_SystemPromptSuffix     = src.template Get<std::string>("AiPlayerbot.LlmAgent.Tier3.SystemPromptSuffix",     std::string{});
    cfg.Tier3_PersonaCacheTtlSeconds = src.template Get<uint32_t>   ("AiPlayerbot.LlmAgent.Tier3.PersonaCacheTtlSeconds", uint32_t{600});
    cfg.Tier3_BuiltInSystemPromptSuffix = std::string{kDefaultTier3SystemPromptSuffix};
```

- [ ] **Step 4: Define `kDefaultTier3SystemPromptSuffix` in `src/Bot/LlmAgent/LlmAgentConfig.cpp`**

Append:

```cpp
const char* const kDefaultTier3SystemPromptSuffix =
    "You are this character. A real human player has whispered, invited, "
    "or joined your group. Reply with ONLY a JSON object: "
    "{\"utterance\": \"<what you say>\", \"side_effects\": [<0..N tool calls>]}. "
    "Utterance: in-character, <=200 chars, no markdown, no third-person narration. "
    "Side-effects: actions you take ALONGSIDE the utterance — accept_party_invite, "
    "leave_party, set_goal, memory.remember. "
    "If you say \"I'll come\" you MUST emit accept_party_invite. "
    "If you say \"let me finish first\" emit set_goal with the current quest. "
    "Never say one thing and do another. When no action fits, side_effects is [].";
```

- [ ] **Step 5: Build + run — expect PASS**

```bash
cmake --build build/llmagent_tests && ./build/llmagent_tests/llmagent_unit_tests | tail -3
```

Expected: 159 + 2 = 161 cases pass.

- [ ] **Step 6: Commit**

```bash
git add src/Bot/LlmAgent/LlmAgentConfig.h \
        src/Bot/LlmAgent/LlmAgentConfig.cpp \
        tests/llmagent/test_config_load.cpp
git commit -m "feat(llm-agent): Phase 5 config keys + Tier3 system prompt suffix

Seven Tier3.* config keys (Enabled, CooldownMs, DialogueHistorySize,
WhisperWindowSeconds, MaxUtteranceChars, SystemPromptSuffix,
PersonaCacheTtlSeconds). Default suffix tells the model the envelope
shape + 'never say one thing and do another' principle from parent
design §7.3."
```

---

## Task 7: `LlmAgentManager` Phase 5 extensions

**Files:**
- Modify: `src/Bot/LlmAgent/LlmAgentManager.h`
- Modify: `src/Bot/LlmAgent/LlmAgentManager.cpp`

Adds `whispers_` (WhisperBuffer) and `t3_cooldowns_` (separate cooldown map, mirroring Phase 4's `t2_cooldowns_`) plus accessors. No new unit tests — existing manager tests verify nothing is broken.

- [ ] **Step 1: Verify the test baseline**

```bash
./build/llmagent_tests/llmagent_unit_tests | tail -3
```

Expected: `161 | 161 passed | 0 failed`.

- [ ] **Step 2: Extend `src/Bot/LlmAgent/LlmAgentManager.h`**

After the existing `#include "EventBuffer/InteractionEventBuffer.h"` add:

```cpp
#include "Chat/WhisperBuffer.h"
```

In the public section of `class LlmAgentManager`, after `InteractionEventBuffer& Interactions()` add:

```cpp
    WhisperBuffer&         Whispers()         { return whispers_; }
    BotCooldownMap&        T3Cooldowns()      { return t3_cooldowns_; }
```

In the private section, after `BotCooldownMap t2_cooldowns_;` (if it exists; otherwise next to `cooldowns_`) add:

```cpp
    BotCooldownMap                                t3_cooldowns_;
    WhisperBuffer                                 whispers_;
```

(If Phase 4 added `t2_cooldowns_` somewhere; locate by `grep -n "t2_cooldowns_" src/Bot/LlmAgent/LlmAgentManager.h` and place `t3_cooldowns_` adjacent.)

- [ ] **Step 3: Extend `Shutdown()` in `src/Bot/LlmAgent/LlmAgentManager.cpp`**

Find the existing `interactions_.ClearAll();` line in `Shutdown()` and append:

```cpp
    whispers_.ClearAll();
    t3_cooldowns_.Clear();
```

- [ ] **Step 4: Build + run — expect PASS**

```bash
cmake --build build/llmagent_tests && ./build/llmagent_tests/llmagent_unit_tests | tail -3
```

Expected: 161 cases still pass.

- [ ] **Step 5: Commit**

```bash
git add src/Bot/LlmAgent/LlmAgentManager.h \
        src/Bot/LlmAgent/LlmAgentManager.cpp
git commit -m "feat(llm-agent): Manager exposes Whispers() + T3Cooldowns()

WhisperBuffer instance for per-(bot, sender) dialogue history.
Separate BotCooldownMap for T3 (mirrors Phase 4's t2_cooldowns_).
Shutdown clears both."
```

---

## Task 8: `Tier3_ChatBrain` digest + request builder

**Files:**
- Create: `src/Bot/LlmAgent/Tiers/Tier3_ChatBrain.h`
- Create: `src/Bot/LlmAgent/Tiers/Tier3_ChatBrain.cpp`

Worldserver-only (guarded by `#ifndef LLMAGENT_UNIT_TESTS`). No new unit tests — covered end-to-end by smoke.

- [ ] **Step 1: Write `src/Bot/LlmAgent/Tiers/Tier3_ChatBrain.h`**

```cpp
#ifndef _PLAYERBOT_LLMAGENT_TIER3_CHATBRAIN_H
#define _PLAYERBOT_LLMAGENT_TIER3_CHATBRAIN_H

#include "Vendor/nlohmann_json.hpp"
#include <cstdint>
#include <string>

#ifndef LLMAGENT_UNIT_TESTS
class PlayerbotAI;

namespace LlmAgentTier3 {

enum class EventKind : uint8_t { Whisper = 0, Invite = 1, Join = 2 };

struct ChatContext {
    EventKind   kind;
    std::string sender_name;
    uint64_t    sender_guid = 0;
    std::string sender_message;     // populated only for Whisper
};

// Returns the JSON user-message payload for a T3 LLM call.
nlohmann::json BuildT3Digest(PlayerbotAI* botAI, const ChatContext& ctx);

// Returns the OpenAI-shaped POST body (model, messages, response_format, ...).
std::string BuildT3RequestBody(PlayerbotAI* botAI, const ChatContext& ctx);

}  // namespace LlmAgentTier3
#endif  // LLMAGENT_UNIT_TESTS

#endif  // _PLAYERBOT_LLMAGENT_TIER3_CHATBRAIN_H
```

- [ ] **Step 2: Write `src/Bot/LlmAgent/Tiers/Tier3_ChatBrain.cpp`**

```cpp
#include "Tiers/Tier3_ChatBrain.h"

#ifndef LLMAGENT_UNIT_TESTS

#include "Tiers/Tier0_StateDigest.h"
#include "Tools/ToolCatalog.h"
#include "LlmAgentManager.h"
#include "Chat/PersonaCache.h"
#include "PlayerbotAI.h"
#include "Player.h"

#include <ctime>

namespace LlmAgentTier3 {

namespace {

const char* event_kind_str(EventKind k) {
    switch (k) {
        case EventKind::Whisper: return "whisper";
        case EventKind::Invite:  return "invite";
        case EventKind::Join:    return "join";
    }
    return "unknown";
}

}  // namespace

nlohmann::json BuildT3Digest(PlayerbotAI* botAI, const ChatContext& ctx) {
    LlmBotState state = SnapshotBot(botAI);
    nlohmann::json digest = BuildDigestJson(state);

    // Strip fields T3 doesn't need (mirrors Phase 4 T2 trim — keeps prompt small).
    digest.erase("quest_log");
    if (digest.contains("location") && digest["location"].contains("position"))
        digest["location"].erase("position");
    if (digest.contains("social") && digest["social"].contains("recent_whispers"))
        digest["social"].erase("recent_whispers");
    digest.erase("interaction_context");  // T2 field; T3 supplies its own

    digest["event_kind"]     = event_kind_str(ctx.kind);
    digest["sender_name"]    = ctx.sender_name;
    digest["sender_message"] = ctx.sender_message;

    auto& mgr = LlmAgentManager::Instance();
    const auto& cfg = mgr.Config();
    const uint64_t bot_guid = botAI->GetBot()->GetGUID().GetRawValue();
    const int64_t now = static_cast<int64_t>(time(nullptr));

    nlohmann::json history = nlohmann::json::array();
    auto entries = mgr.Whispers().SnapshotFor(
        bot_guid, ctx.sender_guid, now,
        cfg.Tier3_WhisperWindowSeconds, cfg.Tier3_DialogueHistorySize);
    for (const auto& e : entries) {
        history.push_back({
            {"direction", e.direction == WhisperEntry::Incoming ? "in" : "out"},
            {"text",      e.text},
            {"age_s",     now - e.ts}
        });
    }
    digest["dialogue_history"] = history;

    nlohmann::json mem_hints = nlohmann::json::array();
    if (!ctx.sender_name.empty()) {
        auto hints = mgr.MemoryClient().RecallAbout(
            bot_guid, ctx.sender_name, /*hops*/2, /*top_k*/3);
        for (const auto& h : hints) mem_hints.push_back(h);
    }
    digest["memory_about_sender"] = mem_hints;

    return digest;
}

std::string BuildT3RequestBody(PlayerbotAI* botAI, const ChatContext& ctx) {
    auto& mgr = LlmAgentManager::Instance();
    const auto& cfg = mgr.Config();
    const uint64_t bot_guid = botAI->GetBot()->GetGUID().GetRawValue();

    nlohmann::json digest = BuildT3Digest(botAI, ctx);

    std::string persona = mgr.Persona().Get(bot_guid);
    std::string suffix = cfg.Tier3_SystemPromptSuffix.empty()
        ? cfg.Tier3_BuiltInSystemPromptSuffix
        : cfg.Tier3_SystemPromptSuffix;
    std::string system_msg = persona.empty() ? suffix : (persona + "\n\n" + suffix);

    nlohmann::json body;
    body["model"]    = cfg.Model;
    body["messages"] = nlohmann::json::array();
    body["messages"].push_back({{"role", "system"}, {"content", system_msg}});
    body["messages"].push_back({{"role", "user"},   {"content", digest.dump()}});
    body["response_format"] = {
        {"type", "json_schema"},
        {"json_schema", {
            {"name", "chat_envelope"},
            {"schema", nlohmann::json::parse(kT3OutputSchema)}
        }}
    };
    body["temperature"] = 0.7;
    body["max_tokens"]  = 300;
    return body.dump();
}

}  // namespace LlmAgentTier3

#endif  // LLMAGENT_UNIT_TESTS
```

**Note:** this file references `mgr.Persona()` which is added in Task 10 (alongside the rest of the Manager's PersonaCache wiring). The file will fail to link in the worldserver build until Task 10 lands. The unit-test build is unaffected (this .cpp is guarded). That mirrors Phase 4 Task 9's forward-reference pattern.

- [ ] **Step 3: Build unit tests — expect PASS at 161 cases (unchanged)**

```bash
cmake --build build/llmagent_tests && ./build/llmagent_tests/llmagent_unit_tests | tail -3
```

- [ ] **Step 4: Commit**

```bash
git add src/Bot/LlmAgent/Tiers/Tier3_ChatBrain.h \
        src/Bot/LlmAgent/Tiers/Tier3_ChatBrain.cpp
git commit -m "feat(llm-agent): Tier3_ChatBrain digest + request builder

BuildT3Digest enriches Tier0's digest with event_kind, sender info,
dialogue_history (from WhisperBuffer), and memory_about_sender (from
RecallAbout). BuildT3RequestBody assembles the OpenAI-shaped body with
persona-in-system, digest-in-user, response_format=json_schema bound
to kT3OutputSchema. temperature=0.7, max_tokens=300.

Forward reference: mgr.Persona() — landed in Task 10."
```

---

## Task 9: `LlmAgentHooks` — push to `WhisperBuffer` on incoming whisper

**Files:**
- Modify: `src/Bot/LlmAgent/Hooks/LlmAgentHooks.cpp`

Phase 4's `OnWhisperReceived` already pushes to `InteractionEventBuffer`. Phase 5 additionally pushes to `WhisperBuffer` so dialogue history persists across calls.

- [ ] **Step 1: Inspect existing hook to find the insertion point**

```bash
grep -n "PushWhisper\|OnWhisperReceived" src/Bot/LlmAgent/Hooks/LlmAgentHooks.cpp | head
```

Find the line that calls `mgr.Interactions().PushWhisper(...)` inside the `#ifndef LLMAGENT_UNIT_TESTS` block of `OnWhisperReceived`.

- [ ] **Step 2: Insert WhisperBuffer push right after the Interactions push**

After:

```cpp
    mgr.Interactions().PushWhisper(
        bot_guid, sender->GetName(), sender->GetGUID().GetRawValue(),
        truncate_whisper(text), static_cast<int64_t>(time(nullptr)));
```

Add:

```cpp
    mgr.Whispers().Push(
        bot_guid, sender->GetGUID().GetRawValue(),
        WhisperEntry::Incoming,
        truncate_whisper(text),
        static_cast<int64_t>(time(nullptr)));
```

At the top of the file (after the other `#include`s), add:

```cpp
#include "Chat/WhisperBuffer.h"
```

(Likely already pulled in transitively via `LlmAgentManager.h` after Task 7, but adding the direct include keeps the file self-documenting.)

- [ ] **Step 3: Build unit tests — expect PASS at 161 cases (unchanged)**

```bash
cmake --build build/llmagent_tests && ./build/llmagent_tests/llmagent_unit_tests | tail -3
```

- [ ] **Step 4: Commit**

```bash
git add src/Bot/LlmAgent/Hooks/LlmAgentHooks.cpp
git commit -m "feat(llm-agent): OnWhisperReceived pushes to WhisperBuffer

Each incoming whisper now lands in both the InteractionEventBuffer
(short-lived 'pending event' surface for the trigger) and the
WhisperBuffer (rolling dialogue history for T3 context)."
```

---

## Task 10: `LlmChatTrigger` + `LlmChatAction` + Manager PersonaCache wiring + contexts

**Files:**
- Create: `src/Bot/LlmAgent/Triggers/LlmChatTrigger.h`
- Create: `src/Bot/LlmAgent/Triggers/LlmChatTrigger.cpp`
- Create: `src/Bot/LlmAgent/Actions/LlmChatAction.h`
- Create: `src/Bot/LlmAgent/Actions/LlmChatAction.cpp`
- Create: `src/Bot/LlmAgent/Context/LlmAgentTier3TriggerContext.h`
- Create: `src/Bot/LlmAgent/Context/LlmAgentTier3ActionContext.h`
- Modify: `src/Bot/LlmAgent/LlmAgentManager.h`
- Modify: `src/Bot/LlmAgent/LlmAgentManager.cpp`

Worldserver-only. No new unit tests.

- [ ] **Step 1: Wire `PersonaCache` into `LlmAgentManager`**

Add to `LlmAgentManager.h` includes:

```cpp
#include "Chat/PersonaCache.h"
```

In the public section, after `T3Cooldowns()`:

```cpp
    PersonaCache&          Persona()          { return *persona_; }
```

In the private section, after `whispers_`:

```cpp
    std::unique_ptr<PersonaCache>                 persona_;
```

In `LlmAgentManager::Start` (or wherever `memory_client_` is constructed), add right after `memory_client_` initialization:

```cpp
#ifndef LLMAGENT_UNIT_TESTS
    persona_ = std::make_unique<PersonaCache>(
        [this](uint64_t guid) -> std::optional<std::string> {
            return memory_client_->GetPersonality(guid);
        },
        static_cast<int64_t>(cfg_.Tier3_PersonaCacheTtlSeconds),
        []{ return static_cast<int64_t>(time(nullptr)); });
#endif
```

In `Shutdown()`, after `whispers_.ClearAll();`:

```cpp
    if (persona_) persona_->ClearAll();
```

- [ ] **Step 2: Write `src/Bot/LlmAgent/Triggers/LlmChatTrigger.h`**

```cpp
#ifndef _PLAYERBOT_LLM_CHAT_TRIGGER_H
#define _PLAYERBOT_LLM_CHAT_TRIGGER_H

#include "Trigger.h"

class LlmChatTrigger : public Trigger {
  public:
    LlmChatTrigger(PlayerbotAI* ai) : Trigger(ai, "llm chat") {}
    bool IsActive() override;
};

#endif
```

- [ ] **Step 3: Write `src/Bot/LlmAgent/Triggers/LlmChatTrigger.cpp`**

```cpp
#include "Triggers/LlmChatTrigger.h"
#include "LlmAgentManager.h"
#include "PlayerbotAI.h"
#include "Player.h"

#include <ctime>

bool LlmChatTrigger::IsActive() {
    if (!botAI) return false;
    auto& mgr = LlmAgentManager::Instance();
    const auto& cfg = mgr.Config();
    if (!mgr.Enabled() || !cfg.Tier3_Enabled) return false;
    Player* bot = botAI->GetBot();
    if (!bot) return false;
    const uint64_t guid = bot->GetGUID().GetRawValue();

    // Drain side: T3 results waiting.
    if (mgr.HasPendingResults(guid, /*tier*/3)) return true;

    // Enqueue side: need pending interaction + LLM-enabled + cooldown clear + not in-flight.
    if (!mgr.Selector().IsLlmBot(guid)) return false;
    if (!mgr.T3Cooldowns().Eligible(guid)) return false;
    if (mgr.IsInFlight(guid, /*tier*/3)) return false;

    mgr.Interactions().ExpireOlderThan(
        guid, static_cast<int64_t>(time(nullptr)),
        cfg.Tier3_WhisperWindowSeconds);
    return mgr.Interactions().HasPending(guid);
}
```

- [ ] **Step 4: Write `src/Bot/LlmAgent/Actions/LlmChatAction.h`**

```cpp
#ifndef _PLAYERBOT_LLM_CHAT_ACTION_H
#define _PLAYERBOT_LLM_CHAT_ACTION_H

#include "Action.h"

class LlmChatAction : public Action {
  public:
    LlmChatAction(PlayerbotAI* ai) : Action(ai, "llm chat") {}
    bool Execute(Event event) override;
};

#endif
```

- [ ] **Step 5: Write `src/Bot/LlmAgent/Actions/LlmChatAction.cpp`**

```cpp
#include "Actions/LlmChatAction.h"
#include "LlmAgentManager.h"
#include "Schemas/ChatEnvelope.h"
#include "Tools/ToolValidators.h"
#include "Tools/ToolExecutors.h"
#include "Tools/InteractionContext.h"
#include "Tiers/Tier3_ChatBrain.h"
#include "Mgr/Text/PlayerbotTextMgr.h"   // for ChatQueuedReply
#include "PlayerbotAI.h"
#include "Player.h"
#include "Group.h"
#include "ObjectAccessor.h"
#include "Log.h"
#include "SharedDefines.h"               // CHAT_MSG_WHISPER / CHAT_MSG_PARTY

#include <algorithm>
#include <chrono>
#include <ctime>
#include <type_traits>
#include <variant>

namespace {

using LlmAgentTier3::ChatContext;
using LlmAgentTier3::EventKind;

const char* event_kind_str(EventKind k) {
    switch (k) {
        case EventKind::Whisper: return "whisper";
        case EventKind::Invite:  return "invite";
        case EventKind::Join:    return "join";
    }
    return "unknown";
}

// Picks the highest-priority pending interaction for this bot and converts
// it into a ChatContext for T3. Priority: whisper > invite > join (whisper
// is the most direct human signal; join is the most ambient).
bool BuildChatContext(uint64_t bot_guid, ChatContext& out) {
    auto& mgr = LlmAgentManager::Instance();
    auto payload = mgr.Interactions().SnapshotFor(bot_guid);
    if (!payload.recent_whispers.empty()) {
        const auto& w = payload.recent_whispers.front();
        out.kind = EventKind::Whisper;
        out.sender_name = w.from_name;
        out.sender_guid = w.from_guid;
        out.sender_message = w.text;
        return true;
    }
    if (!payload.pending_invites.empty()) {
        const auto& i = payload.pending_invites.front();
        out.kind = EventKind::Invite;
        out.sender_name = i.from_name;
        out.sender_guid = i.from_guid;
        out.sender_message.clear();
        return true;
    }
    if (!payload.recent_group_joins.empty()) {
        const auto& j = payload.recent_group_joins.front();
        out.kind = EventKind::Join;
        out.sender_name = j.leader_name;
        out.sender_guid = j.leader_guid;
        out.sender_message.clear();
        return true;
    }
    return false;
}

const char* tool_call_name(const ParsedToolCall& c) {
    return std::visit([](auto const& t) -> const char* {
        using T = std::decay_t<decltype(t)>;
        if      constexpr (std::is_same_v<T, AcceptPartyInviteCall>) return "accept_party_invite";
        else if constexpr (std::is_same_v<T, LeavePartyCall>)        return "leave_party";
        else if constexpr (std::is_same_v<T, AcceptQuestCall>)       return "accept_quest";
        else if constexpr (std::is_same_v<T, TurnInQuestCall>)       return "turn_in_quest";
        else if constexpr (std::is_same_v<T, SetGoalCall>)           return "set_goal";
        else if constexpr (std::is_same_v<T, VendorJunkCall>)        return "vendor_junk";
        else if constexpr (std::is_same_v<T, MemoryRememberCall>)    return "memory.remember";
        else                                                          return "unknown";
    }, c);
}

void QueueUtterance(PlayerbotAI* botAI, const std::string& utterance,
                    const ChatContext& ctx) {
    uint32 chat_type = (ctx.kind == EventKind::Join)
        ? CHAT_MSG_PARTY
        : CHAT_MSG_WHISPER;
    uint32 guid1 = static_cast<uint32>(ctx.sender_guid & 0xFFFFFFFFULL);
    ChatQueuedReply reply(
        chat_type,
        guid1,
        /*guid2*/0,
        utterance,
        /*chanName*/std::string{},
        ctx.sender_name,
        static_cast<time_t>(time(nullptr)));
    botAI->QueueChatResponse(reply);
}

}  // namespace

bool LlmChatAction::Execute(Event /*event*/) {
    if (!botAI) return false;
    auto& mgr = LlmAgentManager::Instance();
    const auto& cfg = mgr.Config();
    if (!mgr.Enabled() || !cfg.Tier3_Enabled) return false;
    Player* bot = botAI->GetBot();
    if (!bot) return false;
    const uint64_t guid = bot->GetGUID().GetRawValue();

    bool applied_any = false;

    // ===== Drain phase =====
    auto results = mgr.DrainResults(guid, /*tier*/3);
    for (const auto& r : results) {
        if (r.parsed_status != "ok") {
            mgr.Counters().IncFallbackUsed();
            mgr.T3Cooldowns().Set(guid,
                std::chrono::steady_clock::now() +
                std::chrono::milliseconds(cfg.FallbackCooldownMs));
            continue;
        }
        auto parsed = ParseChatEnvelope(r.raw_response);
        if (std::holds_alternative<ParseError>(parsed)) {
            mgr.Counters().IncChatEnvelopeParsed("schema_error");
            mgr.T3Cooldowns().Set(guid,
                std::chrono::steady_clock::now() +
                std::chrono::milliseconds(cfg.FallbackCooldownMs));
            continue;
        }
        const auto& env = std::get<ParsedChatEnvelope>(parsed);
        mgr.Counters().IncChatEnvelopeParsed("ok");

        // Determine ChatContext from the most recent interaction (matches the
        // event that caused the original enqueue — buffer wasn't cleared yet).
        ChatContext ctx;
        if (!BuildChatContext(guid, ctx)) {
            // No pending interaction anymore (cleared between enqueue and result).
            // Drop the utterance — we don't know where to send it.
            mgr.Counters().IncChatSenderOffline();
            mgr.Interactions().Clear(guid);
            continue;
        }

        // Apply side_effects (Phase 4 path, unchanged).
        InteractionContext vctx = SnapshotInteractionContext(botAI);
        const uint32_t cap = static_cast<uint32_t>(env.side_effects.size());
        for (uint32_t i = 0; i < cap; ++i) {
            const auto& call = env.side_effects[i];
            mgr.Counters().IncToolReceived(tool_call_name(call));
            auto decision = std::visit([&](auto const& t){ return Validate(t, vctx); }, call);
            if (!decision.accepted) {
                mgr.Counters().IncToolRejected(decision.reject_reason);
                if (!std::holds_alternative<MemoryRememberCall>(call)) break;
                continue;
            }
            try {
                bool ok = LlmAgentTools::ApplyToolCall(call, botAI);
                if (ok) {
                    mgr.Counters().IncToolApplied(tool_call_name(call));
                    applied_any = true;
                } else {
                    mgr.Counters().IncToolThrew(tool_call_name(call));
                    break;
                }
            } catch (...) {
                mgr.Counters().IncToolThrew(tool_call_name(call));
                break;
            }
        }

        // Queue the utterance.
        if (!env.utterance.empty()) {
            try {
                QueueUtterance(botAI, env.utterance, ctx);
                mgr.Counters().IncChatUtterancesQueued();
                mgr.Whispers().Push(
                    guid, ctx.sender_guid,
                    WhisperEntry::Outgoing,
                    env.utterance,
                    static_cast<int64_t>(time(nullptr)));
            } catch (...) {
                LOG_ERROR("playerbots", "[LlmAgent] QueueUtterance threw for bot {}", guid);
            }
        }

        // Consume the interaction. Set a short nominal cooldown.
        mgr.Interactions().Clear(guid);
        mgr.T3Cooldowns().Set(guid,
            std::chrono::steady_clock::now() +
            std::chrono::milliseconds(cfg.Tier3_CooldownMs));
    }

    // ===== Enqueue phase =====
    if (!applied_any &&
        mgr.Selector().IsLlmBot(guid) &&
        mgr.T3Cooldowns().Eligible(guid) &&
        !mgr.IsInFlight(guid, /*tier*/3) &&
        mgr.Interactions().HasPending(guid))
    {
        ChatContext ctx;
        if (BuildChatContext(guid, ctx)) {
            mgr.Counters().IncChatEventKind(event_kind_str(ctx.kind));
            LlmRequest req;
            req.bot_guid    = guid;
            req.bot_name    = bot->GetName();
            req.body_json   = LlmAgentTier3::BuildT3RequestBody(botAI, ctx);
            req.digest_json = LlmAgentTier3::BuildT3Digest(botAI, ctx);
            req.tier        = 3;
            mgr.Enqueue(std::move(req));
        }
    }

    return applied_any;
}
```

- [ ] **Step 6: Write `src/Bot/LlmAgent/Context/LlmAgentTier3TriggerContext.h`**

```cpp
#ifndef _PLAYERBOT_LLMAGENT_TIER3_TRIGGER_CONTEXT_H
#define _PLAYERBOT_LLMAGENT_TIER3_TRIGGER_CONTEXT_H

#include "NamedObjectContext.h"
#include "Triggers/LlmChatTrigger.h"

class LlmAgentTier3TriggerContext : public NamedObjectContext<Trigger> {
  public:
    LlmAgentTier3TriggerContext() {
        creators["llm chat"] = &LlmAgentTier3TriggerContext::llm_chat;
    }
  private:
    static Trigger* llm_chat(PlayerbotAI* ai) { return new LlmChatTrigger(ai); }
};

#endif
```

- [ ] **Step 7: Write `src/Bot/LlmAgent/Context/LlmAgentTier3ActionContext.h`**

```cpp
#ifndef _PLAYERBOT_LLMAGENT_TIER3_ACTION_CONTEXT_H
#define _PLAYERBOT_LLMAGENT_TIER3_ACTION_CONTEXT_H

#include "NamedObjectContext.h"
#include "Actions/LlmChatAction.h"

class LlmAgentTier3ActionContext : public NamedObjectContext<Action> {
  public:
    LlmAgentTier3ActionContext() {
        creators["llm chat"] = &LlmAgentTier3ActionContext::llm_chat;
    }
  private:
    static Action* llm_chat(PlayerbotAI* ai) { return new LlmChatAction(ai); }
};

#endif
```

- [ ] **Step 8: Build unit tests — expect PASS at 161 (no test changes, but the .h's transitively included don't break the build)**

```bash
cmake --build build/llmagent_tests && ./build/llmagent_tests/llmagent_unit_tests | tail -3
```

- [ ] **Step 9: Commit**

```bash
git add src/Bot/LlmAgent/Triggers/LlmChatTrigger.h \
        src/Bot/LlmAgent/Triggers/LlmChatTrigger.cpp \
        src/Bot/LlmAgent/Actions/LlmChatAction.h \
        src/Bot/LlmAgent/Actions/LlmChatAction.cpp \
        src/Bot/LlmAgent/Context/LlmAgentTier3TriggerContext.h \
        src/Bot/LlmAgent/Context/LlmAgentTier3ActionContext.h \
        src/Bot/LlmAgent/LlmAgentManager.h \
        src/Bot/LlmAgent/LlmAgentManager.cpp
git commit -m "feat(llm-agent): LlmChatTrigger + LlmChatAction + Tier3 contexts + Persona

LlmChatTrigger fires on HasPending OR HasPendingResults(g, 3); guarded
by Tier3.Enabled + IsLlmBot + T3Cooldowns + !IsInFlight.

LlmChatAction:
  - Drains tier=3 results
  - ParseChatEnvelope; falls back silent on error (per spec §9)
  - Applies side_effects through Phase 4's Validate/ApplyToolCall path
  - QueueChatResponse with channel routed per event_kind (whisper events
    → whisper back; invite events → whisper inviter; join events → party)
  - Pushes outgoing utterance to WhisperBuffer
  - Sets 5s nominal cooldown on success, 5min fallback on parse/transport failure

Manager now owns PersonaCache via std::unique_ptr; constructed in Start()
with a fetcher closure over MemoryHttpClient::GetPersonality, TTL from
Tier3.PersonaCacheTtlSeconds."
```

---

## Task 11: Strategy + Engine context registration

**Files:**
- Modify: `src/Bot/LlmAgent/Strategy/LlmAgentStrategy.cpp`
- Modify: `src/Bot/Engine/BuildSharedActionContexts.cpp`
- Modify: `src/Bot/Engine/BuildSharedTriggerContexts.cpp`

- [ ] **Step 1: Replace T2 trigger node with T3 in `LlmAgentStrategy.cpp`**

Find the Phase 4 push_back for `"llm interact"`. Replace:

```cpp
    triggers.push_back(
        new TriggerNode(
            "llm interact",
            {NextAction("llm interact", 16.0f)}
        )
    );
```

With:

```cpp
    triggers.push_back(
        new TriggerNode(
            "llm chat",
            {NextAction("llm chat", 16.0f)}
        )
    );
```

The Phase 1 `"llm replan idle"` push_back stays untouched.

- [ ] **Step 2: Swap T2 → T3 action context in `BuildSharedActionContexts.cpp`**

Replace:

```cpp
#include "Bot/LlmAgent/Context/LlmAgentTier2ActionContext.h"
```

with:

```cpp
#include "Bot/LlmAgent/Context/LlmAgentTier3ActionContext.h"
```

Replace `actionContexts.Add(new LlmAgentTier2ActionContext());` with `actionContexts.Add(new LlmAgentTier3ActionContext());`. The Phase 1 `LlmAgentActionContext` line stays untouched.

- [ ] **Step 3: Swap T2 → T3 trigger context in `BuildSharedTriggerContexts.cpp`**

Replace:

```cpp
#include "Bot/LlmAgent/Context/LlmAgentTier2TriggerContext.h"
```

with:

```cpp
#include "Bot/LlmAgent/Context/LlmAgentTier3TriggerContext.h"
```

Replace `triggerContexts.Add(new LlmAgentTier2TriggerContext());` with `triggerContexts.Add(new LlmAgentTier3TriggerContext());`.

- [ ] **Step 4: Build unit tests — expect PASS at 161 (engine context files aren't in test build)**

```bash
cmake --build build/llmagent_tests && ./build/llmagent_tests/llmagent_unit_tests | tail -3
```

- [ ] **Step 5: Commit**

```bash
git add src/Bot/LlmAgent/Strategy/LlmAgentStrategy.cpp \
        src/Bot/Engine/BuildSharedActionContexts.cpp \
        src/Bot/Engine/BuildSharedTriggerContexts.cpp
git commit -m "feat(llm-agent): swap T2 trigger node for T3 chat in LlmAgentStrategy

Strategy unregisters 'llm interact' (Phase 4) and registers 'llm chat'
(Phase 5) at the same relevance 16.0. Engine context wiring swapped
from Tier2 to Tier3 contexts. Phase 4 trigger/action/context files
stay in tree as Phase 5.1 scaffold."
```

---

## Task 12: Admin command `.playerbots t3 inject_whisper`

**Files:**
- Modify: `src/Script/PlayerbotCommandScript.cpp`

Mirrors Phase 4 Task 11's `t2 inject_whisper`. Pushes a synthetic whisper into `InteractionEventBuffer` AND `WhisperBuffer` so we can drive T3 mechanically without a connected player.

- [ ] **Step 1: Add the include**

After Phase 4's `#include "Bot/LlmAgent/LlmAgentManager.h"`:

```cpp
#include "Bot/LlmAgent/Chat/WhisperBuffer.h"
```

- [ ] **Step 2: Add the T3 sub-table to `GetCommands()`**

Inside `playerbots_commandscript::GetCommands()`, near the existing `static ChatCommandTable playerbotsT2CommandTable = { ... };`, add:

```cpp
        static ChatCommandTable playerbotsT3CommandTable = {
            {"inject_whisper", HandleT3InjectWhisper, SEC_GAMEMASTER, Console::Yes},
        };
```

Inside `playerbotsCommandTable`, add the new sub-table entry (next to the existing `{"t2", playerbotsT2CommandTable},`):

```cpp
            {"t3", playerbotsT3CommandTable},
```

- [ ] **Step 3: Add the handler method**

Inside the class, next to `HandleT2InjectWhisper`:

```cpp
    static bool HandleT3InjectWhisper(ChatHandler* handler, char const* args)
    {
        if (!args || !*args)
        {
            handler->PSendSysMessage("usage: .playerbots t3 inject_whisper <bot_name> <text>");
            return true;
        }
        std::string a(args);
        auto sp = a.find(' ');
        if (sp == std::string::npos)
        {
            handler->PSendSysMessage("usage: .playerbots t3 inject_whisper <bot_name> <text>");
            return true;
        }
        std::string bot_name = a.substr(0, sp);
        std::string text     = a.substr(sp + 1);
        Player* bot = ObjectAccessor::FindPlayerByName(bot_name);
        if (!bot)
        {
            handler->PSendSysMessage("bot %s not found", bot_name.c_str());
            return true;
        }
        auto& mgr = LlmAgentManager::Instance();
        if (mgr.Config().SocialOptIn)
            mgr.Selector().OptInBot(bot->GetGUID().GetRawValue());

        std::string from_name = "GM";
        uint64_t    from_guid = 0;
        if (handler->GetSession())
        {
            from_name = handler->GetSession()->GetPlayerName();
            if (Player* p = handler->GetSession()->GetPlayer())
                from_guid = p->GetGUID().GetRawValue();
        }

        const int64_t now = static_cast<int64_t>(time(nullptr));
        mgr.Interactions().PushWhisper(
            bot->GetGUID().GetRawValue(),
            from_name, from_guid, text, now);
        mgr.Whispers().Push(
            bot->GetGUID().GetRawValue(),
            from_guid,
            WhisperEntry::Incoming,
            text, now);
        handler->PSendSysMessage("T3 whisper injected for %s", bot_name.c_str());
        return true;
    }
```

- [ ] **Step 4: Build unit tests — expect PASS at 161**

```bash
cmake --build build/llmagent_tests && ./build/llmagent_tests/llmagent_unit_tests | tail -3
```

- [ ] **Step 5: Commit**

```bash
git add src/Script/PlayerbotCommandScript.cpp
git commit -m "feat(llm-agent): .playerbots t3 inject_whisper admin command

Mirrors Phase 4 Task 11's t2 inject_whisper. Pushes a synthetic
whisper into both InteractionEventBuffer (so the trigger fires) AND
WhisperBuffer (so dialogue history is populated for T3 context).
SEC_GAMEMASTER; console-yes."
```

---

## Task 13: `conf/playerbots.conf.dist` Phase 5 keys

**Files:**
- Modify: `conf/playerbots.conf.dist`

- [ ] **Step 1: Append Phase 5 block**

Append after the existing Tier2 block:

```ini

####################################################################################################
# LLM AGENT (Phase 5 — T3 chat brain)
#
# T3 fires when a human whispers, invites, or joins-group-with an LLM-enabled
# bot. One LLM call returns {utterance, side_effects}: the bot says something
# in-character AND emits 0..N tool calls (accept_party_invite, set_goal,
# memory.remember, etc.) so it doesn't say "I'll come" then sit idle.
# Replaces Tier 2 on these three events — the Tier2 keys above are inert
# unless Tier3.Enabled = 0.

# Default: 1 (enabled when LlmAgent.Enabled=1)
AiPlayerbot.LlmAgent.Tier3.Enabled = 1

# Per-bot cooldown between T3 fires (ms). Short so conversation flows.
# Default: 5000 (5 s)
AiPlayerbot.LlmAgent.Tier3.CooldownMs = 5000

# Last N whispers per (bot, sender) pair kept in memory for dialogue context.
# Default: 6
AiPlayerbot.LlmAgent.Tier3.DialogueHistorySize = 6

# Whispers older than this drop from the dialogue history.
# Default: 600 (10 min)
AiPlayerbot.LlmAgent.Tier3.WhisperWindowSeconds = 600

# Cap on the model's utterance length (chars). The schema enforces this server-side.
# Default: 200
AiPlayerbot.LlmAgent.Tier3.MaxUtteranceChars = 200

# Override the default T3 system-prompt suffix (leave empty to use built-in).
AiPlayerbot.LlmAgent.Tier3.SystemPromptSuffix = ""

# In-process persona cache TTL (s). Bigger TTL = fewer sidecar hits.
# Default: 600 (10 min)
AiPlayerbot.LlmAgent.Tier3.PersonaCacheTtlSeconds = 600
```

- [ ] **Step 2: Commit**

```bash
git add conf/playerbots.conf.dist
git commit -m "feat(llm-agent): playerbots.conf.dist gets seven Tier3 keys

Tier3.{Enabled, CooldownMs, DialogueHistorySize, WhisperWindowSeconds,
MaxUtteranceChars, SystemPromptSuffix, PersonaCacheTtlSeconds}."
```

---

## Task 14: Heimdal build + T3 smoke test

**Files:**
- Create: `results/2026-05-14-llm-phase-5-smoke/summary.md`
- Create: `results/2026-05-14-llm-phase-5-smoke/sample_records.jsonl`

The build process and attach-pipeline pattern are proven from Phase 4 (commits `6629ab3f`, `c7f35ec7`, `b6abf92c`). Reuse them.

### 14.1 Build + deploy

- [ ] **Step 1: Push branch**

```bash
git push -u origin claude/llm-agent-phase-5-t3-chat-brain
```

- [ ] **Step 2: Bundle modified source files into an overlay tar**

```bash
cd /Users/tbrack/Documents/Projects/playerbots-dev/mod-playerbots
tar -cf /tmp/p5-source-overlay.tar \
  src/Bot/LlmAgent/Chat/WhisperBuffer.h \
  src/Bot/LlmAgent/Chat/WhisperBuffer.cpp \
  src/Bot/LlmAgent/Chat/PersonaCache.h \
  src/Bot/LlmAgent/Chat/PersonaCache.cpp \
  src/Bot/LlmAgent/Schemas/ChatEnvelope.h \
  src/Bot/LlmAgent/Schemas/ChatEnvelope.cpp \
  src/Bot/LlmAgent/Tools/ToolCatalog.h \
  src/Bot/LlmAgent/Tools/ToolCatalog.cpp \
  src/Bot/LlmAgent/Telemetry/LlmCounters.h \
  src/Bot/LlmAgent/Telemetry/LlmCounters.cpp \
  src/Bot/LlmAgent/LlmAgentConfig.h \
  src/Bot/LlmAgent/LlmAgentConfig.cpp \
  src/Bot/LlmAgent/LlmAgentManager.h \
  src/Bot/LlmAgent/LlmAgentManager.cpp \
  src/Bot/LlmAgent/Tiers/Tier3_ChatBrain.h \
  src/Bot/LlmAgent/Tiers/Tier3_ChatBrain.cpp \
  src/Bot/LlmAgent/Hooks/LlmAgentHooks.cpp \
  src/Bot/LlmAgent/Triggers/LlmChatTrigger.h \
  src/Bot/LlmAgent/Triggers/LlmChatTrigger.cpp \
  src/Bot/LlmAgent/Actions/LlmChatAction.h \
  src/Bot/LlmAgent/Actions/LlmChatAction.cpp \
  src/Bot/LlmAgent/Context/LlmAgentTier3TriggerContext.h \
  src/Bot/LlmAgent/Context/LlmAgentTier3ActionContext.h \
  src/Bot/LlmAgent/Strategy/LlmAgentStrategy.cpp \
  src/Bot/Engine/BuildSharedActionContexts.cpp \
  src/Bot/Engine/BuildSharedTriggerContexts.cpp \
  src/Script/PlayerbotCommandScript.cpp
scp /tmp/p5-source-overlay.tar heimdal:/tmp/
ssh heimdal chmod 644 /tmp/p5-source-overlay.tar
```

- [ ] **Step 3: Incremental rebuild inside builder image (using Phase 4's wow-builder-cache volume)**

```bash
ssh heimdal 'echo "101423" | sudo -S bash -c "
podman run --rm \
  -v /tmp/p5-source-overlay.tar:/tmp/overlay.tar:z \
  -v wow-builder-cache:/azerothcore/build:Z \
  --name p5-build \
  $(podman images --filter reference=\"<none>\" --format \"{{.Id}}\" | head -1) \
  bash -c \"
    set -e
    cd /azerothcore/modules/mod-playerbots/
    tar -xf /tmp/overlay.tar
    find src/Bot/LlmAgent/Chat src/Bot/LlmAgent/Tiers src/Bot/LlmAgent/Triggers src/Bot/LlmAgent/Actions src/Bot/LlmAgent/Schemas src/Bot/LlmAgent/Context -newer /tmp/overlay.tar -exec touch {} +
    touch src/Bot/LlmAgent/Tools/ToolCatalog.* src/Bot/LlmAgent/Telemetry/LlmCounters.* src/Bot/LlmAgent/LlmAgentConfig.* src/Bot/LlmAgent/LlmAgentManager.* src/Bot/LlmAgent/Hooks/LlmAgentHooks.cpp src/Bot/LlmAgent/Strategy/LlmAgentStrategy.cpp
    cd /azerothcore/build
    cmake --build . --target worldserver -j 17 2>&1 | tail -15
    cmake --install . 2>&1 | tail -3
    ls -la /azerothcore/env/dist/bin/worldserver
  \"
"
```

If the find/touch ordering misses any newly-created subdir (Chat/, Schemas/, Context/, Tiers/, Triggers/, Actions/), append explicit `touch src/Bot/LlmAgent/<subdir>/*.{h,cpp}` lines.

Expected: 100% Built target worldserver. If cmake reports "could not find source file" for a newly-created file, the CMakeLists.txt for mod-playerbots needs updating — find it via `find /azerothcore/modules/mod-playerbots -name CMakeLists.txt` and add the new sources (mod-playerbots typically auto-globs, but verify).

- [ ] **Step 4: Repackage into a runtime image**

```bash
ssh heimdal 'echo "101423" | sudo -S bash -c "
cp /var/lib/containers/storage/volumes/wow-builder-cache/_data/src/server/apps/worldserver /tmp/p4-build-ctx/worldserver
cd /tmp/p4-build-ctx
podman build -t localhost/wow-server:phase5-t3 . 2>&1 | tail -3
podman tag localhost/wow-server:phase5-t3 localhost/wow-server:current
"'
```

(`/tmp/p4-build-ctx` is left over from Phase 4 with a working Containerfile that COPYs the binary into the runtime layer. If it's missing, recreate it: a one-line `Containerfile`:
`FROM localhost/wow-server:phase4-tierdispatch` then `COPY worldserver /azerothcore/env/dist/bin/worldserver`.)

- [ ] **Step 5: Update Heimdal playerbots.conf with Phase 5 keys**

```bash
ssh heimdal 'echo "101423" | sudo -S bash -c "
cat >> /opt/containers/wow/etc/modules/playerbots.conf <<EOF

AiPlayerbot.LlmAgent.Enabled = 1
AiPlayerbot.LlmAgent.Tier3.Enabled = 1
AiPlayerbot.LlmAgent.Tier3.CooldownMs = 5000
AiPlayerbot.LlmAgent.Tier3.DialogueHistorySize = 6
AiPlayerbot.LlmAgent.Tier3.WhisperWindowSeconds = 600
AiPlayerbot.LlmAgent.Tier3.MaxUtteranceChars = 200
AiPlayerbot.LlmAgent.Tier3.PersonaCacheTtlSeconds = 600
EOF
"'
```

The other Phase 4 keys are already in place from the prior smoke. Verify `LlmAgent.Enabled = 1` (Phase 4 ended with it = 0).

- [ ] **Step 6: Clear old JSONL + restart worldserver**

```bash
ssh heimdal 'echo "101423" | sudo -S rm -f /opt/containers/wow/logs/llm_agent_phase4.jsonl && echo "101423" | sudo -S systemctl restart wow-worldserver.service'
```

(JSONL filename is still `llm_agent_phase4.jsonl` — it's the `JsonlPath` config key value; we can rename via config later. For now reuse the file path Phase 4 used.)

### 14.2 Smoke validation

- [ ] **Step 7: Re-establish attach pipeline (proven Phase 4 pattern)**

```bash
ssh heimdal 'echo "101423" | sudo -S rm -f /tmp/wow-stdin /tmp/wow-stdout; echo "101423" | sudo -S mkfifo /tmp/wow-stdin; echo "101423" | sudo -S chmod 666 /tmp/wow-stdin; echo "101423" | sudo -S nohup setsid bash -c "podman attach --sig-proxy=false wow-worldserver < /tmp/wow-stdin > /tmp/wow-stdout 2>&1 & PID=\$!; exec 99>/tmp/wow-stdin; wait \$PID" > /dev/null 2>&1 < /dev/null &'
sleep 3
ssh heimdal 'echo "101423" | sudo -S ps -ef | grep "podman attach.*wow-worldserver" | grep -v grep | head'
```

- [ ] **Step 8: Pick 10 fresh bot names + inject T3 whispers**

```bash
ssh heimdal 'until [ -s /opt/containers/wow/logs/llm_agent_phase4.jsonl ] 2>/dev/null || echo "101423" | sudo -S [ -s /opt/containers/wow/logs/llm_agent_phase4.jsonl ]; do sleep 5; done; echo READY; echo "101423" | sudo -S jq -r ".bot_name" /opt/containers/wow/logs/llm_agent_phase4.jsonl 2>&1 | sort -u | shuf | head -10 > /tmp/p5-bot-targets.txt; cat /tmp/p5-bot-targets.txt'
ssh heimdal 'echo "101423" | sudo -S bash -c "
for BOT in \$(cat /tmp/p5-bot-targets.txt); do
  echo \".playerbots t3 inject_whisper \$BOT want to group up at the inn?\" > /tmp/wow-stdin
  sleep 0.4
done
echo INJECTED_10
"'
```

- [ ] **Step 9: Poll for T3 ok records**

```bash
ssh heimdal 'echo "101423" | sudo -S bash -c "
for i in \$(seq 1 24); do
  T3=\$(jq -s \"[.[] | select(.tier == 3)] | length\" /opt/containers/wow/logs/llm_agent_phase4.jsonl 2>/dev/null)
  OK=\$(jq -s \"[.[] | select(.tier == 3 and .parsed_status == \\\"ok\\\")] | length\" /opt/containers/wow/logs/llm_agent_phase4.jsonl 2>/dev/null)
  echo \"\$(date +%H:%M:%S) T3=\$T3 ok=\$OK\"
  if [ \"\$OK\" -ge \"3\" ]; then break; fi
  sleep 15
done
"'
```

Expected: ≥3 records with `tier:3, parsed_status:"ok"` within ~3 minutes.

- [ ] **Step 10: Inspect one ok record**

```bash
ssh heimdal 'echo "101423" | sudo -S jq -c "select(.tier == 3 and .parsed_status == \"ok\") | {bot_name, event_kind, sender_name: (.digest.sender_name // \"-\"), inference_ms, raw: (.raw_response // \"-\")[:400]}" /opt/containers/wow/logs/llm_agent_phase4.jsonl 2>&1 | head -3'
```

Expected: a JSON envelope with `utterance` (non-empty) and `side_effects` (0..3 calls).

- [ ] **Step 11: Verify utterance actually dispatched (worldserver console)**

```bash
ssh heimdal 'echo "101423" | sudo -S grep -E "ChatReplyDo|whisper.*Sure|whisper.*sec" /tmp/wow-stdout 2>&1 | head -5'
```

If a real human were logged in, the whisper packets would have gone out. With 0 connected players this just confirms the chat-dispatch code path didn't crash.

### 14.3 Capture + commit results

- [ ] **Step 12: Save sample records**

```bash
ssh heimdal 'echo "101423" | sudo -S jq -c "select(.tier == 3 and .parsed_status == \"ok\") | {bot_name, event_kind, inference_ms, queue_wait_ms, total_latency_ms, raw_response}" /opt/containers/wow/logs/llm_agent_phase4.jsonl' \
  > results/2026-05-14-llm-phase-5-smoke/sample_records.jsonl
mkdir -p results/2026-05-14-llm-phase-5-smoke
wc -l results/2026-05-14-llm-phase-5-smoke/sample_records.jsonl
```

- [ ] **Step 13: Compute headline stats**

```bash
ssh heimdal 'echo "101423" | sudo -S bash -c "
echo == TIER COUNT ==
jq -s \"group_by(.tier) | map({tier: .[0].tier, count: length})\" /opt/containers/wow/logs/llm_agent_phase4.jsonl
echo == T3 LATENCY ==
jq -s \"[.[] | select(.tier == 3)] | {n: length, p50_inference_ms: (map(.inference_ms) | sort | .[length/2|floor]), p95_inference_ms: (map(.inference_ms) | sort | .[length*95/100|floor])}\" /opt/containers/wow/logs/llm_agent_phase4.jsonl
echo == T3 STATUS ==
jq -s \"[.[] | select(.tier == 3)] | group_by(.parsed_status) | map({status: .[0].parsed_status, count: length})\" /opt/containers/wow/logs/llm_agent_phase4.jsonl
echo == EVENT KIND DIST ==
jq -s \"[.[] | select(.tier == 3) | .event_kind] | group_by(.) | map({kind: .[0], count: length})\" /opt/containers/wow/logs/llm_agent_phase4.jsonl
"'
```

- [ ] **Step 14: Write `results/2026-05-14-llm-phase-5-smoke/summary.md`**

Use Phase 4's summary as a template (`results/2026-05-13-llm-phase-4-smoke/summary.md`). Sections:

- TL;DR + §11.3.5 success criterion check (≥3 ok T3 records with valid envelope + ≥1 side_effect applied)
- Run setup (host, llama-server flags, bot population, config snapshot)
- Headline numbers (records by tier, T3 p50/p95, event_kind distribution, sample utterances)
- Bugs found + fixed (likely several — Phase 5.0 ≈ Phase 4.0 in that respect)
- What's confirmed working (per-component checklist)
- Operator state at hand-off
- Phase 5.1 follow-ups

- [ ] **Step 15: Disable LlmAgent (per standing instruction)**

```bash
ssh heimdal 'echo "101423" | sudo -S sed -i "s|AiPlayerbot.LlmAgent.Enabled = 1|AiPlayerbot.LlmAgent.Enabled = 0|" /opt/containers/wow/etc/modules/playerbots.conf && echo "101423" | sudo -S systemctl restart wow-worldserver.service'
```

- [ ] **Step 16: Commit results**

```bash
git add results/2026-05-14-llm-phase-5-smoke/
git commit -m "test(llm-agent): record Phase 5 T3 chat-brain smoke-test results"
git push origin claude/llm-agent-phase-5-t3-chat-brain
```

---

## Phase 5 success criteria

All six must hold:

1. **C++ tests** ≈ 161/161 (137 prior + 7 WhisperBuffer + 4 PersonaCache + 1 schema-existence + 7 ChatEnvelope + 3 counters + 2 config).
2. **Python sidecar tests** still 45/45 (no Python modified).
3. **`Tier3.Enabled = 0`** → byte-identical baseline (T3 trigger short-circuits; rule path runs).
4. **`LlmAgent.Enabled = 0`** → byte-identical baseline.
5. **At least 3 T3 records** with `parsed_status: "ok"` and valid envelope, ≥1 side_effect applied total, `chat_utterances_queued ≥ 1`.
6. **No worldserver-tick regression** (mean ≤ 20 ms, p95 ≤ 50 ms).

## Plan deviations from spec

1. **`memory_about_sender`** — spec §5.1 calls `RecallAbout(sender_name, 2, 3)`. Plan keeps that exactly, but only runs the call when `sender_name` is non-empty (group-join leader on a freshly-spawned group might have an empty name briefly). Empty → empty array, no sidecar hit.
2. **`event_kind` in JSONL** — the spec §5.3 records `event_kind` as a top-level JSONL field. Plan stores it in the `digest_json` (under `digest.event_kind`) because the JSONL record-builder is shared with T1/T2 and we don't want to add another tier-conditional field. Smoke jq queries adjusted accordingly (`(.digest.event_kind // "-")`).
3. **Persona format** — spec §7.1 mentions `LlmAgentPersonality::StubPersonaText`. Plan doesn't touch that — it's already populated lazily in Phase 3's digest path. Phase 5 just reads via `PersonaCache → MemoryHttpClient::GetPersonality`. If the sidecar returns nullopt (first-ever T3 call for a bot whose digest path hasn't run yet), persona is empty and the system message is just the suffix — still well-formed.
