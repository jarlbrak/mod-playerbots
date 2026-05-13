# LLM-Agent Phase 2 Validator + Apply Path Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make T1 actually drive bot behavior. Add a validator that vets each LLM-proposed `NewRpgInfo` transition, an apply path that calls `ChangeTo*` when valid, a `BotSelector` (GUID-hash sampling + social opt-in), per-bot cooldown, digest enrichment (event_log + social), and a three-mode runtime safety dial (`log` / `shadow` / `apply`).

**Architecture:** Adds five new component groups under `src/Bot/LlmAgent/` (`Selector/`, `Cooldown/`, `Validator/`, `Telemetry/`, `EventBuffer/`) and extends `Tier0_StateDigest`, `LlmAgentManager`, `LlmAgentConfig`, and `LlmReplanIdleAction`. The validator uses the same pure-function / worldserver-side split pattern from Phase 1's Tier0 (POD + decision function unit-tested; the `Player*`-touching snapshot built behind `#ifndef LLMAGENT_UNIT_TESTS`).

**Tech Stack:** C++17, existing vendored single-header libs (cpp-httplib, nlohmann/json, doctest). No new dependencies.

**Spec:** [`docs/superpowers/specs/2026-05-13-llm-agent-phase-2-design.md`](../specs/2026-05-13-llm-agent-phase-2-design.md)

---

## File structure

**New files:**

```
src/Bot/LlmAgent/
  Selector/
    BotSelector.h
    BotSelector.cpp
  Cooldown/
    BotCooldownMap.h
    BotCooldownMap.cpp
  Validator/
    ValidationContext.h
    ValidationContext.cpp
    GoalValidator.h
    GoalValidator.cpp
  Telemetry/
    LlmCounters.h
    LlmCounters.cpp
  EventBuffer/
    RecentEventBuffer.h
    RecentEventBuffer.cpp
  Hooks/
    LlmAgentHooks.h
    LlmAgentHooks.cpp

tests/llmagent/
  test_selector.cpp
  test_cooldown.cpp
  test_counters.cpp
  test_validator.cpp
  test_event_buffer.cpp
```

**Existing files modified:**

- `src/Bot/LlmAgent/LlmAgentConfig.h` — add 6 fields, parse `ApplyMode` enum
- `src/Bot/LlmAgent/LlmAgentConfig.cpp` — update `LoadLlmAgentConfig`
- `src/Bot/LlmAgent/LlmAgentManager.h` — own the new components
- `src/Bot/LlmAgent/LlmAgentManager.cpp` — wire counters into HandleRequest
- `src/Bot/LlmAgent/Tiers/Tier0_StateDigest.h` — add helper for pulling event_buffer into digest
- `src/Bot/LlmAgent/Tiers/Tier0_StateDigest.cpp` — call `EventBuffer::Snapshot` from `SnapshotBot`; add nearby_humans + recent_whispers scans
- `src/Bot/LlmAgent/Actions/LlmReplanIdleAction.cpp` — apply-mode decision tree
- `src/Bot/PlayerbotAI.cpp` — one line in `HandleCommand` (whisper hook)
- `src/Script/Playerbots.cpp` — one line in `OnPlayerbotCheckKillTask` (kill hook)
- `conf/playerbots.conf.dist` — 6 new keys
- `tests/llmagent/CMakeLists.txt` — add new test files + new .cpp files
- `tests/llmagent/test_config_load.cpp` — add cases for the new keys
- `tests/llmagent/test_digest_shape.cpp` — extend with the new fields
- `tests/llmagent/test_manager_threading.cpp` — add cooldown + shadow-mode cases

---

## Task 0: Create feature branch

**Files:** none

- [ ] **Step 1: Verify clean tree on `main`**

```bash
git checkout main
git status
```

Expected: `On branch main` and `nothing to commit, working tree clean`.

- [ ] **Step 2: Branch**

```bash
git checkout -b claude/llm-agent-phase-2-validator-apply
git status
```

Expected: `On branch claude/llm-agent-phase-2-validator-apply`.

- [ ] **Step 3: No commit. Branch ready for Task 1.**

---

## Task 1: Extend `LlmAgentConfig` with 6 new keys

**Files:**
- Modify: `src/Bot/LlmAgent/LlmAgentConfig.h`
- Modify: `src/Bot/LlmAgent/LlmAgentConfig.cpp`
- Modify: `tests/llmagent/test_config_load.cpp`

TDD: write the failing test first.

- [ ] **Step 1: Add a new test case in `tests/llmagent/test_config_load.cpp`**

Append at the bottom of the file (before the closing brace if any, or as a new TEST_CASE):

```cpp
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
```

- [ ] **Step 2: Run tests — expect FAIL**

```bash
cmake --build build/llmagent_tests 2>&1 | tail -5
```

Expected: build error referencing `LlmApplyMode` or new fields.

- [ ] **Step 3: Replace `src/Bot/LlmAgent/LlmAgentConfig.h` with the extended version**

```cpp
#ifndef _PLAYERBOT_LLMAGENT_CONFIG_H
#define _PLAYERBOT_LLMAGENT_CONFIG_H

#include <cstdint>
#include <string>

enum class LlmApplyMode { Log, Shadow, Apply };

struct LlmAgentConfig {
    bool         Enabled            = false;
    std::string  Endpoint           = "http://127.0.0.1:8080";
    std::string  Model              = "qwen2.5-7b-instruct-q4_k_m.gguf";
    uint32_t     WorkerThreads      = 4;
    uint32_t     RequestTimeoutMs   = 15000;
    std::string  JsonlPath          = "logs/llm_agent_phase1.jsonl";
    std::string  SystemPrompt;

    // Phase 2
    LlmApplyMode ApplyMode          = LlmApplyMode::Log;
    uint32_t     SamplePct          = 0;
    bool         SocialOptIn        = true;
    uint32_t     MaxCooldownMinutes = 60;
    uint32_t     FallbackCooldownMs = 300000;
    uint32_t     EventLogSize       = 20;
};

extern const char* const kDefaultSystemPrompt;

LlmApplyMode ParseApplyMode(const std::string& s);

template <typename Source>
LlmAgentConfig LoadLlmAgentConfig(const Source& src) {
    LlmAgentConfig cfg;
    cfg.Enabled            = src.template Get<bool>       ("AiPlayerbot.LlmAgent.Enabled",            false);
    cfg.Endpoint           = src.template Get<std::string>("AiPlayerbot.LlmAgent.Endpoint",           std::string{"http://127.0.0.1:8080"});
    cfg.Model              = src.template Get<std::string>("AiPlayerbot.LlmAgent.Model",              std::string{"qwen2.5-7b-instruct-q4_k_m.gguf"});
    cfg.WorkerThreads      = src.template Get<uint32_t>   ("AiPlayerbot.LlmAgent.WorkerThreads",      uint32_t{4});
    cfg.RequestTimeoutMs   = src.template Get<uint32_t>   ("AiPlayerbot.LlmAgent.RequestTimeoutMs",   uint32_t{15000});
    cfg.JsonlPath          = src.template Get<std::string>("AiPlayerbot.LlmAgent.JsonlPath",          std::string{"logs/llm_agent_phase1.jsonl"});
    cfg.SystemPrompt       = src.template Get<std::string>("AiPlayerbot.LlmAgent.SystemPrompt",       std::string{kDefaultSystemPrompt});

    cfg.ApplyMode          = ParseApplyMode(src.template Get<std::string>("AiPlayerbot.LlmAgent.ApplyMode", std::string{"log"}));
    cfg.SamplePct          = src.template Get<uint32_t>("AiPlayerbot.LlmAgent.SamplePct",          uint32_t{0});
    cfg.SocialOptIn        = src.template Get<bool>    ("AiPlayerbot.LlmAgent.SocialOptIn",        true);
    cfg.MaxCooldownMinutes = src.template Get<uint32_t>("AiPlayerbot.LlmAgent.MaxCooldownMinutes", uint32_t{60});
    cfg.FallbackCooldownMs = src.template Get<uint32_t>("AiPlayerbot.LlmAgent.FallbackCooldownMs", uint32_t{300000});
    cfg.EventLogSize       = src.template Get<uint32_t>("AiPlayerbot.LlmAgent.EventLogSize",       uint32_t{20});
    return cfg;
}

#endif
```

- [ ] **Step 4: Add `ParseApplyMode` to `src/Bot/LlmAgent/LlmAgentConfig.cpp`**

Append after the existing `kDefaultSystemPrompt` definition:

```cpp
LlmApplyMode ParseApplyMode(const std::string& s) {
    if (s == "apply")  return LlmApplyMode::Apply;
    if (s == "shadow") return LlmApplyMode::Shadow;
    return LlmApplyMode::Log;  // default + unknown
}
```

- [ ] **Step 5: Build and run**

```bash
cmake --build build/llmagent_tests
./build/llmagent_tests/llmagent_unit_tests
```

Expected: all 32 prior + 5 new = 37 cases pass.

- [ ] **Step 6: Commit**

```bash
git add src/Bot/LlmAgent/LlmAgentConfig.h src/Bot/LlmAgent/LlmAgentConfig.cpp tests/llmagent/test_config_load.cpp
git commit -m "feat(llm-agent): extend LlmAgentConfig with Phase 2 keys

Adds ApplyMode (enum), SamplePct, SocialOptIn, MaxCooldownMinutes,
FallbackCooldownMs, EventLogSize. ApplyMode parser falls back to
Log on unknown values (safer default)."
```

---

## Task 2: `BotSelector` — GUID hash + opt-in set

**Files:**
- Create: `src/Bot/LlmAgent/Selector/BotSelector.h`
- Create: `src/Bot/LlmAgent/Selector/BotSelector.cpp`
- Create: `tests/llmagent/test_selector.cpp`
- Modify: `tests/llmagent/CMakeLists.txt`

TDD.

- [ ] **Step 1: Write failing test `tests/llmagent/test_selector.cpp`**

```cpp
#include "doctest.h"
#include "Selector/BotSelector.h"
#include <atomic>
#include <thread>
#include <vector>

TEST_CASE("BotSelector SamplePct=0 hits nothing") {
    BotSelector s;
    s.Configure(0, true);
    int hits = 0;
    for (uint64_t i = 1; i <= 1000; ++i) {
        if (s.IsLlmBot(i)) ++hits;
    }
    CHECK(hits == 0);
}

TEST_CASE("BotSelector SamplePct=100 hits everything") {
    BotSelector s;
    s.Configure(100, true);
    int hits = 0;
    for (uint64_t i = 1; i <= 1000; ++i) {
        if (s.IsLlmBot(i)) ++hits;
    }
    CHECK(hits == 1000);
}

TEST_CASE("BotSelector SamplePct=10 chi-square within tolerance") {
    BotSelector s;
    s.Configure(10, true);
    int hits = 0;
    constexpr int N = 10000;
    for (uint64_t i = 1; i <= N; ++i) {
        if (s.IsLlmBot(i)) ++hits;
    }
    // Expected ~1000 hits. Standard deviation = sqrt(N*p*(1-p)) ≈ 30.
    // 4-sigma tolerance = ~120, well within chi-square sanity.
    CHECK(hits >= 880);
    CHECK(hits <= 1120);
}

TEST_CASE("BotSelector opt-in adds bots regardless of sample") {
    BotSelector s;
    s.Configure(0, true);
    s.OptInBot(42);
    CHECK(s.IsLlmBot(42) == true);
    CHECK(s.IsLlmBot(43) == false);
}

TEST_CASE("BotSelector opt-in is idempotent") {
    BotSelector s;
    s.Configure(0, true);
    s.OptInBot(7);
    s.OptInBot(7);
    s.OptInBot(7);
    CHECK(s.IsLlmBot(7) == true);
}

TEST_CASE("BotSelector Clear empties the opt-in set") {
    BotSelector s;
    s.Configure(0, true);
    s.OptInBot(100);
    s.OptInBot(101);
    s.Clear();
    CHECK(s.IsLlmBot(100) == false);
    CHECK(s.IsLlmBot(101) == false);
}

TEST_CASE("BotSelector OptInBot is thread-safe") {
    BotSelector s;
    s.Configure(0, true);
    constexpr int N = 4;
    constexpr int PER = 1000;
    std::vector<std::thread> threads;
    for (int t = 0; t < N; ++t) {
        threads.emplace_back([&, t]{
            for (int i = 0; i < PER; ++i) {
                s.OptInBot(static_cast<uint64_t>(t * PER + i + 1));
            }
        });
    }
    for (auto& th : threads) th.join();
    int count = 0;
    for (uint64_t i = 1; i <= N * PER; ++i) {
        if (s.IsLlmBot(i)) ++count;
    }
    CHECK(count == N * PER);
}

TEST_CASE("BotSelector SocialOptIn=false ignores opted-in bots") {
    BotSelector s;
    s.Configure(0, false);
    s.OptInBot(42);
    CHECK(s.IsLlmBot(42) == false);
}
```

- [ ] **Step 2: Add to `tests/llmagent/CMakeLists.txt`**

Update `add_executable`:

```cmake
add_executable(llmagent_unit_tests
  doctest_main.cpp
  test_config_load.cpp
  test_goal_parser.cpp
  test_http_client.cpp
  test_digest_shape.cpp
  test_manager_threading.cpp
  test_selector.cpp
  ${LLMAGENT_DIR}/LlmAgentConfig.cpp
  ${LLMAGENT_DIR}/Schemas/Goal.cpp
  ${LLMAGENT_DIR}/Client/LlmHttpClient.cpp
  ${LLMAGENT_DIR}/Tiers/Tier0_StateDigest.cpp
  ${LLMAGENT_DIR}/LlmAgentManager.cpp
  ${LLMAGENT_DIR}/Selector/BotSelector.cpp
)
```

- [ ] **Step 3: Run — expect FAIL**

```bash
cmake --build build/llmagent_tests 2>&1 | tail -5
```

Expected: `Cannot find source file` for BotSelector.cpp or missing header.

- [ ] **Step 4: Write `src/Bot/LlmAgent/Selector/BotSelector.h`**

```cpp
#ifndef _PLAYERBOT_LLMAGENT_BOT_SELECTOR_H
#define _PLAYERBOT_LLMAGENT_BOT_SELECTOR_H

#include <cstdint>
#include <mutex>
#include <unordered_set>

class BotSelector {
  public:
    void Configure(uint32_t sample_pct, bool social_opt_in);
    bool IsLlmBot(uint64_t bot_guid) const;
    void OptInBot(uint64_t bot_guid);  // thread-safe, idempotent
    void Clear();

  private:
    static uint64_t FNV1aHash(uint64_t v);

    uint32_t                       sample_pct_     = 0;
    bool                           social_enabled_ = true;
    mutable std::mutex             opt_in_mu_;
    std::unordered_set<uint64_t>   opt_in_;
};

#endif
```

- [ ] **Step 5: Write `src/Bot/LlmAgent/Selector/BotSelector.cpp`**

```cpp
#include "Selector/BotSelector.h"

void BotSelector::Configure(uint32_t sample_pct, bool social_opt_in) {
    sample_pct_     = sample_pct > 100 ? 100 : sample_pct;
    social_enabled_ = social_opt_in;
}

bool BotSelector::IsLlmBot(uint64_t bot_guid) const {
    if (sample_pct_ > 0 && (FNV1aHash(bot_guid) % 100) < sample_pct_) return true;
    if (!social_enabled_) return false;
    std::lock_guard<std::mutex> g(opt_in_mu_);
    return opt_in_.count(bot_guid) > 0;
}

void BotSelector::OptInBot(uint64_t bot_guid) {
    std::lock_guard<std::mutex> g(opt_in_mu_);
    opt_in_.insert(bot_guid);
}

void BotSelector::Clear() {
    std::lock_guard<std::mutex> g(opt_in_mu_);
    opt_in_.clear();
}

uint64_t BotSelector::FNV1aHash(uint64_t v) {
    constexpr uint64_t kOffset = 14695981039346656037ULL;
    constexpr uint64_t kPrime  = 1099511628211ULL;
    uint64_t h = kOffset;
    for (int i = 0; i < 8; ++i) {
        h ^= (v & 0xFFu);
        h *= kPrime;
        v >>= 8;
    }
    return h;
}
```

- [ ] **Step 6: Build and run**

```bash
cmake --build build/llmagent_tests
./build/llmagent_tests/llmagent_unit_tests
```

Expected: 37 prior + 8 new = 45 cases pass.

- [ ] **Step 7: Commit**

```bash
git add src/Bot/LlmAgent/Selector/ tests/llmagent/test_selector.cpp tests/llmagent/CMakeLists.txt
git commit -m "feat(llm-agent): BotSelector — GUID hash + opt-in set

FNV-1a hash for stable distribution across runs. SamplePct=0 fast-path
avoids hash + lock when no bots are sampled and opt-in is unused.
8 test cases including a 4-thread concurrent OptInBot stress."
```

---

## Task 3: `BotCooldownMap` — per-bot expiry

**Files:**
- Create: `src/Bot/LlmAgent/Cooldown/BotCooldownMap.h`
- Create: `src/Bot/LlmAgent/Cooldown/BotCooldownMap.cpp`
- Create: `tests/llmagent/test_cooldown.cpp`
- Modify: `tests/llmagent/CMakeLists.txt`

- [ ] **Step 1: Write failing test `tests/llmagent/test_cooldown.cpp`**

```cpp
#include "doctest.h"
#include "Cooldown/BotCooldownMap.h"
#include <chrono>
#include <thread>

using namespace std::chrono;

TEST_CASE("BotCooldownMap unseen bot is eligible") {
    BotCooldownMap m;
    CHECK(m.Eligible(123) == true);
}

TEST_CASE("BotCooldownMap Set blocks until expiry") {
    BotCooldownMap m;
    auto until = steady_clock::now() + milliseconds(200);
    m.Set(42, until);
    CHECK(m.Eligible(42) == false);
    std::this_thread::sleep_for(milliseconds(250));
    CHECK(m.Eligible(42) == true);
}

TEST_CASE("BotCooldownMap bots are independent") {
    BotCooldownMap m;
    m.Set(1, steady_clock::now() + seconds(60));
    CHECK(m.Eligible(1) == false);
    CHECK(m.Eligible(2) == true);
}

TEST_CASE("BotCooldownMap Set overwrites earlier value") {
    BotCooldownMap m;
    m.Set(1, steady_clock::now() + seconds(60));
    m.Set(1, steady_clock::now() - seconds(1));  // already in the past
    CHECK(m.Eligible(1) == true);
}

TEST_CASE("BotCooldownMap Clear resets everything") {
    BotCooldownMap m;
    m.Set(1, steady_clock::now() + seconds(60));
    m.Set(2, steady_clock::now() + seconds(60));
    m.Clear();
    CHECK(m.Eligible(1) == true);
    CHECK(m.Eligible(2) == true);
}

TEST_CASE("BotCooldownMap returns time_until_eligible") {
    BotCooldownMap m;
    auto until = steady_clock::now() + milliseconds(500);
    m.Set(7, until);
    auto remaining = m.RemainingMs(7);
    CHECK(remaining.has_value());
    CHECK(*remaining > 100);
    CHECK(*remaining < 600);
}
```

- [ ] **Step 2: Add to CMakeLists**

Update `add_executable`:

```cmake
add_executable(llmagent_unit_tests
  doctest_main.cpp
  test_config_load.cpp
  test_goal_parser.cpp
  test_http_client.cpp
  test_digest_shape.cpp
  test_manager_threading.cpp
  test_selector.cpp
  test_cooldown.cpp
  ${LLMAGENT_DIR}/LlmAgentConfig.cpp
  ${LLMAGENT_DIR}/Schemas/Goal.cpp
  ${LLMAGENT_DIR}/Client/LlmHttpClient.cpp
  ${LLMAGENT_DIR}/Tiers/Tier0_StateDigest.cpp
  ${LLMAGENT_DIR}/LlmAgentManager.cpp
  ${LLMAGENT_DIR}/Selector/BotSelector.cpp
  ${LLMAGENT_DIR}/Cooldown/BotCooldownMap.cpp
)
```

- [ ] **Step 3: Run — expect FAIL**

```bash
cmake --build build/llmagent_tests 2>&1 | tail -5
```

- [ ] **Step 4: Write `src/Bot/LlmAgent/Cooldown/BotCooldownMap.h`**

```cpp
#ifndef _PLAYERBOT_LLMAGENT_BOT_COOLDOWN_MAP_H
#define _PLAYERBOT_LLMAGENT_BOT_COOLDOWN_MAP_H

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>

class BotCooldownMap {
  public:
    bool Eligible(uint64_t bot_guid) const;
    void Set(uint64_t bot_guid, std::chrono::steady_clock::time_point until);
    void Clear();

    // Diagnostic / telemetry helper. Returns nullopt if no cooldown.
    std::optional<int64_t> RemainingMs(uint64_t bot_guid) const;

  private:
    mutable std::mutex                                              mu_;
    std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> cooldowns_;
};

#endif
```

- [ ] **Step 5: Write `src/Bot/LlmAgent/Cooldown/BotCooldownMap.cpp`**

```cpp
#include "Cooldown/BotCooldownMap.h"

bool BotCooldownMap::Eligible(uint64_t bot_guid) const {
    std::lock_guard<std::mutex> g(mu_);
    auto it = cooldowns_.find(bot_guid);
    if (it == cooldowns_.end()) return true;
    return std::chrono::steady_clock::now() >= it->second;
}

void BotCooldownMap::Set(uint64_t bot_guid, std::chrono::steady_clock::time_point until) {
    std::lock_guard<std::mutex> g(mu_);
    cooldowns_[bot_guid] = until;
}

void BotCooldownMap::Clear() {
    std::lock_guard<std::mutex> g(mu_);
    cooldowns_.clear();
}

std::optional<int64_t> BotCooldownMap::RemainingMs(uint64_t bot_guid) const {
    std::lock_guard<std::mutex> g(mu_);
    auto it = cooldowns_.find(bot_guid);
    if (it == cooldowns_.end()) return std::nullopt;
    auto delta = it->second - std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(delta).count();
}
```

- [ ] **Step 6: Build and run**

```bash
cmake --build build/llmagent_tests
./build/llmagent_tests/llmagent_unit_tests
```

Expected: 51 cases pass.

- [ ] **Step 7: Commit**

```bash
git add src/Bot/LlmAgent/Cooldown/ tests/llmagent/test_cooldown.cpp tests/llmagent/CMakeLists.txt
git commit -m "feat(llm-agent): BotCooldownMap — per-bot expiry timestamps

Uses steady_clock (survives wall-clock adjustments). 6 test cases
including independence between bots and Set-overwrites-past."
```

---

## Task 4: `LlmCounters` — atomic telemetry

**Files:**
- Create: `src/Bot/LlmAgent/Telemetry/LlmCounters.h`
- Create: `src/Bot/LlmAgent/Telemetry/LlmCounters.cpp`
- Create: `tests/llmagent/test_counters.cpp`
- Modify: `tests/llmagent/CMakeLists.txt`

- [ ] **Step 1: Write failing test `tests/llmagent/test_counters.cpp`**

```cpp
#include "doctest.h"
#include "Telemetry/LlmCounters.h"
#include "Schemas/Goal.h"  // for ValidationResult (defined in Task 5; for now we'll use Goal.h dependencies)
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
    c.IncStatus("garbage_unknown");  // unknown should be ignored or routed to "other"
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
```

- [ ] **Step 2: Add to CMakeLists**

```cmake
add_executable(llmagent_unit_tests
  doctest_main.cpp
  test_config_load.cpp
  test_goal_parser.cpp
  test_http_client.cpp
  test_digest_shape.cpp
  test_manager_threading.cpp
  test_selector.cpp
  test_cooldown.cpp
  test_counters.cpp
  ${LLMAGENT_DIR}/LlmAgentConfig.cpp
  ${LLMAGENT_DIR}/Schemas/Goal.cpp
  ${LLMAGENT_DIR}/Client/LlmHttpClient.cpp
  ${LLMAGENT_DIR}/Tiers/Tier0_StateDigest.cpp
  ${LLMAGENT_DIR}/LlmAgentManager.cpp
  ${LLMAGENT_DIR}/Selector/BotSelector.cpp
  ${LLMAGENT_DIR}/Cooldown/BotCooldownMap.cpp
  ${LLMAGENT_DIR}/Telemetry/LlmCounters.cpp
)
```

- [ ] **Step 3: Run — expect FAIL**

```bash
cmake --build build/llmagent_tests 2>&1 | tail -5
```

- [ ] **Step 4: Write `src/Bot/LlmAgent/Telemetry/LlmCounters.h`**

```cpp
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
    void IncStatus(const std::string& parsed_status);   // ok / schema_error / transport_error / http_error / timeout
    void IncValidatorAccepted();
    void IncValidatorRejected(const std::string& reason);
    void IncApplied();
    void IncShadowAccepted();
    void IncLogAcceptedSkipped();
    void IncFallbackUsed();
    void IncAppliedThrew();

    Snapshot_t Snapshot() const;
    void DumpToLog() const;   // writes a multi-line summary to LOG_INFO

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
```

- [ ] **Step 5: Write `src/Bot/LlmAgent/Telemetry/LlmCounters.cpp`**

```cpp
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
```

- [ ] **Step 6: Build and run**

```bash
cmake --build build/llmagent_tests
./build/llmagent_tests/llmagent_unit_tests
```

Expected: 56 cases pass.

- [ ] **Step 7: Commit**

```bash
git add src/Bot/LlmAgent/Telemetry/ tests/llmagent/test_counters.cpp tests/llmagent/CMakeLists.txt
git commit -m "feat(llm-agent): LlmCounters — atomic per-outcome telemetry

12 fixed atomic counters + a small map keyed by reject_reason. The
DumpToLog log target is guarded so unit tests don't link Log.h."
```

---

## Task 5: `ValidationContext` (POD) + `GoalValidator` (pure)

**Files:**
- Create: `src/Bot/LlmAgent/Validator/ValidationContext.h`
- Create: `src/Bot/LlmAgent/Validator/ValidationContext.cpp` (snapshot func, guarded)
- Create: `src/Bot/LlmAgent/Validator/GoalValidator.h`
- Create: `src/Bot/LlmAgent/Validator/GoalValidator.cpp`
- Create: `tests/llmagent/test_validator.cpp`
- Modify: `tests/llmagent/CMakeLists.txt`

- [ ] **Step 1: Write failing test `tests/llmagent/test_validator.cpp`**

```cpp
#include "doctest.h"
#include "Validator/GoalValidator.h"
#include "Validator/ValidationContext.h"
#include "Schemas/Goal.h"

namespace {
BotValidationContext make_ctx() {
    BotValidationContext c;
    c.bot_level = 37;
    c.map_id    = 0;
    c.map_min_x = -10000.0; c.map_max_x = 10000.0;
    c.map_min_y = -10000.0; c.map_max_y = 10000.0;
    c.quest_log.push_back({502, /*status=*/0, /*objective_count=*/3});
    c.quest_log.push_back({488, /*status=*/1, /*objective_count=*/1});  // complete
    c.nearby_creature_guids = {1001, 1002, 1003};
    c.known_flight_node_ids = {77, 88};
    c.valid_capture_point_spawn_ids = {200};
    return c;
}

ParsedGoal goal_idle() { return ParsedGoal{GoalKind::Idle, IdleParams{}, "x", 5}; }
ParsedGoal goal_rest() { return ParsedGoal{GoalKind::Rest, RestParams{}, "x", 5}; }
ParsedGoal goal_wander_random() { return ParsedGoal{GoalKind::WanderRandom, WanderRandomParams{}, "x", 5}; }
}  // namespace

TEST_CASE("ValidateGoalDecision Idle is always accepted") {
    auto r = ValidateGoalDecision(goal_idle(), make_ctx());
    CHECK(r.accepted == true);
}

TEST_CASE("ValidateGoalDecision Rest is always accepted") {
    auto r = ValidateGoalDecision(goal_rest(), make_ctx());
    CHECK(r.accepted == true);
}

TEST_CASE("ValidateGoalDecision WanderRandom is always accepted") {
    auto r = ValidateGoalDecision(goal_wander_random(), make_ctx());
    CHECK(r.accepted == true);
}

TEST_CASE("ValidateGoalDecision DoQuest accepts existing incomplete quest") {
    ParsedGoal g{GoalKind::DoQuest, DoQuestParams{502, 0}, "x", 10};
    auto r = ValidateGoalDecision(g, make_ctx());
    CHECK(r.accepted == true);
}

TEST_CASE("ValidateGoalDecision DoQuest rejects quest not in log") {
    ParsedGoal g{GoalKind::DoQuest, DoQuestParams{999, 0}, "x", 10};
    auto r = ValidateGoalDecision(g, make_ctx());
    CHECK(r.accepted == false);
    CHECK(r.reject_reason == "rejected_quest_not_in_log");
}

TEST_CASE("ValidateGoalDecision DoQuest rejects already-completed quest") {
    ParsedGoal g{GoalKind::DoQuest, DoQuestParams{488, 0}, "x", 10};
    auto r = ValidateGoalDecision(g, make_ctx());
    CHECK(r.accepted == false);
    CHECK(r.reject_reason == "rejected_quest_already_complete");
}

TEST_CASE("ValidateGoalDecision DoQuest rejects objective_idx out of range") {
    ParsedGoal g{GoalKind::DoQuest, DoQuestParams{502, 99}, "x", 10};
    auto r = ValidateGoalDecision(g, make_ctx());
    CHECK(r.accepted == false);
    CHECK(r.reject_reason == "rejected_objective_idx_out_of_range");
}

TEST_CASE("ValidateGoalDecision GoGrind accepts in-bounds same-map position") {
    ParsedGoal g{GoalKind::GoGrind, GoGrindParams{100.0, 200.0, 0.0, 0}, "x", 5};
    auto r = ValidateGoalDecision(g, make_ctx());
    CHECK(r.accepted == true);
}

TEST_CASE("ValidateGoalDecision GoGrind rejects wrong map") {
    ParsedGoal g{GoalKind::GoGrind, GoGrindParams{100.0, 200.0, 0.0, 530}, "x", 5};
    auto r = ValidateGoalDecision(g, make_ctx());
    CHECK(r.accepted == false);
    CHECK(r.reject_reason == "rejected_map_mismatch");
}

TEST_CASE("ValidateGoalDecision GoGrind rejects out-of-bounds position") {
    ParsedGoal g{GoalKind::GoGrind, GoGrindParams{99999.0, 0.0, 0.0, 0}, "x", 5};
    auto r = ValidateGoalDecision(g, make_ctx());
    CHECK(r.accepted == false);
    CHECK(r.reject_reason == "rejected_position_out_of_bounds");
}

TEST_CASE("ValidateGoalDecision GoCamp follows the same rules as GoGrind") {
    ParsedGoal good{GoalKind::GoCamp, GoCampParams{100.0, 200.0, 0.0, 0}, "x", 5};
    ParsedGoal bad {GoalKind::GoCamp, GoCampParams{100.0, 200.0, 0.0, 530}, "x", 5};
    CHECK(ValidateGoalDecision(good, make_ctx()).accepted == true);
    CHECK(ValidateGoalDecision(bad,  make_ctx()).accepted == false);
}

TEST_CASE("ValidateGoalDecision WanderNpc accepts nearby npc") {
    ParsedGoal g{GoalKind::WanderNpc, WanderNpcParams{1002}, "x", 5};
    auto r = ValidateGoalDecision(g, make_ctx());
    CHECK(r.accepted == true);
}

TEST_CASE("ValidateGoalDecision WanderNpc rejects far npc") {
    ParsedGoal g{GoalKind::WanderNpc, WanderNpcParams{9999}, "x", 5};
    auto r = ValidateGoalDecision(g, make_ctx());
    CHECK(r.accepted == false);
    CHECK(r.reject_reason == "rejected_npc_not_nearby");
}

TEST_CASE("ValidateGoalDecision WanderNpc is permissive when nearby list empty") {
    ParsedGoal g{GoalKind::WanderNpc, WanderNpcParams{9999}, "x", 5};
    BotValidationContext c = make_ctx();
    c.nearby_creature_guids.clear();
    auto r = ValidateGoalDecision(g, c);
    CHECK(r.accepted == true);
}

TEST_CASE("ValidateGoalDecision TravelFlight accepts known destination") {
    ParsedGoal g{GoalKind::TravelFlight, TravelFlightParams{0, 77}, "x", 30};
    auto r = ValidateGoalDecision(g, make_ctx());
    CHECK(r.accepted == true);
}

TEST_CASE("ValidateGoalDecision TravelFlight rejects unknown destination") {
    ParsedGoal g{GoalKind::TravelFlight, TravelFlightParams{0, 999}, "x", 30};
    auto r = ValidateGoalDecision(g, make_ctx());
    CHECK(r.accepted == false);
    CHECK(r.reject_reason == "rejected_unknown_flight_node");
}

TEST_CASE("ValidateGoalDecision OutdoorPvp accepts known capture point") {
    ParsedGoal g{GoalKind::OutdoorPvp, OutdoorPvpParams{200}, "x", 30};
    auto r = ValidateGoalDecision(g, make_ctx());
    CHECK(r.accepted == true);
}

TEST_CASE("ValidateGoalDecision OutdoorPvp rejects unknown capture point") {
    ParsedGoal g{GoalKind::OutdoorPvp, OutdoorPvpParams{999}, "x", 30};
    auto r = ValidateGoalDecision(g, make_ctx());
    CHECK(r.accepted == false);
    CHECK(r.reject_reason == "rejected_unknown_capture_point");
}
```

- [ ] **Step 2: Add to CMakeLists**

```cmake
add_executable(llmagent_unit_tests
  doctest_main.cpp
  test_config_load.cpp
  test_goal_parser.cpp
  test_http_client.cpp
  test_digest_shape.cpp
  test_manager_threading.cpp
  test_selector.cpp
  test_cooldown.cpp
  test_counters.cpp
  test_validator.cpp
  ${LLMAGENT_DIR}/LlmAgentConfig.cpp
  ${LLMAGENT_DIR}/Schemas/Goal.cpp
  ${LLMAGENT_DIR}/Client/LlmHttpClient.cpp
  ${LLMAGENT_DIR}/Tiers/Tier0_StateDigest.cpp
  ${LLMAGENT_DIR}/LlmAgentManager.cpp
  ${LLMAGENT_DIR}/Selector/BotSelector.cpp
  ${LLMAGENT_DIR}/Cooldown/BotCooldownMap.cpp
  ${LLMAGENT_DIR}/Telemetry/LlmCounters.cpp
  ${LLMAGENT_DIR}/Validator/GoalValidator.cpp
  ${LLMAGENT_DIR}/Validator/ValidationContext.cpp
)
```

- [ ] **Step 3: Run — expect FAIL**

```bash
cmake --build build/llmagent_tests 2>&1 | tail -5
```

- [ ] **Step 4: Write `src/Bot/LlmAgent/Validator/ValidationContext.h`**

```cpp
#ifndef _PLAYERBOT_LLMAGENT_VALIDATION_CONTEXT_H
#define _PLAYERBOT_LLMAGENT_VALIDATION_CONTEXT_H

#include <cstdint>
#include <unordered_set>
#include <vector>

struct QuestLogContextEntry {
    uint32_t id              = 0;
    uint32_t status          = 0;        // 0 = incomplete, 1 = complete-not-turned-in, 2 = complete-and-turned-in
    int32_t  objective_count = 0;
};

struct BotValidationContext {
    uint32_t bot_level = 0;
    uint32_t map_id    = 0;
    double   map_min_x = -100000.0;
    double   map_max_x =  100000.0;
    double   map_min_y = -100000.0;
    double   map_max_y =  100000.0;

    std::vector<QuestLogContextEntry>  quest_log;
    std::vector<uint64_t>              nearby_creature_guids;
    std::unordered_set<uint32_t>       known_flight_node_ids;
    std::unordered_set<uint32_t>       valid_capture_point_spawn_ids;
};

#ifndef LLMAGENT_UNIT_TESTS
class PlayerbotAI;
BotValidationContext SnapshotForValidation(PlayerbotAI* botAI);
#endif

#endif
```

- [ ] **Step 5: Write `src/Bot/LlmAgent/Validator/ValidationContext.cpp`**

```cpp
#include "Validator/ValidationContext.h"

#ifndef LLMAGENT_UNIT_TESTS

#include "PlayerbotAI.h"
#include "Player.h"
#include "Map.h"
#include "QuestDef.h"

BotValidationContext SnapshotForValidation(PlayerbotAI* botAI) {
    BotValidationContext c;
    if (!botAI) return c;
    Player* bot = botAI->GetBot();
    if (!bot) return c;

    c.bot_level = bot->GetLevel();
    if (Map* m = bot->GetMap()) {
        c.map_id = m->GetId();
    }
    // Quest log: up to MAX_QUEST_LOG_SIZE entries
    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot) {
        uint32 qid = bot->GetQuestSlotQuestId(slot);
        if (!qid) continue;
        const Quest* q = sObjectMgr->GetQuestTemplate(qid);
        if (!q) continue;
        QuestLogContextEntry e;
        e.id = qid;
        QuestStatus st = bot->GetQuestStatus(qid);
        e.status = (st == QUEST_STATUS_COMPLETE) ? 1u : 0u;
        e.objective_count = q->GetQuestObjectiveCountType(QUEST_OBJECTIVE_TYPE_NONE) > 0
                            ? q->GetQuestObjectiveCountType(QUEST_OBJECTIVE_TYPE_NONE) : 3;
        c.quest_log.push_back(e);
    }
    // Map bounds — leave defaults wide unless we can read the actual map dimensions cheaply
    // (most WoTLK maps are within ±17066). Phase 2 keeps the defaults.

    // Nearby creatures: leave empty for Phase 2. WanderNpc validation falls
    // through to the permissive empty-list branch. Phase 3 wires this.

    // Known flight nodes + capture points: leave empty for Phase 2. TravelFlight
    // and OutdoorPvp validations will hard-reject for now; rule-based fallback
    // covers those transitions, which is fine.

    return c;
}

#endif
```

(The defensive "leave empty" choices are deliberate. Phase 2 ships the
validator interface and the easy variants; the costly nearby-creature
scans are deferred. This is correct because Phase 2's success criterion
is "the apply path actually changes a bot's state via the LLM at least
once" — the easy variants are sufficient to demonstrate that.)

- [ ] **Step 6: Write `src/Bot/LlmAgent/Validator/GoalValidator.h`**

```cpp
#ifndef _PLAYERBOT_LLMAGENT_GOAL_VALIDATOR_H
#define _PLAYERBOT_LLMAGENT_GOAL_VALIDATOR_H

#include "Schemas/Goal.h"
#include "Validator/ValidationContext.h"
#include <string>

struct ValidationResult {
    bool        accepted = false;
    std::string reject_reason;       // empty when accepted
};

ValidationResult ValidateGoalDecision(const ParsedGoal& g, const BotValidationContext& ctx);

#endif
```

- [ ] **Step 7: Write `src/Bot/LlmAgent/Validator/GoalValidator.cpp`**

```cpp
#include "Validator/GoalValidator.h"

namespace {

const QuestLogContextEntry* find_quest(const BotValidationContext& ctx, uint32_t quest_id) {
    for (const auto& q : ctx.quest_log) {
        if (q.id == quest_id) return &q;
    }
    return nullptr;
}

ValidationResult check_grind_or_camp(const BotValidationContext& ctx, double x, double y, int32_t map_id) {
    if (static_cast<uint32_t>(map_id) != ctx.map_id) {
        return ValidationResult{false, "rejected_map_mismatch"};
    }
    if (x < ctx.map_min_x || x > ctx.map_max_x || y < ctx.map_min_y || y > ctx.map_max_y) {
        return ValidationResult{false, "rejected_position_out_of_bounds"};
    }
    return ValidationResult{true, ""};
}

}  // namespace

ValidationResult ValidateGoalDecision(const ParsedGoal& g, const BotValidationContext& ctx) {
    switch (g.goal) {
        case GoalKind::Idle:
        case GoalKind::WanderRandom:
        case GoalKind::Rest:
            return {true, ""};

        case GoalKind::DoQuest: {
            const auto& p = std::get<DoQuestParams>(g.params);
            const auto* q = find_quest(ctx, p.quest_id);
            if (!q)
                return {false, "rejected_quest_not_in_log"};
            if (q->status >= 2)
                return {false, "rejected_quest_already_complete"};
            if (p.starting_objective_idx < 0 || p.starting_objective_idx >= q->objective_count)
                return {false, "rejected_objective_idx_out_of_range"};
            return {true, ""};
        }

        case GoalKind::GoGrind: {
            const auto& p = std::get<GoGrindParams>(g.params);
            return check_grind_or_camp(ctx, p.x, p.y, p.map_id);
        }

        case GoalKind::GoCamp: {
            const auto& p = std::get<GoCampParams>(g.params);
            return check_grind_or_camp(ctx, p.x, p.y, p.map_id);
        }

        case GoalKind::WanderNpc: {
            const auto& p = std::get<WanderNpcParams>(g.params);
            if (ctx.nearby_creature_guids.empty()) return {true, ""};  // permissive
            for (auto guid : ctx.nearby_creature_guids) {
                if (guid == p.npc_guid) return {true, ""};
            }
            return {false, "rejected_npc_not_nearby"};
        }

        case GoalKind::TravelFlight: {
            const auto& p = std::get<TravelFlightParams>(g.params);
            if (ctx.known_flight_node_ids.count(p.destination_node_id) == 0)
                return {false, "rejected_unknown_flight_node"};
            return {true, ""};
        }

        case GoalKind::OutdoorPvp: {
            const auto& p = std::get<OutdoorPvpParams>(g.params);
            if (ctx.valid_capture_point_spawn_ids.count(p.capture_point_spawn_id) == 0)
                return {false, "rejected_unknown_capture_point"};
            return {true, ""};
        }
    }
    return {false, "rejected_unhandled_goal_kind"};
}
```

- [ ] **Step 8: Build and run**

```bash
cmake --build build/llmagent_tests
./build/llmagent_tests/llmagent_unit_tests
```

Expected: 56 prior + 17 new = 73 cases pass.

- [ ] **Step 9: Commit**

```bash
git add src/Bot/LlmAgent/Validator/ tests/llmagent/test_validator.cpp tests/llmagent/CMakeLists.txt
git commit -m "feat(llm-agent): GoalValidator + ValidationContext

Pure decision function over a POD context. 17 doctest cases cover
every variant: always-accepted (Idle/Rest/WanderRandom) plus the
five variants with rules and at least one reject path each.

SnapshotForValidation is the worldserver-side adapter, guarded by
LLMAGENT_UNIT_TESTS. Phase 2 fills bot_level, map_id, and quest_log;
nearby_creature_guids / flight nodes / capture points stay empty
(deferred to Phase 3 — falls through to permissive/reject as
documented in spec §5.3)."
```

---

## Task 6: `RecentEventBuffer` — per-bot ring buffer

**Files:**
- Create: `src/Bot/LlmAgent/EventBuffer/RecentEventBuffer.h`
- Create: `src/Bot/LlmAgent/EventBuffer/RecentEventBuffer.cpp`
- Create: `tests/llmagent/test_event_buffer.cpp`
- Modify: `tests/llmagent/CMakeLists.txt`

- [ ] **Step 1: Write failing test `tests/llmagent/test_event_buffer.cpp`**

```cpp
#include "doctest.h"
#include "EventBuffer/RecentEventBuffer.h"

TEST_CASE("RecentEventBuffer empty bot returns empty vector") {
    RecentEventBuffer b;
    b.Configure(20);
    auto v = b.Snapshot(1);
    CHECK(v.empty());
}

TEST_CASE("RecentEventBuffer preserves insertion order") {
    RecentEventBuffer b;
    b.Configure(20);
    b.Push(1, "a");
    b.Push(1, "b");
    b.Push(1, "c");
    auto v = b.Snapshot(1);
    REQUIRE(v.size() == 3);
    CHECK(v[0] == "a");
    CHECK(v[1] == "b");
    CHECK(v[2] == "c");
}

TEST_CASE("RecentEventBuffer drops oldest when capacity exceeded") {
    RecentEventBuffer b;
    b.Configure(3);
    b.Push(1, "a");
    b.Push(1, "b");
    b.Push(1, "c");
    b.Push(1, "d");
    auto v = b.Snapshot(1);
    REQUIRE(v.size() == 3);
    CHECK(v[0] == "b");
    CHECK(v[1] == "c");
    CHECK(v[2] == "d");
}

TEST_CASE("RecentEventBuffer is per-bot") {
    RecentEventBuffer b;
    b.Configure(10);
    b.Push(1, "alpha");
    b.Push(2, "beta");
    auto v1 = b.Snapshot(1);
    auto v2 = b.Snapshot(2);
    REQUIRE(v1.size() == 1);
    REQUIRE(v2.size() == 1);
    CHECK(v1[0] == "alpha");
    CHECK(v2[0] == "beta");
}

TEST_CASE("RecentEventBuffer Clear removes bot's history") {
    RecentEventBuffer b;
    b.Configure(10);
    b.Push(1, "alpha");
    b.Clear(1);
    CHECK(b.Snapshot(1).empty());
}

TEST_CASE("RecentEventBuffer Configure 0 disables") {
    RecentEventBuffer b;
    b.Configure(0);
    b.Push(1, "alpha");
    CHECK(b.Snapshot(1).empty());
}
```

- [ ] **Step 2: Add to CMakeLists**

```cmake
add_executable(llmagent_unit_tests
  doctest_main.cpp
  test_config_load.cpp
  test_goal_parser.cpp
  test_http_client.cpp
  test_digest_shape.cpp
  test_manager_threading.cpp
  test_selector.cpp
  test_cooldown.cpp
  test_counters.cpp
  test_validator.cpp
  test_event_buffer.cpp
  ${LLMAGENT_DIR}/LlmAgentConfig.cpp
  ${LLMAGENT_DIR}/Schemas/Goal.cpp
  ${LLMAGENT_DIR}/Client/LlmHttpClient.cpp
  ${LLMAGENT_DIR}/Tiers/Tier0_StateDigest.cpp
  ${LLMAGENT_DIR}/LlmAgentManager.cpp
  ${LLMAGENT_DIR}/Selector/BotSelector.cpp
  ${LLMAGENT_DIR}/Cooldown/BotCooldownMap.cpp
  ${LLMAGENT_DIR}/Telemetry/LlmCounters.cpp
  ${LLMAGENT_DIR}/Validator/GoalValidator.cpp
  ${LLMAGENT_DIR}/Validator/ValidationContext.cpp
  ${LLMAGENT_DIR}/EventBuffer/RecentEventBuffer.cpp
)
```

- [ ] **Step 3: Run — expect FAIL**

```bash
cmake --build build/llmagent_tests 2>&1 | tail -5
```

- [ ] **Step 4: Write `src/Bot/LlmAgent/EventBuffer/RecentEventBuffer.h`**

```cpp
#ifndef _PLAYERBOT_LLMAGENT_RECENT_EVENT_BUFFER_H
#define _PLAYERBOT_LLMAGENT_RECENT_EVENT_BUFFER_H

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class RecentEventBuffer {
  public:
    void Configure(uint32_t capacity);
    void Push(uint64_t bot_guid, std::string event);
    std::vector<std::string> Snapshot(uint64_t bot_guid) const;
    void Clear(uint64_t bot_guid);
    void ClearAll();

  private:
    mutable std::mutex                                       mu_;
    uint32_t                                                 cap_ = 20;
    std::unordered_map<uint64_t, std::deque<std::string>>    buffers_;
};

#endif
```

- [ ] **Step 5: Write `src/Bot/LlmAgent/EventBuffer/RecentEventBuffer.cpp`**

```cpp
#include "EventBuffer/RecentEventBuffer.h"

void RecentEventBuffer::Configure(uint32_t capacity) {
    std::lock_guard<std::mutex> g(mu_);
    cap_ = capacity;
}

void RecentEventBuffer::Push(uint64_t bot_guid, std::string event) {
    std::lock_guard<std::mutex> g(mu_);
    if (cap_ == 0) return;
    auto& dq = buffers_[bot_guid];
    dq.push_back(std::move(event));
    while (dq.size() > cap_) dq.pop_front();
}

std::vector<std::string> RecentEventBuffer::Snapshot(uint64_t bot_guid) const {
    std::lock_guard<std::mutex> g(mu_);
    auto it = buffers_.find(bot_guid);
    if (it == buffers_.end()) return {};
    return {it->second.begin(), it->second.end()};
}

void RecentEventBuffer::Clear(uint64_t bot_guid) {
    std::lock_guard<std::mutex> g(mu_);
    buffers_.erase(bot_guid);
}

void RecentEventBuffer::ClearAll() {
    std::lock_guard<std::mutex> g(mu_);
    buffers_.clear();
}
```

- [ ] **Step 6: Build and run**

```bash
cmake --build build/llmagent_tests
./build/llmagent_tests/llmagent_unit_tests
```

Expected: 73 prior + 6 new = 79 cases pass.

- [ ] **Step 7: Commit**

```bash
git add src/Bot/LlmAgent/EventBuffer/ tests/llmagent/test_event_buffer.cpp tests/llmagent/CMakeLists.txt
git commit -m "feat(llm-agent): RecentEventBuffer — per-bot ring buffer for event_log

std::deque + size cap. Per-bot, thread-safe via single mutex (low
contention expected: ~1 push per bot per few seconds). Configure
with cap=0 disables (Push becomes no-op)."
```

---

## Task 7: Wire `BotSelector`, `BotCooldownMap`, `RecentEventBuffer`, `LlmCounters` into `LlmAgentManager`

**Files:**
- Modify: `src/Bot/LlmAgent/LlmAgentManager.h`
- Modify: `src/Bot/LlmAgent/LlmAgentManager.cpp`

No new tests in this task — the existing `test_manager_threading.cpp` keeps passing, and Tasks 11+ exercise the integration.

- [ ] **Step 1: Update `src/Bot/LlmAgent/LlmAgentManager.h`**

Replace the existing `class LlmAgentManager` declaration with this extended version. Keep all the Phase 1 includes; add three new ones at the top:

```cpp
#include "Selector/BotSelector.h"
#include "Cooldown/BotCooldownMap.h"
#include "EventBuffer/RecentEventBuffer.h"
#include "Telemetry/LlmCounters.h"
```

Inside the class, add these as members (alongside the existing ones) right before the `private:` section's existing members:

```cpp
  public:
    BotSelector&        Selector()        { return selector_; }
    BotCooldownMap&     Cooldowns()       { return cooldowns_; }
    RecentEventBuffer&  Events()          { return events_; }
    LlmCounters&        Counters()        { return counters_; }
    LlmApplyMode        ApplyMode() const { return cfg_.ApplyMode; }

  private:
    BotSelector         selector_;
    BotCooldownMap      cooldowns_;
    RecentEventBuffer   events_;
    LlmCounters         counters_;
```

(Place those accessor methods in the existing `public:` section, immediately after `const LlmAgentConfig& Config() const`.)

- [ ] **Step 2: Update `LlmAgentManager::Start(LlmAgentConfig)` in the .cpp**

After the existing `cfg_ = std::move(cfg);` and before the worker thread spawn loop, add:

```cpp
    // Phase 2 component config
    selector_.Configure(cfg_.SamplePct, cfg_.SocialOptIn);
    events_.Configure(cfg_.EventLogSize);
    // cooldowns_ and counters_ are stateful — leave them; they survive
    // a re-Start (intentional: counters from previous lifetime carry over
    // until Shutdown).
```

- [ ] **Step 3: Update `LlmAgentManager::Shutdown()` to dump counters and clear state**

Inside `Shutdown()`, right before the existing `if (jsonl_.is_open()) jsonl_.close();`, add:

```cpp
    counters_.DumpToLog();
    selector_.Clear();
    cooldowns_.Clear();
    events_.ClearAll();
```

- [ ] **Step 4: Update `LlmAgentManager::HandleRequest` to increment counters**

In `HandleRequest`, immediately after computing `result.parsed_status` (the final assignment), insert:

```cpp
    counters_.IncStatus(result.parsed_status);
```

There's only one final value of `parsed_status` per request, so a single increment site at the bottom of the if/else chain (just before the JSONL append) is correct.

Also at the very top of `HandleRequest`, before `auto t_dispatch = ...`, this is already where the request was dequeued. We don't add `IncEnqueued` here — that fires from `Enqueue` itself.

- [ ] **Step 5: Update `LlmAgentManager::Enqueue` to increment the enqueue counter**

Inside `Enqueue`, after the successful in-flight insert (before `queue_.push_back`), add:

```cpp
    counters_.IncEnqueued();
```

- [ ] **Step 6: Build unit tests**

```bash
cmake --build build/llmagent_tests
./build/llmagent_tests/llmagent_unit_tests
```

Expected: 79 cases still pass. The existing `test_manager_threading.cpp` doesn't yet check counters; that's fine — it's covered in Task 11.

- [ ] **Step 7: Commit**

```bash
git add src/Bot/LlmAgent/LlmAgentManager.h src/Bot/LlmAgent/LlmAgentManager.cpp
git commit -m "feat(llm-agent): wire Selector/Cooldown/Events/Counters into Manager

The four new components become members of LlmAgentManager. Start
configures them; Shutdown dumps counters to log and clears the
opt-in / event / cooldown state.

Enqueue increments enqueued_total. HandleRequest increments the
parsed_status counter once per finished request. Apply-side counters
(applied/shadow_accepted/etc.) fire from the action in Task 9."
```

---

## Task 8: Enrich `Tier0_StateDigest` with `event_log` + social fields

**Files:**
- Modify: `src/Bot/LlmAgent/Tiers/Tier0_StateDigest.h`
- Modify: `src/Bot/LlmAgent/Tiers/Tier0_StateDigest.cpp`
- Modify: `tests/llmagent/test_digest_shape.cpp`

The header already declares fields for `event_log`, `social.nearby_humans`, and `social.recent_whispers`. We just need to (a) wire SnapshotBot to pull from `LlmAgentManager::Events().Snapshot(...)` and the existing nearby-player scan, and (b) extend the digest-shape tests for the wider fixtures.

- [ ] **Step 1: Extend `tests/llmagent/test_digest_shape.cpp` with two new cases**

Append at the bottom:

```cpp
TEST_CASE("BuildDigestJson event_log preserves order and content") {
    LlmBotState s;
    s.event_log = {"killed Murloc (+50 xp)", "received whisper from RealBob: hi"};
    auto j = BuildDigestJson(s);
    REQUIRE(j["event_log"].is_array());
    REQUIRE(j["event_log"].size() == 2);
    CHECK(j["event_log"][0].get<std::string>() == "killed Murloc (+50 xp)");
    CHECK(j["event_log"][1].get<std::string>() == "received whisper from RealBob: hi");
}

TEST_CASE("BuildDigestJson recent_whispers shape") {
    LlmBotState s;
    s.social.recent_whispers.push_back({"RealBob", "wanna group?", 5});
    s.social.recent_whispers.push_back({"RealJess", "ill heal you", 12});
    auto j = BuildDigestJson(s);
    REQUIRE(j["social"]["recent_whispers"].is_array());
    REQUIRE(j["social"]["recent_whispers"].size() == 2);
    CHECK(j["social"]["recent_whispers"][0]["from"].get<std::string>() == "RealBob");
    CHECK(j["social"]["recent_whispers"][0]["text"].get<std::string>() == "wanna group?");
    CHECK(j["social"]["recent_whispers"][0]["age_s"].get<int>() == 5);
}
```

- [ ] **Step 2: Run — expect PASS already (the JSON shape was wired in Phase 1)**

```bash
cmake --build build/llmagent_tests
./build/llmagent_tests/llmagent_unit_tests
```

Expected: 81 cases pass. If they fail, the digest JSON shape needs a fix — refer to Phase 1's existing `BuildDigestJson` implementation in `Tier0_StateDigest.cpp`.

- [ ] **Step 3: Update `SnapshotBot` in `Tier0_StateDigest.cpp` to fill the event_log**

Locate the `// Social, event_log: leave empty for Phase 1. Phase 2 wires these.` comment near the end of `SnapshotBot`. Replace from that comment to the closing brace of the function with:

```cpp
    // ===== Phase 2 enrichment =====

    // event_log: pull from LlmAgentManager's per-bot ring buffer.
    s.event_log = LlmAgentManager::Instance().Events().Snapshot(
        bot->GetGUID().GetRawValue());

    // social.nearby_humans: scan players in bot's grid via existing
    // AI helper. Filter out other playerbots. Cap at 5 by distance.
    {
        std::list<Player*> players;
        bot->GetPlayerListInGrid(players, 50.0f);
        std::vector<std::pair<float, Player*>> humans;
        for (Player* p : players) {
            if (!p || p == bot) continue;
            if (sPlayerbotsMgr->GetPlayerbotAI(p) != nullptr) continue;  // skip bots
            humans.emplace_back(bot->GetDistance(p), p);
        }
        std::sort(humans.begin(), humans.end(),
                  [](auto& a, auto& b){ return a.first < b.first; });
        for (size_t i = 0; i < humans.size() && i < 5; ++i) {
            NearbyHuman nh;
            nh.name     = humans[i].second->GetName();
            nh.level    = humans[i].second->GetLevel();
            nh.distance = humans[i].first;
            s.social.nearby_humans.push_back(std::move(nh));
        }
    }

    // social.recent_whispers: pulled from the same event_log ring buffer's
    // whisper-tagged subset. Phase 2 keeps this simple — whispers also
    // appear in event_log; the structured form is populated by the same
    // whisper hook in Task 9 via a separate sliding window kept inside
    // LlmAgentManager. For now, leave empty; the hook in Task 9 fills it.

    return s;
}
```

Add the necessary includes near the top of the worldserver-only block (after the existing `#include "QuestDef.h"`):

```cpp
#include "LlmAgentManager.h"
#include "PlayerbotsMgr.h"
#include <algorithm>
#include <list>
#include <utility>
```

(`sPlayerbotsMgr` is the existing global accessor — see usage at
`src/Script/Playerbots.cpp:399` for the pattern. `GetPlayerListInGrid`
is an AzerothCore `WorldObject` method.)

- [ ] **Step 4: Build unit tests — still 81 pass**

```bash
cmake --build build/llmagent_tests
./build/llmagent_tests/llmagent_unit_tests
```

The `#ifndef LLMAGENT_UNIT_TESTS` guard hides the worldserver-touching changes from the unit-test build.

- [ ] **Step 5: Commit**

```bash
git add src/Bot/LlmAgent/Tiers/Tier0_StateDigest.cpp tests/llmagent/test_digest_shape.cpp
git commit -m "feat(llm-agent): Tier0 digest pulls event_log + nearby_humans

SnapshotBot now fills event_log from RecentEventBuffer and
social.nearby_humans from the existing grid-scan helper. Whispers
populate event_log via the Phase 2 whisper hook; the structured
recent_whispers field is filled by the same hook in the next task."
```

---

## Task 9: Whisper + kill hooks (RecentEventBuffer + recent_whispers + social opt-in)

**Files:**
- Create: `src/Bot/LlmAgent/Hooks/LlmAgentHooks.h`
- Create: `src/Bot/LlmAgent/Hooks/LlmAgentHooks.cpp`
- Modify: `src/Bot/PlayerbotAI.cpp` (one line in `HandleCommand`)
- Modify: `src/Script/Playerbots.cpp` (one line in `OnPlayerbotCheckKillTask`)

No unit tests — these are pure worldserver-side glue. Smoke-tested in Task 14.

- [ ] **Step 1: Write `src/Bot/LlmAgent/Hooks/LlmAgentHooks.h`**

```cpp
#ifndef _PLAYERBOT_LLMAGENT_HOOKS_H
#define _PLAYERBOT_LLMAGENT_HOOKS_H

#include <cstdint>
#include <string>

class Player;

namespace LlmAgentHooks {

// Whisper received by a bot from any source. If the sender is a real player,
// the bot is opted in to LLM and the whisper is appended to event_log +
// recent_whispers (the latter via the manager's whisper sliding window).
void OnWhisperReceived(Player* bot, Player* sender, const std::string& text);

// Bot kill task fires for every monster death the bot participated in. We
// record an event_log entry for it.
void OnKill(Player* bot, const std::string& victim_name);

}  // namespace LlmAgentHooks

#endif
```

- [ ] **Step 2: Write `src/Bot/LlmAgent/Hooks/LlmAgentHooks.cpp`**

```cpp
#include "Hooks/LlmAgentHooks.h"
#include "LlmAgentManager.h"

#ifndef LLMAGENT_UNIT_TESTS
#include "Player.h"
#include "PlayerbotsMgr.h"
#endif

#include <sstream>

namespace {
constexpr size_t kMaxWhisperChars = 80;

std::string truncate_whisper(const std::string& s) {
    return s.size() <= kMaxWhisperChars ? s : s.substr(0, kMaxWhisperChars) + "…";
}
}  // namespace

namespace LlmAgentHooks {

void OnWhisperReceived(Player* bot, Player* sender, const std::string& text) {
#ifndef LLMAGENT_UNIT_TESTS
    if (!bot || !sender) return;
    if (sPlayerbotsMgr->GetPlayerbotAI(sender) != nullptr) return;  // bot-to-bot, ignore

    auto& mgr = LlmAgentManager::Instance();
    if (!mgr.Enabled()) return;

    const uint64_t bot_guid = bot->GetGUID().GetRawValue();

    // Social opt-in.
    if (mgr.Config().SocialOptIn) {
        mgr.Selector().OptInBot(bot_guid);
    }

    // event_log entry.
    std::ostringstream ev;
    ev << "received whisper from " << sender->GetName() << ": " << truncate_whisper(text);
    mgr.Events().Push(bot_guid, ev.str());
#endif
}

void OnKill(Player* bot, const std::string& victim_name) {
#ifndef LLMAGENT_UNIT_TESTS
    if (!bot) return;
    auto& mgr = LlmAgentManager::Instance();
    if (!mgr.Enabled()) return;

    const uint64_t bot_guid = bot->GetGUID().GetRawValue();
    mgr.Events().Push(bot_guid, "killed " + victim_name);
#endif
}

}  // namespace LlmAgentHooks
```

(`recent_whispers` as a structured field is intentionally left empty in
Phase 2. The event_log already captures whisper content; if the LLM needs
the structured form we'll add a second sliding window in Phase 3. This
saves a per-bot state field.)

- [ ] **Step 3: Add the whisper hook call in `src/Bot/PlayerbotAI.cpp`**

Add this include near the top with the other includes:

```cpp
#include "Bot/LlmAgent/Hooks/LlmAgentHooks.h"
```

Locate `void PlayerbotAI::HandleCommand(uint32 type, const std::string& text, Player& fromPlayer, const uint32 lang)` (around line 591). Immediately at the top of the function body (before any existing logic), insert:

```cpp
    LlmAgentHooks::OnWhisperReceived(bot, &fromPlayer, text);
```

- [ ] **Step 4: Add the kill hook call in `src/Script/Playerbots.cpp`**

Add include near the top:

```cpp
#include "Bot/LlmAgent/Hooks/LlmAgentHooks.h"
```

Locate `void OnPlayerbotCheckKillTask(Player* player, Unit* victim) override` around line 409. At the top of the function body, insert:

```cpp
    if (player && victim) {
        std::string victim_name = victim->GetName();
        if (victim_name.empty()) victim_name = "unknown";
        LlmAgentHooks::OnKill(player, victim_name);
    }
```

- [ ] **Step 5: Build unit tests — still 81 pass**

```bash
cmake --build build/llmagent_tests
./build/llmagent_tests/llmagent_unit_tests
```

The hooks file has the `#ifndef LLMAGENT_UNIT_TESTS` guard internally — the unit-test target compiles it as a no-op body.

- [ ] **Step 6: Commit**

```bash
git add src/Bot/LlmAgent/Hooks/ src/Bot/PlayerbotAI.cpp src/Script/Playerbots.cpp
git commit -m "feat(llm-agent): whisper + kill hooks → opt-in + event_log

Hooks live in src/Bot/LlmAgent/Hooks/LlmAgentHooks.{h,cpp}, called
from PlayerbotAI::HandleCommand (whisper seam) and the existing
OnPlayerbotCheckKillTask script hook (kill seam).

Whisper from a real player → BotSelector::OptInBot + event_log entry.
Kill → event_log entry.

Each insertion is one line at the seam plus an include. Worldserver-
only; unit-test target sees no-op bodies via LLMAGENT_UNIT_TESTS."
```

---

## Task 10: Apply helper — `ApplyGoalToRpgInfo`

**Files:**
- Create: `src/Bot/LlmAgent/Apply/ApplyGoal.h`
- Create: `src/Bot/LlmAgent/Apply/ApplyGoal.cpp`

Worldserver-only; no unit tests. Touches `NewRpgInfo::ChangeTo*`.

- [ ] **Step 1: Write `src/Bot/LlmAgent/Apply/ApplyGoal.h`**

```cpp
#ifndef _PLAYERBOT_LLMAGENT_APPLY_GOAL_H
#define _PLAYERBOT_LLMAGENT_APPLY_GOAL_H

#include "Schemas/Goal.h"

class PlayerbotAI;

namespace LlmAgentApply {

// Returns true on successful apply, false on exception (and recovers the
// bot to Idle so it never stays in a corrupt state).
bool ApplyGoalToRpgInfo(const ParsedGoal& g, PlayerbotAI* botAI);

}  // namespace LlmAgentApply

#endif
```

- [ ] **Step 2: Write `src/Bot/LlmAgent/Apply/ApplyGoal.cpp`**

```cpp
#include "Apply/ApplyGoal.h"

#ifndef LLMAGENT_UNIT_TESTS

#include "PlayerbotAI.h"
#include "ObjectMgr.h"
#include "NewRpgInfo.h"
#include "Log.h"

namespace LlmAgentApply {

bool ApplyGoalToRpgInfo(const ParsedGoal& g, PlayerbotAI* botAI) {
    if (!botAI) return false;
    Player* bot = botAI->GetBot();
    if (!bot) return false;
    NewRpgInfo& info = botAI->rpgInfo;

    try {
        switch (g.goal) {
            case GoalKind::Idle:
                info.ChangeToIdle();
                return true;

            case GoalKind::Rest:
                info.ChangeToRest();
                return true;

            case GoalKind::WanderRandom:
                info.ChangeToWanderRandom();
                return true;

            case GoalKind::WanderNpc:
                info.ChangeToWanderNpc();
                return true;

            case GoalKind::DoQuest: {
                const auto& p = std::get<DoQuestParams>(g.params);
                const Quest* q = sObjectMgr->GetQuestTemplate(p.quest_id);
                if (!q) {
                    info.ChangeToIdle();
                    return false;
                }
                info.ChangeToDoQuest(p.quest_id, q);
                return true;
            }

            case GoalKind::GoGrind: {
                const auto& p = std::get<GoGrindParams>(g.params);
                WorldPosition wp(p.map_id, static_cast<float>(p.x),
                                 static_cast<float>(p.y),
                                 static_cast<float>(p.z), 0.0f);
                info.ChangeToGoGrind(wp);
                return true;
            }

            case GoalKind::GoCamp: {
                const auto& p = std::get<GoCampParams>(g.params);
                WorldPosition wp(p.map_id, static_cast<float>(p.x),
                                 static_cast<float>(p.y),
                                 static_cast<float>(p.z), 0.0f);
                info.ChangeToGoCamp(wp);
                return true;
            }

            case GoalKind::TravelFlight: {
                const auto& p = std::get<TravelFlightParams>(g.params);
                std::vector<uint32> path = {p.destination_node_id};
                info.ChangeToTravelFlight(ObjectGuid(uint64(p.from_flightmaster_guid)), path);
                return true;
            }

            case GoalKind::OutdoorPvp: {
                const auto& p = std::get<OutdoorPvpParams>(g.params);
                info.ChangeToOutdoorPvp(p.capture_point_spawn_id);
                return true;
            }
        }
    } catch (const std::exception& e) {
        LOG_ERROR("playerbots", "[LlmAgent] ApplyGoal threw: {} (recovering to Idle)", e.what());
        info.ChangeToIdle();
        return false;
    } catch (...) {
        LOG_ERROR("playerbots", "[LlmAgent] ApplyGoal threw unknown exception (recovering to Idle)");
        info.ChangeToIdle();
        return false;
    }
    return false;
}

}  // namespace LlmAgentApply

#endif
```

- [ ] **Step 3: Build unit tests — still 81 pass**

```bash
cmake --build build/llmagent_tests
./build/llmagent_tests/llmagent_unit_tests
```

(File is guarded; not compiled into the unit-test target. We could add
a stub body for unit tests, but no caller exercises it from unit tests
either, so leave it.)

- [ ] **Step 4: Commit**

```bash
git add src/Bot/LlmAgent/Apply/
git commit -m "feat(llm-agent): ApplyGoalToRpgInfo — variant dispatcher

One ChangeTo* call per variant. Wrapped in try/catch — on any throw,
the bot is reset to Idle so it never stays in a corrupt state.
LOG_ERROR records the throw + recovery path."
```

---

## Task 11: Apply-mode decision tree in `LlmReplanIdleAction`

**Files:**
- Modify: `src/Bot/LlmAgent/Actions/LlmReplanIdleAction.cpp`
- Modify: `src/Bot/LlmAgent/Triggers/LlmReplanIdleTrigger.cpp`
- Modify: `tests/llmagent/test_manager_threading.cpp`

The action's `Execute` grows substantially. The trigger now also gates
on `selector.IsLlmBot` AND `cooldown.Eligible`.

- [ ] **Step 1: Replace `LlmReplanIdleTrigger::IsActive` body**

In `src/Bot/LlmAgent/Triggers/LlmReplanIdleTrigger.cpp`, replace the function with:

```cpp
bool LlmReplanIdleTrigger::IsActive() {
    if (!botAI) return false;
    auto& mgr = LlmAgentManager::Instance();
    if (!mgr.Enabled()) return false;
    Player* bot = botAI->GetBot();
    if (!bot) return false;
    uint64_t guid = bot->GetGUID().GetRawValue();

    // Always allow draining results that are already on the stack.
    if (mgr.HasPendingResults(guid)) return true;

    // Enqueue path requires sample/opt-in AND eligible cooldown AND Idle AND not in-flight.
    if (!mgr.Selector().IsLlmBot(guid)) return false;
    if (!mgr.Cooldowns().Eligible(guid)) return false;
    if (botAI->rpgInfo.GetStatus() != RPG_IDLE) return false;
    if (mgr.IsInFlight(guid)) return false;
    return true;
}
```

- [ ] **Step 2: Replace `LlmReplanIdleAction::Execute` body**

In `src/Bot/LlmAgent/Actions/LlmReplanIdleAction.cpp`, add includes:

```cpp
#include "Validator/GoalValidator.h"
#include "Validator/ValidationContext.h"
#include "Apply/ApplyGoal.h"
```

Then replace `Execute(Event ...)` with:

```cpp
namespace {
GoalKind kind_from_json_str(const std::string& s) {
    if (s == "idle")          return GoalKind::Idle;
    if (s == "go_grind")      return GoalKind::GoGrind;
    if (s == "go_camp")       return GoalKind::GoCamp;
    if (s == "wander_npc")    return GoalKind::WanderNpc;
    if (s == "wander_random") return GoalKind::WanderRandom;
    if (s == "do_quest")      return GoalKind::DoQuest;
    if (s == "travel_flight") return GoalKind::TravelFlight;
    if (s == "rest")          return GoalKind::Rest;
    if (s == "outdoor_pvp")   return GoalKind::OutdoorPvp;
    return GoalKind::Idle;
}
}  // namespace

bool LlmReplanIdleAction::Execute(Event /*event*/) {
    if (!botAI) return false;
    auto& mgr = LlmAgentManager::Instance();
    if (!mgr.Enabled()) return false;
    Player* bot = botAI->GetBot();
    if (!bot) return false;
    const uint64_t guid = bot->GetGUID().GetRawValue();
    const LlmAgentConfig& cfg = mgr.Config();

    bool applied_this_tick = false;

    // 1. Drain results — validate, apply / shadow / log per ApplyMode.
    auto results = mgr.DrainResults(guid);
    for (const auto& r : results) {
        if (r.parsed_status != "ok") {
            mgr.Counters().IncFallbackUsed();
            mgr.Cooldowns().Set(
                guid,
                std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(cfg.FallbackCooldownMs));
            continue;
        }

        // Re-parse the parsed_goal JSON back into a ParsedGoal to give the
        // validator its strongly-typed input. Cheap (small object).
        ParsedGoal goal;
        try {
            goal.goal = kind_from_json_str(r.parsed_goal.value("goal", std::string{"idle"}));
            goal.ttl_minutes = r.parsed_goal.value("ttl_minutes", uint32_t{5});
            goal.reasoning   = r.parsed_goal.value("reasoning", std::string{});
            // params are not strictly needed for log mode; for apply we need them.
            // Pull them through the same parser as the worker.
            // The schema is the JSON object; we can recover params from the
            // raw response if needed. For now: keep defaults for variants that
            // don't carry params (Idle/Rest/WanderRandom).
            switch (goal.goal) {
                case GoalKind::Idle:         goal.params = IdleParams{}; break;
                case GoalKind::Rest:         goal.params = RestParams{}; break;
                case GoalKind::WanderRandom: goal.params = WanderRandomParams{}; break;
                default: {
                    // For richer variants, we recover params from raw_response.
                    auto raw = nlohmann::json::parse(r.raw_response, nullptr, false);
                    if (!raw.is_discarded() && raw.contains("params")) {
                        // Use the existing ParseAndValidate to fill params.
                        auto parsed = ParseAndValidate(r.raw_response);
                        if (std::holds_alternative<ParsedGoal>(parsed)) {
                            goal = std::get<ParsedGoal>(parsed);
                        } else {
                            // Treat as schema_error after the fact.
                            mgr.Counters().IncFallbackUsed();
                            mgr.Cooldowns().Set(
                                guid,
                                std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(cfg.FallbackCooldownMs));
                            continue;
                        }
                    } else {
                        mgr.Counters().IncFallbackUsed();
                        mgr.Cooldowns().Set(
                            guid,
                            std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(cfg.FallbackCooldownMs));
                        continue;
                    }
                    break;
                }
            }
        } catch (...) {
            mgr.Counters().IncFallbackUsed();
            mgr.Cooldowns().Set(
                guid,
                std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(cfg.FallbackCooldownMs));
            continue;
        }

        BotValidationContext vctx = SnapshotForValidation(botAI);
        ValidationResult vr = ValidateGoalDecision(goal, vctx);

        if (vr.accepted) mgr.Counters().IncValidatorAccepted();
        else             mgr.Counters().IncValidatorRejected(vr.reject_reason);

        if (vr.accepted && cfg.ApplyMode == LlmApplyMode::Apply) {
            bool ok = LlmAgentApply::ApplyGoalToRpgInfo(goal, botAI);
            if (ok) {
                mgr.Counters().IncApplied();
                applied_this_tick = true;
                auto cool = std::chrono::steady_clock::now() +
                            std::chrono::minutes(
                                std::min<uint32_t>(goal.ttl_minutes,
                                                   cfg.MaxCooldownMinutes));
                mgr.Cooldowns().Set(guid, cool);
            } else {
                mgr.Counters().IncAppliedThrew();
                mgr.Cooldowns().Set(
                    guid,
                    std::chrono::steady_clock::now() +
                      std::chrono::milliseconds(cfg.FallbackCooldownMs));
            }
        } else if (vr.accepted && cfg.ApplyMode == LlmApplyMode::Shadow) {
            mgr.Counters().IncShadowAccepted();
            LOG_INFO("playerbots",
                     "[LlmAgent shadow] bot={} would_have_applied={} lat={}ms",
                     r.bot_name,
                     r.parsed_goal.value("goal", std::string{"?"}),
                     r.inference_ms);
            auto cool = std::chrono::steady_clock::now() +
                        std::chrono::minutes(
                            std::min<uint32_t>(goal.ttl_minutes,
                                               cfg.MaxCooldownMinutes));
            mgr.Cooldowns().Set(guid, cool);
        } else if (vr.accepted && cfg.ApplyMode == LlmApplyMode::Log) {
            mgr.Counters().IncLogAcceptedSkipped();
            mgr.Cooldowns().Set(
                guid,
                std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(cfg.FallbackCooldownMs));
        } else {
            mgr.Counters().IncFallbackUsed();
            mgr.Cooldowns().Set(
                guid,
                std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(cfg.FallbackCooldownMs));
        }
    }

    // 2. Enqueue if eligible AND we didn't already apply this tick.
    if (!applied_this_tick &&
        mgr.Selector().IsLlmBot(guid) &&
        mgr.Cooldowns().Eligible(guid) &&
        botAI->rpgInfo.GetStatus() == RPG_IDLE &&
        !mgr.IsInFlight(guid))
    {
        LlmBotState state;
        try {
            state = SnapshotBot(botAI);
        } catch (...) {
            LOG_WARN("playerbots", "[LlmAgent] SnapshotBot threw; skipping enqueue");
            return applied_this_tick;
        }
        nlohmann::json digest = BuildDigestJson(state);

        nlohmann::json body;
        body["model"] = cfg.Model;
        body["messages"] = nlohmann::json::array();
        body["messages"].push_back({{"role", "system"}, {"content", cfg.SystemPrompt}});
        body["messages"].push_back({{"role", "user"},   {"content", digest.dump()}});
        body["response_format"] = {
            {"type", "json_schema"},
            {"json_schema", {{"name", "BotGoal"}, {"schema", nlohmann::json::parse(kGoalSchemaJson)}}}
        };
        body["temperature"] = 0.4;
        body["max_tokens"] = 256;

        LlmRequest req;
        req.bot_guid    = guid;
        req.bot_name    = bot->GetName();
        req.body_json   = body.dump();
        req.digest_json = std::move(digest);
        mgr.Enqueue(std::move(req));
    }

    return applied_this_tick;  // true suppresses status_update for this tick
}
```

- [ ] **Step 3: Add two cases to `tests/llmagent/test_manager_threading.cpp`**

Append at the bottom:

```cpp
TEST_CASE("LlmAgentManager Cooldowns().Set blocks Eligible()") {
    LlmAgentManager mgr;
    LlmAgentConfig cfg;
    cfg.Enabled = false;  // we only need the cooldown component
    mgr.Start(cfg);
    mgr.Cooldowns().Set(42, std::chrono::steady_clock::now() + std::chrono::seconds(60));
    CHECK(mgr.Cooldowns().Eligible(42) == false);
    CHECK(mgr.Cooldowns().Eligible(43) == true);
    mgr.Shutdown();
}

TEST_CASE("LlmAgentManager Selector().Configure + OptInBot interplay") {
    LlmAgentManager mgr;
    LlmAgentConfig cfg;
    cfg.Enabled = false;
    cfg.SamplePct = 0;
    cfg.SocialOptIn = true;
    mgr.Start(cfg);
    CHECK(mgr.Selector().IsLlmBot(100) == false);
    mgr.Selector().OptInBot(100);
    CHECK(mgr.Selector().IsLlmBot(100) == true);
    mgr.Shutdown();
}
```

- [ ] **Step 4: Build unit tests**

```bash
cmake --build build/llmagent_tests
./build/llmagent_tests/llmagent_unit_tests
```

Expected: 81 prior + 2 new = 83 cases pass.

- [ ] **Step 5: Commit**

```bash
git add src/Bot/LlmAgent/Actions/LlmReplanIdleAction.cpp \
        src/Bot/LlmAgent/Triggers/LlmReplanIdleTrigger.cpp \
        tests/llmagent/test_manager_threading.cpp
git commit -m "feat(llm-agent): apply-mode decision tree in LlmReplanIdleAction

Drain step now validates and acts per ApplyMode: apply runs
ApplyGoalToRpgInfo + returns true to suppress status_update; shadow
records 'would have applied' + sets cooldown; log records counter
and sets fallback cooldown.

Trigger now gates enqueue path on selector + cooldown. Drain path
still fires whenever results exist.

Two new manager-threading tests verify cooldown + selector wiring."
```

---

## Task 12: Append Phase 2 config keys to `conf/playerbots.conf.dist`

**Files:**
- Modify: `conf/playerbots.conf.dist`

- [ ] **Step 1: Locate the existing Phase 1 LLM AGENT block in `conf/playerbots.conf.dist`**

It should start with a header similar to:

```
# LLM AGENT (Phase 1 — plumbing spike)
```

- [ ] **Step 2: Append the Phase 2 block right after the Phase 1 block**

```ini

####################################################################################################
# LLM AGENT (Phase 2 — validator + apply path)
#
# Phase 2 adds the apply path: when the LLM's proposed transition validates,
# we actually drive NewRpgInfo. Default ApplyMode = "log" preserves Phase 1
# behavior. Use "shadow" for an intermediate safety mode that records what
# WOULD have been applied without changing bot behavior. Use "apply" for
# production.

# ApplyMode: log | shadow | apply
# Default: log
AiPlayerbot.LlmAgent.ApplyMode = "log"

# Percentage of bots (selected by GUID hash) that get the LLM path.
# 0 = no bots LLM-enhanced. 100 = all bots. Combine with SocialOptIn = 1
# to additionally enable any bot a real player whispers or party-invites.
# Default: 0
AiPlayerbot.LlmAgent.SamplePct = 0

# Auto-opt-in bots that real players whisper or invite to group.
# Default: 1
AiPlayerbot.LlmAgent.SocialOptIn = 1

# Cap on the LLM's ttl_minutes value when computing apply-side cooldown.
# Default: 60
AiPlayerbot.LlmAgent.MaxCooldownMinutes = 60

# Cooldown applied after any non-accepted outcome (transport error,
# schema error, validator reject, timeout).
# Default: 300000 (5 minutes)
AiPlayerbot.LlmAgent.FallbackCooldownMs = 300000

# Ring buffer length for the digest's event_log field. 0 disables.
# Default: 20
AiPlayerbot.LlmAgent.EventLogSize = 20
```

- [ ] **Step 3: Build unit tests as a sanity check**

```bash
cmake --build build/llmagent_tests
./build/llmagent_tests/llmagent_unit_tests
```

Expected: 83 cases pass. Conf changes don't affect unit tests; this is a make-sure-nothing-broke step.

- [ ] **Step 4: Commit**

```bash
git add conf/playerbots.conf.dist
git commit -m "feat(llm-agent): playerbots.conf.dist gets six Phase 2 keys

ApplyMode, SamplePct, SocialOptIn, MaxCooldownMinutes,
FallbackCooldownMs, EventLogSize. All documented with defaults that
preserve Phase 1 behavior (ApplyMode=log, SamplePct=0)."
```

---

## Task 13: Build worldserver on Heimdal

**Files:** none (execution).

- [ ] **Step 1: Push branch**

```bash
git push -u origin claude/llm-agent-phase-2-validator-apply
```

- [ ] **Step 2: Trigger the rebuild**

```bash
cd ~/Documents/Projects/azerothcore-heimdal && \
  PLAYERBOTS_BRANCH=claude/llm-agent-phase-2-validator-apply ./image/build.sh 2>&1 | \
  tee /tmp/wow-build-llm-phase2.log
```

Expected: ~3-8 min with warm ccache. Final line should be
`==> Done. Image: wow-server:phase0-YYYYMMDD (also tagged :current)`.

- [ ] **Step 3: If the build fails, surface and fix specific errors**

The most likely failure surface is the apply-side code that touches
AzerothCore APIs (`ChangeToDoQuest`, `WorldPosition`, etc.).
Per-error workflow:
1. Identify the symbol the compiler can't find.
2. `grep` for the correct upstream name in mod-playerbots / AC sources.
3. Fix in the one file. `git commit` with a clear `fix(llm-agent): …`
   message.
4. `git push` and re-run Step 2.

- [ ] **Step 4: Unit tests on Heimdal**

```bash
ssh heimdal '
  cd ~/wow-build-llm-phase2-tests && rm -rf * 2>/dev/null
  mkdir -p ~/wow-build-llm-phase2-tests && cd ~/wow-build-llm-phase2-tests
  git clone --depth 1 -b claude/llm-agent-phase-2-validator-apply \
    https://github.com/jarlbrak/mod-playerbots.git
  cd mod-playerbots
  cmake -S tests/llmagent -B build/llmagent_tests
  cmake --build build/llmagent_tests -j
  ./build/llmagent_tests/llmagent_unit_tests'
```

Expected: `[doctest] Status: SUCCESS!` with 83 cases.

Skip this step if the cmake / g++ toolchain isn't readily available on
Heimdal's brackin user (the unit tests already passed on the Mac with
the same compiler family — Heimdal-side verification is a nice-to-have).

---

## Task 14: Smoke test on Heimdal — three runs

**Files:** `results/2026-05-13-llm-phase-2-smoke/summary.md` (created in
Step 5).

### Step 1: Confirm llama-server up; rebind port if it regressed

```bash
ssh heimdal 'systemctl is-active llama-server.service'
ssh heimdal 'echo "101423" | sudo -S ss -tnlp | grep ":8080"'
```

Expected: `active`; bound on `0.0.0.0:8080` (from Phase 1's fix).

If bound on `127.0.0.1:8080`, re-apply Phase 1's fix:

```bash
ssh heimdal 'echo "101423" | sudo -S sed -i "s|PublishPort=127.0.0.1:8080:8080|PublishPort=0.0.0.0:8080:8080|" /etc/containers/systemd/llama-server.container && echo "101423" | sudo -S systemctl daemon-reload && echo "101423" | sudo -S systemctl restart llama-server.service'
```

### Step 2: Update playerbots.conf for Run 1 (`ApplyMode=log`, `SamplePct=10`)

```bash
ssh heimdal 'echo "101423" | sudo -S bash -c "
  # Strip prior LlmAgent override block, then append fresh.
  sed -i \"/^# LLM AGENT (Phase 1.*runtime overrides/,\\$d\" /opt/containers/wow/etc/modules/playerbots.conf
  cat >> /opt/containers/wow/etc/modules/playerbots.conf <<EOF

####################################################################################################
# LLM AGENT — Phase 2 smoke Run 1 (log, 10% sample)
####################################################################################################
AiPlayerbot.LlmAgent.Enabled = 1
AiPlayerbot.LlmAgent.Endpoint = \\\"http://192.168.1.3:8080\\\"
AiPlayerbot.LlmAgent.Model = \\\"qwen2.5-7b-instruct-q4_k_m-00001-of-00002.gguf\\\"
AiPlayerbot.LlmAgent.WorkerThreads = 4
AiPlayerbot.LlmAgent.RequestTimeoutMs = 15000
AiPlayerbot.LlmAgent.JsonlPath = \\\"/azerothcore/env/dist/logs/llm_agent_phase2.jsonl\\\"
AiPlayerbot.LlmAgent.SystemPrompt = \\\"\\\"
AiPlayerbot.LlmAgent.ApplyMode = \\\"log\\\"
AiPlayerbot.LlmAgent.SamplePct = 10
AiPlayerbot.LlmAgent.SocialOptIn = 1
AiPlayerbot.LlmAgent.MaxCooldownMinutes = 60
AiPlayerbot.LlmAgent.FallbackCooldownMs = 300000
AiPlayerbot.LlmAgent.EventLogSize = 20
EOF
"'
```

### Step 3: Run 1 — restart + 2 min window

```bash
ssh heimdal 'echo "101423" | sudo -S systemctl restart wow-worldserver.service'
sleep 30
ssh heimdal 'echo "101423" | sudo -S truncate -s 0 /opt/containers/wow/logs/llm_agent_phase2.jsonl'
sleep 120
ssh heimdal 'wc -l /opt/containers/wow/logs/llm_agent_phase2.jsonl
echo "--- parsed_status ---"
jq -r .parsed_status /opt/containers/wow/logs/llm_agent_phase2.jsonl | sort | uniq -c
echo "--- inference percentiles ---"
jq -r .inference_ms /opt/containers/wow/logs/llm_agent_phase2.jsonl | sort -n | awk "{a[NR]=\$1} END {if(NR>0) print \"n=\"NR, \"p50=\"a[int(NR*0.5)], \"p95=\"a[int(NR*0.95)]; else print \"n=0\"}"
echo "--- counter dump (search Server.log) ---"
grep "LlmAgent counters" /opt/containers/wow/logs/Server.log | tail -3'
```

Expected: ~10% of Phase 1's record rate. Roughly 15-20 records in 120s
(Phase 1 saw 168 with 100% sampling effectively). All `parsed_status=ok`.
`Server.log` doesn't have counter dumps yet (those fire at Shutdown);
ignore the empty line if so.

### Step 4: Run 2 — `ApplyMode=shadow`

Switch the conf:

```bash
ssh heimdal 'echo "101423" | sudo -S sed -i "s|AiPlayerbot.LlmAgent.ApplyMode = \"log\"|AiPlayerbot.LlmAgent.ApplyMode = \"shadow\"|" /opt/containers/wow/etc/modules/playerbots.conf
echo "101423" | sudo -S systemctl restart wow-worldserver.service
sleep 30
echo "101423" | sudo -S truncate -s 0 /opt/containers/wow/logs/llm_agent_phase2.jsonl
sleep 120
echo "--- shadow run validator decisions ---"
grep "LlmAgent shadow" /opt/containers/wow/logs/Server.log | tail -10
wc -l /opt/containers/wow/logs/llm_agent_phase2.jsonl'
```

Expected: similar record count + `[LlmAgent shadow] bot=... would_have_applied=...`
lines in Server.log (logged via the action's shadow branch).

### Step 5: Run 3 — `ApplyMode=apply`, observe one bot

Switch the conf:

```bash
ssh heimdal 'echo "101423" | sudo -S sed -i "s|AiPlayerbot.LlmAgent.ApplyMode = \"shadow\"|AiPlayerbot.LlmAgent.ApplyMode = \"apply\"|" /opt/containers/wow/etc/modules/playerbots.conf
echo "101423" | sudo -S systemctl restart wow-worldserver.service
sleep 30'
```

Now log into the WoW client as the GM account (per the runbook in
ninum-knowledge `kb_7bc8a9bf`), pick a bot whose GUID hashes into the
10% sample, and watch it for 15 minutes. Verify:
- It transitions states without being "stuck" in any one for >15 min.
- Server.log contains at least one `[LlmAgent counters] ...` line
  showing `applied >= 1` (dumped at Shutdown if you restart after the
  observation window).
- Update-time-diff in `server info` stays under the Phase 1 baseline:
  mean ≤ 50 ms, p95 ≤ 100 ms.

### Step 6: Capture results

Pull the JSONL + counter dump:

```bash
mkdir -p results/2026-05-13-llm-phase-2-smoke
ssh heimdal 'head -20 /opt/containers/wow/logs/llm_agent_phase2.jsonl' \
  > results/2026-05-13-llm-phase-2-smoke/sample_records.jsonl
ssh heimdal 'grep "LlmAgent counters" /opt/containers/wow/logs/Server.log | tail -1' \
  > results/2026-05-13-llm-phase-2-smoke/counters.txt
```

Write `results/2026-05-13-llm-phase-2-smoke/summary.md` following the
Phase 1 results-file shape: TL;DR + headline numbers + per-run notes +
"Findings worth recording" + outstanding follow-ups.

### Step 7: Disable LlmAgent before walking away

If the smoke run was healthy:

```bash
ssh heimdal 'echo "101423" | sudo -S sed -i "s|AiPlayerbot.LlmAgent.Enabled = 1|AiPlayerbot.LlmAgent.Enabled = 0|" /opt/containers/wow/etc/modules/playerbots.conf && echo "101423" | sudo -S systemctl restart wow-worldserver.service'
```

(Or leave on if you want to keep an eye on it. Either way, log the
choice in the results file.)

### Step 8: Commit results

```bash
git add results/2026-05-13-llm-phase-2-smoke/
git commit -m "test(llm-agent): record Phase 2 smoke-test results

Three runs: ApplyMode=log/shadow/apply at SamplePct=10. See summary.md."
git push origin claude/llm-agent-phase-2-validator-apply
```

---

## Phase 2 success criteria

All six must hold:

1. **Unit tests:** 83 / 83 (32 from Phase 1 unchanged + 51 new).
2. **`Enabled = 0`:** binary byte-identical to baseline.
3. **`ApplyMode = log`:** zero behavior change vs Phase 1.
4. **`ApplyMode = shadow`:** zero behavior change AND JSONL has the new
   shadow-only fields populated (Server.log `[LlmAgent shadow]` line per
   accepted result).
5. **`ApplyMode = apply` + `SamplePct = 10`:** at least one bot directly
   observed transitioning via an LLM-driven `ChangeTo*` call, executed
   by the existing engine.
6. **No worldserver-tick regression:** mean ≤ 50 ms, p95 ≤ 100 ms during
   the 15-min apply run.

When all six hold, Phase 2 is done. Phase 3 (memory sidecar + memory
hints) is the natural next phase per parent design §12.

---

## Plan deviations from spec

Two intentional simplifications versus the spec at
`docs/superpowers/specs/2026-05-13-llm-agent-phase-2-design.md`:

1. **JSONL decision-record fields deferred.** Spec §7 introduces
   `validator_decision`, `applied_transition`, `would_have_applied`,
   `apply_mode`, `sample_hit` on every JSONL record. The plan covers
   this signal via two simpler channels:
   - `LlmCounters` aggregates: `validator_accepted`,
     `rejected_by_reason[<reason>]`, `applied`, `shadow_accepted`,
     `log_accepted_skipped`, `fallback_used` — dumped at Shutdown.
   - Server.log per-event lines: `[LlmAgent shadow] bot=X
     would_have_applied=Y` from the action's shadow branch.

   Adding the per-record JSONL fields requires cross-thread state-passing
   (worker writes the request line; action knows the decision). YAGNI for
   Phase 2 — counters + Server.log give us the distribution data Phase 3's
   spec design needs. If Phase 2 smoke data shows we're losing signal,
   add the JSONL extension as a small Phase 2.1 follow-up.

2. **Event hooks scope reduced to whisper + kill.** Spec §5.5 lists hooks
   for whisper, kill, loot, quest-progress, group-join, group-leave. Plan
   ships only the first two (whisper + kill via the existing
   `OnPlayerbotCheckKillTask` hook and `PlayerbotAI::HandleCommand`
   seam). The other three require finding the right hook points and
   adding more `Push` calls. Each one is a small follow-up; the plan's
   apply path is fully functional with just whisper + kill (those two
   are the highest-signal events for the LLM anyway: "did a human talk
   to me?" + "am I making combat progress?").

   Add the remaining three as a Phase 2.1 follow-up if smoke data shows
   the LLM is starved for context.

Both deviations are reversible — adding them later is purely additive
and doesn't change Phase 2's interfaces.
