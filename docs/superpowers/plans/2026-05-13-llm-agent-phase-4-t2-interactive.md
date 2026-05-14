# LLM-Agent Phase 4 T2 Interactive Tool-Calling Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a T2 (interactive) decision tier that fires when a real human engages a bot (whisper, party invite, group join). T2 emits OpenAI-style tool calls — `accept_party_invite`, `leave_party`, `accept_quest`, `turn_in_quest`, `set_goal`, `vendor_junk`, `memory.remember` — that pass through per-tool validators and execute on the worldserver thread.

**Architecture:** New `Tier2_Interactive` sibling tier inside the existing `LlmAgent` module. Reuses Phase 2's worker pool + Phase 3's MemoryHttpClient. Trigger fires on `InteractionEventBuffer.HasPending(bot)`. Tools validated then applied sequentially (stop-on-reject, no rollback). Mutually exclusive with T1 per tick via relevance 16 > 15. Admin command (`.playerbots t2 inject`) lets us validate the path without needing a human in-game.

**Tech Stack:** C++17 + existing vendored cpp-httplib + nlohmann/json + doctest. No new dependencies.

**Spec:** [`docs/superpowers/specs/2026-05-13-llm-agent-phase-4-t2-interactive-design.md`](../specs/2026-05-13-llm-agent-phase-4-t2-interactive-design.md)

---

## File structure

**New files:**

```
src/Bot/LlmAgent/
  Tiers/
    Tier2_Interactive.h
    Tier2_Interactive.cpp
  Triggers/
    LlmInteractTrigger.h
    LlmInteractTrigger.cpp
  Actions/
    LlmInteractAction.h
    LlmInteractAction.cpp
  Tools/
    ToolCatalog.h
    ToolCatalog.cpp
    ToolValidators.h
    ToolValidators.cpp
    ToolExecutors.h
    ToolExecutors.cpp
    InteractionContext.h
    InteractionContext.cpp
  EventBuffer/
    InteractionEventBuffer.h
    InteractionEventBuffer.cpp
  Context/
    LlmAgentTier2ActionContext.h
    LlmAgentTier2TriggerContext.h

tests/llmagent/
  test_tool_catalog.cpp
  test_tool_parser.cpp
  test_tool_validators.cpp
  test_interaction_buffer.cpp
```

**Modified files:**

- `src/Bot/LlmAgent/LlmAgentConfig.h` — 4 new fields (`Tier2_*`)
- `src/Bot/LlmAgent/LlmAgentConfig.cpp` — load + ParseApplyMode-style helpers if any
- `src/Bot/LlmAgent/LlmAgentManager.h/cpp` — own `InteractionEventBuffer`; expose `Tier2Counters()`; new in-flight set keyed by tier
- `src/Bot/LlmAgent/Hooks/LlmAgentHooks.h/cpp` — `OnPartyInviteReceived`, `OnGroupJoined`; whisper hook also pushes to `InteractionEventBuffer`
- `src/Bot/LlmAgent/Telemetry/LlmCounters.h/cpp` — new tool counters
- `src/Bot/LlmAgent/Strategy/LlmAgentStrategy.cpp` — register T2 triggers via `LlmAgentTier2TriggerContext` and `LlmAgentTier2ActionContext`
- `src/Bot/Engine/BuildSharedActionContexts.cpp` — add `LlmAgentTier2ActionContext`
- `src/Bot/Engine/BuildSharedTriggerContexts.cpp` — add `LlmAgentTier2TriggerContext`
- `src/Bot/PlayerbotAI.cpp` — whisper hook already exists from Phase 2 (now writes to InteractionEventBuffer via updated `OnWhisperReceived`)
- `src/Script/PlayerbotCommandScript.cpp` — register `.playerbots t2 inject <bot> <text>` admin command
- `conf/playerbots.conf.dist` — 4 new keys
- `tests/llmagent/CMakeLists.txt` — add new test sources

---

## Task 0: Create feature branch + cherry-pick Phase 4 spec

**Files:** none

- [ ] **Step 1: Verify Phase 3 branch clean**

```bash
git checkout claude/llm-agent-phase-3-memory-sidecar
git status
```

Expected: clean tree on Phase 3 branch.

- [ ] **Step 2: Branch off Phase 3 tip**

```bash
git checkout -b claude/llm-agent-phase-4-t2-interactive
git status
```

Expected: `On branch claude/llm-agent-phase-4-t2-interactive`.

- [ ] **Step 3: Cherry-pick Phase 4 spec from main**

The Phase 4 spec was committed on main as `3747da10`.

```bash
git cherry-pick 3747da10
git log --oneline -3
```

Expected: top commit is the cherry-picked spec.

- [ ] **Step 4: No code commits in Task 0.**

---

## Task 1: `InteractionEventBuffer`

**Files:**
- Create: `src/Bot/LlmAgent/EventBuffer/InteractionEventBuffer.h`
- Create: `src/Bot/LlmAgent/EventBuffer/InteractionEventBuffer.cpp`
- Create: `tests/llmagent/test_interaction_buffer.cpp`
- Modify: `tests/llmagent/CMakeLists.txt`

TDD.

- [ ] **Step 1: Write failing test `tests/llmagent/test_interaction_buffer.cpp`**

```cpp
#include "doctest.h"
#include "EventBuffer/InteractionEventBuffer.h"

TEST_CASE("InteractionEventBuffer empty bot has no pending events") {
    InteractionEventBuffer b;
    CHECK(b.HasPending(123) == false);
    auto p = b.SnapshotFor(123);
    CHECK(p.pending_invites.empty());
    CHECK(p.recent_whispers.empty());
    CHECK(p.recent_group_joins.empty());
}

TEST_CASE("InteractionEventBuffer PushWhisper makes HasPending true") {
    InteractionEventBuffer b;
    b.PushWhisper(123, "RealPlayerBob", 999, "hi there", 1000);
    CHECK(b.HasPending(123) == true);
    auto p = b.SnapshotFor(123);
    REQUIRE(p.recent_whispers.size() == 1);
    CHECK(p.recent_whispers[0].from_name == "RealPlayerBob");
    CHECK(p.recent_whispers[0].text == "hi there");
    CHECK(p.recent_whispers[0].ts == 1000);
}

TEST_CASE("InteractionEventBuffer PushInvite + PushJoin both flag pending") {
    InteractionEventBuffer b;
    b.PushInvite(1, "Alice", 100, 5000);
    CHECK(b.HasPending(1));
    b.PushJoin(2, "Bob", 101, 5000);
    CHECK(b.HasPending(2));
}

TEST_CASE("InteractionEventBuffer ExpireOlderThan drops stale whispers") {
    InteractionEventBuffer b;
    b.PushWhisper(1, "A", 100, "old",   1000);
    b.PushWhisper(1, "B", 101, "fresh", 2000);
    b.ExpireOlderThan(1, /*now=*/2100, /*window_seconds=*/60);
    auto p = b.SnapshotFor(1);
    REQUIRE(p.recent_whispers.size() == 1);
    CHECK(p.recent_whispers[0].text == "fresh");
}

TEST_CASE("InteractionEventBuffer is per-bot") {
    InteractionEventBuffer b;
    b.PushWhisper(1, "X", 1, "a", 1);
    b.PushWhisper(2, "Y", 2, "b", 1);
    CHECK(b.SnapshotFor(1).recent_whispers.size() == 1);
    CHECK(b.SnapshotFor(2).recent_whispers.size() == 1);
    CHECK(b.SnapshotFor(3).recent_whispers.empty());
}

TEST_CASE("InteractionEventBuffer Clear removes a bot's events") {
    InteractionEventBuffer b;
    b.PushWhisper(1, "X", 1, "a", 1);
    b.PushInvite(1, "Y", 2, 1);
    b.Clear(1);
    CHECK(b.HasPending(1) == false);
}
```

- [ ] **Step 2: Add to CMakeLists**

Edit `tests/llmagent/CMakeLists.txt`. In `add_executable(llmagent_unit_tests ...)`, add `test_interaction_buffer.cpp` and `${LLMAGENT_DIR}/EventBuffer/InteractionEventBuffer.cpp`.

- [ ] **Step 3: Run — expect FAIL**

```bash
cmake --build build/llmagent_tests 2>&1 | tail -5
```

Expected: `Cannot find source file: InteractionEventBuffer.cpp` or missing header.

- [ ] **Step 4: Write `src/Bot/LlmAgent/EventBuffer/InteractionEventBuffer.h`**

```cpp
#ifndef _PLAYERBOT_LLMAGENT_INTERACTION_EVENT_BUFFER_H
#define _PLAYERBOT_LLMAGENT_INTERACTION_EVENT_BUFFER_H

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct PendingInvite {
    std::string from_name;
    uint64_t    from_guid = 0;
    int64_t     ts = 0;
};

struct UnhandledWhisper {
    std::string from_name;
    uint64_t    from_guid = 0;
    std::string text;
    int64_t     ts = 0;
};

struct RecentGroupJoin {
    std::string leader_name;
    uint64_t    leader_guid = 0;
    int64_t     ts = 0;
};

struct InteractionPayload {
    std::vector<PendingInvite>    pending_invites;
    std::vector<UnhandledWhisper> recent_whispers;
    std::vector<RecentGroupJoin>  recent_group_joins;
};

class InteractionEventBuffer {
  public:
    void PushWhisper(uint64_t bot_guid, std::string from_name, uint64_t from_guid,
                     std::string text, int64_t ts);
    void PushInvite (uint64_t bot_guid, std::string from_name, uint64_t from_guid,
                     int64_t ts);
    void PushJoin   (uint64_t bot_guid, std::string leader_name, uint64_t leader_guid,
                     int64_t ts);

    bool HasPending(uint64_t bot_guid) const;
    InteractionPayload SnapshotFor(uint64_t bot_guid) const;
    void ExpireOlderThan(uint64_t bot_guid, int64_t now, int64_t window_seconds);
    void Clear(uint64_t bot_guid);
    void ClearAll();

  private:
    mutable std::mutex                                                   mu_;
    std::unordered_map<uint64_t, std::vector<PendingInvite>>             invites_;
    std::unordered_map<uint64_t, std::vector<UnhandledWhisper>>          whispers_;
    std::unordered_map<uint64_t, std::vector<RecentGroupJoin>>           joins_;
};

#endif
```

- [ ] **Step 5: Write `src/Bot/LlmAgent/EventBuffer/InteractionEventBuffer.cpp`**

```cpp
#include "EventBuffer/InteractionEventBuffer.h"

void InteractionEventBuffer::PushWhisper(uint64_t bot_guid, std::string from_name,
                                          uint64_t from_guid, std::string text,
                                          int64_t ts) {
    std::lock_guard<std::mutex> g(mu_);
    whispers_[bot_guid].push_back({std::move(from_name), from_guid, std::move(text), ts});
}

void InteractionEventBuffer::PushInvite(uint64_t bot_guid, std::string from_name,
                                         uint64_t from_guid, int64_t ts) {
    std::lock_guard<std::mutex> g(mu_);
    invites_[bot_guid].push_back({std::move(from_name), from_guid, ts});
}

void InteractionEventBuffer::PushJoin(uint64_t bot_guid, std::string leader_name,
                                       uint64_t leader_guid, int64_t ts) {
    std::lock_guard<std::mutex> g(mu_);
    joins_[bot_guid].push_back({std::move(leader_name), leader_guid, ts});
}

bool InteractionEventBuffer::HasPending(uint64_t bot_guid) const {
    std::lock_guard<std::mutex> g(mu_);
    auto inv = invites_.find(bot_guid);
    if (inv != invites_.end() && !inv->second.empty()) return true;
    auto wh = whispers_.find(bot_guid);
    if (wh != whispers_.end() && !wh->second.empty()) return true;
    auto jn = joins_.find(bot_guid);
    if (jn != joins_.end() && !jn->second.empty()) return true;
    return false;
}

InteractionPayload InteractionEventBuffer::SnapshotFor(uint64_t bot_guid) const {
    std::lock_guard<std::mutex> g(mu_);
    InteractionPayload p;
    if (auto it = invites_.find(bot_guid);  it != invites_.end())  p.pending_invites = it->second;
    if (auto it = whispers_.find(bot_guid); it != whispers_.end()) p.recent_whispers = it->second;
    if (auto it = joins_.find(bot_guid);    it != joins_.end())    p.recent_group_joins = it->second;
    return p;
}

void InteractionEventBuffer::ExpireOlderThan(uint64_t bot_guid, int64_t now, int64_t window_seconds) {
    std::lock_guard<std::mutex> g(mu_);
    const int64_t cutoff = now - window_seconds;
    auto drop_older = [&](auto& vec) {
        vec.erase(std::remove_if(vec.begin(), vec.end(),
                  [cutoff](const auto& e){ return e.ts < cutoff; }),
                  vec.end());
    };
    if (auto it = invites_.find(bot_guid);  it != invites_.end())  drop_older(it->second);
    if (auto it = whispers_.find(bot_guid); it != whispers_.end()) drop_older(it->second);
    if (auto it = joins_.find(bot_guid);    it != joins_.end())    drop_older(it->second);
}

void InteractionEventBuffer::Clear(uint64_t bot_guid) {
    std::lock_guard<std::mutex> g(mu_);
    invites_.erase(bot_guid);
    whispers_.erase(bot_guid);
    joins_.erase(bot_guid);
}

void InteractionEventBuffer::ClearAll() {
    std::lock_guard<std::mutex> g(mu_);
    invites_.clear();
    whispers_.clear();
    joins_.clear();
}
```

The `<algorithm>` header is needed for `std::remove_if`. Add it.

```cpp
#include "EventBuffer/InteractionEventBuffer.h"
#include <algorithm>
```

- [ ] **Step 6: Build and run**

```bash
cmake --build build/llmagent_tests
./build/llmagent_tests/llmagent_unit_tests
```

Expected: 93 prior + 6 new = 99 cases.

- [ ] **Step 7: Commit**

```bash
git add src/Bot/LlmAgent/EventBuffer/InteractionEventBuffer.h \
        src/Bot/LlmAgent/EventBuffer/InteractionEventBuffer.cpp \
        tests/llmagent/test_interaction_buffer.cpp \
        tests/llmagent/CMakeLists.txt
git commit -m "feat(llm-agent): InteractionEventBuffer for Phase 4 T2 triggers

Per-bot sliding window for pending invites + unhandled whispers +
recent group joins. Thread-safe via single mutex (low contention
expected). ExpireOlderThan drops stale entries; HasPending is the
T2 trigger gate. 6 doctest cases."
```

---

## Task 2: `ToolCatalog` + `ParseToolCalls`

**Files:**
- Create: `src/Bot/LlmAgent/Tools/ToolCatalog.h`
- Create: `src/Bot/LlmAgent/Tools/ToolCatalog.cpp`
- Create: `tests/llmagent/test_tool_catalog.cpp`
- Create: `tests/llmagent/test_tool_parser.cpp`
- Modify: `tests/llmagent/CMakeLists.txt`

TDD.

- [ ] **Step 1: Write failing tests `tests/llmagent/test_tool_catalog.cpp`**

```cpp
#include "doctest.h"
#include "Tools/ToolCatalog.h"
#include "Vendor/nlohmann_json.hpp"

TEST_CASE("kToolsJsonSchema is a non-empty JSON array") {
    auto j = nlohmann::json::parse(kToolsJsonSchema);
    REQUIRE(j.is_array());
    CHECK(j.size() == 7);  // 7 tools per spec §3
}

TEST_CASE("kToolsJsonSchema contains expected tool names") {
    auto j = nlohmann::json::parse(kToolsJsonSchema);
    std::vector<std::string> names;
    for (const auto& t : j) names.push_back(t["function"]["name"].get<std::string>());
    CHECK(std::find(names.begin(), names.end(), "accept_party_invite") != names.end());
    CHECK(std::find(names.begin(), names.end(), "leave_party")         != names.end());
    CHECK(std::find(names.begin(), names.end(), "accept_quest")        != names.end());
    CHECK(std::find(names.begin(), names.end(), "turn_in_quest")       != names.end());
    CHECK(std::find(names.begin(), names.end(), "set_goal")            != names.end());
    CHECK(std::find(names.begin(), names.end(), "vendor_junk")         != names.end());
    CHECK(std::find(names.begin(), names.end(), "memory.remember")     != names.end());
}

TEST_CASE("kToolsJsonSchema each tool has function.parameters.type==object") {
    auto j = nlohmann::json::parse(kToolsJsonSchema);
    for (const auto& t : j) {
        REQUIRE(t.contains("function"));
        REQUIRE(t["function"].contains("parameters"));
        CHECK(t["function"]["parameters"].value("type", std::string{}) == "object");
    }
}

TEST_CASE("accept_party_invite requires 'from'") {
    auto j = nlohmann::json::parse(kToolsJsonSchema);
    for (const auto& t : j) {
        if (t["function"]["name"] == "accept_party_invite") {
            auto req = t["function"]["parameters"].value("required", nlohmann::json::array());
            CHECK(std::find(req.begin(), req.end(), "from") != req.end());
            return;
        }
    }
    FAIL("accept_party_invite missing from catalog");
}

TEST_CASE("leave_party has no required params") {
    auto j = nlohmann::json::parse(kToolsJsonSchema);
    for (const auto& t : j) {
        if (t["function"]["name"] == "leave_party") {
            auto req = t["function"]["parameters"].value("required", nlohmann::json::array());
            CHECK(req.empty());
            return;
        }
    }
    FAIL("leave_party missing from catalog");
}

TEST_CASE("memory.remember has text/entities/salience required") {
    auto j = nlohmann::json::parse(kToolsJsonSchema);
    for (const auto& t : j) {
        if (t["function"]["name"] == "memory.remember") {
            auto req = t["function"]["parameters"].value("required", nlohmann::json::array());
            CHECK(std::find(req.begin(), req.end(), "text")     != req.end());
            CHECK(std::find(req.begin(), req.end(), "entities") != req.end());
            CHECK(std::find(req.begin(), req.end(), "salience") != req.end());
            return;
        }
    }
    FAIL("memory.remember missing from catalog");
}
```

- [ ] **Step 2: Write failing tests `tests/llmagent/test_tool_parser.cpp`**

```cpp
#include "doctest.h"
#include "Tools/ToolCatalog.h"
#include "Vendor/nlohmann_json.hpp"

TEST_CASE("ParseToolCalls accepts a single accept_party_invite") {
    const std::string raw = R"([
        {"name": "accept_party_invite", "arguments": "{\"from\":\"RealPlayerBob\"}"}
    ])";
    auto result = ParseToolCalls(raw);
    REQUIRE(std::holds_alternative<std::vector<ParsedToolCall>>(result));
    const auto& calls = std::get<std::vector<ParsedToolCall>>(result);
    REQUIRE(calls.size() == 1);
    REQUIRE(std::holds_alternative<AcceptPartyInviteCall>(calls[0]));
    CHECK(std::get<AcceptPartyInviteCall>(calls[0]).from == "RealPlayerBob");
}

TEST_CASE("ParseToolCalls accepts leave_party with empty args") {
    const std::string raw = R"([
        {"name": "leave_party", "arguments": "{}"}
    ])";
    auto result = ParseToolCalls(raw);
    REQUIRE(std::holds_alternative<std::vector<ParsedToolCall>>(result));
    const auto& calls = std::get<std::vector<ParsedToolCall>>(result);
    REQUIRE(calls.size() == 1);
    CHECK(std::holds_alternative<LeavePartyCall>(calls[0]));
}

TEST_CASE("ParseToolCalls accepts accept_quest with quest_id + npc_name") {
    const std::string raw = R"([
        {"name": "accept_quest",
         "arguments": "{\"quest_id\":502, \"from_npc_name\":\"Marshal Dughan\"}"}
    ])";
    auto result = ParseToolCalls(raw);
    REQUIRE(std::holds_alternative<std::vector<ParsedToolCall>>(result));
    const auto& c = std::get<AcceptQuestCall>(std::get<std::vector<ParsedToolCall>>(result)[0]);
    CHECK(c.quest_id == 502u);
    CHECK(c.from_npc_name == "Marshal Dughan");
}

TEST_CASE("ParseToolCalls returns multiple calls in order") {
    const std::string raw = R"([
        {"name": "accept_party_invite", "arguments": "{\"from\":\"Bob\"}"},
        {"name": "set_goal", "arguments": "{\"goal\":\"rest\",\"params\":{},\"reasoning\":\"x\",\"ttl_minutes\":5}"}
    ])";
    auto result = ParseToolCalls(raw);
    REQUIRE(std::holds_alternative<std::vector<ParsedToolCall>>(result));
    const auto& calls = std::get<std::vector<ParsedToolCall>>(result);
    REQUIRE(calls.size() == 2);
    CHECK(std::holds_alternative<AcceptPartyInviteCall>(calls[0]));
    CHECK(std::holds_alternative<SetGoalCall>(calls[1]));
}

TEST_CASE("ParseToolCalls rejects unknown tool name") {
    const std::string raw = R"([
        {"name": "wipe_database", "arguments": "{}"}
    ])";
    auto result = ParseToolCalls(raw);
    REQUIRE(std::holds_alternative<ParseError>(result));
}

TEST_CASE("ParseToolCalls rejects malformed top-level JSON") {
    auto result = ParseToolCalls("not json");
    REQUIRE(std::holds_alternative<ParseError>(result));
}

TEST_CASE("ParseToolCalls returns empty list on empty array (model declined)") {
    auto result = ParseToolCalls("[]");
    REQUIRE(std::holds_alternative<std::vector<ParsedToolCall>>(result));
    CHECK(std::get<std::vector<ParsedToolCall>>(result).empty());
}

TEST_CASE("ParseToolCalls rejects missing required field") {
    const std::string raw = R"([
        {"name": "accept_party_invite", "arguments": "{}"}
    ])";
    auto result = ParseToolCalls(raw);
    REQUIRE(std::holds_alternative<ParseError>(result));
}
```

- [ ] **Step 3: Add to CMakeLists**

Edit `tests/llmagent/CMakeLists.txt` `add_executable` to add `test_tool_catalog.cpp`, `test_tool_parser.cpp`, and `${LLMAGENT_DIR}/Tools/ToolCatalog.cpp`.

- [ ] **Step 4: Run — expect FAIL**

```bash
cmake --build build/llmagent_tests 2>&1 | tail -5
```

- [ ] **Step 5: Write `src/Bot/LlmAgent/Tools/ToolCatalog.h`**

```cpp
#ifndef _PLAYERBOT_LLMAGENT_TOOL_CATALOG_H
#define _PLAYERBOT_LLMAGENT_TOOL_CATALOG_H

#include "Schemas/Goal.h"   // for ParsedGoal (used by SetGoalCall)
#include "Vendor/nlohmann_json.hpp"
#include <cstdint>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

struct AcceptPartyInviteCall { std::string from; };
struct LeavePartyCall        { };
struct AcceptQuestCall       { uint32_t quest_id = 0; std::string from_npc_name; };
struct TurnInQuestCall       { uint32_t quest_id = 0; std::string to_npc_name; };
struct SetGoalCall           { ParsedGoal goal; };
struct VendorJunkCall        { std::string vendor_npc_name; };
struct MemoryRememberCall {
    std::string text;
    std::vector<std::string> entities;
    double salience = 0.0;
    std::vector<std::tuple<std::string, std::string, std::string>> relations;
};

using ParsedToolCall = std::variant<
    AcceptPartyInviteCall, LeavePartyCall, AcceptQuestCall,
    TurnInQuestCall, SetGoalCall, VendorJunkCall, MemoryRememberCall
>;

struct ParseError {
    std::string message;
};

extern const char* const kToolsJsonSchema;

std::variant<std::vector<ParsedToolCall>, ParseError>
ParseToolCalls(const std::string& raw_json);

#endif
```

- [ ] **Step 6: Write `src/Bot/LlmAgent/Tools/ToolCatalog.cpp`**

```cpp
#include "Tools/ToolCatalog.h"

#include <stdexcept>

namespace {

template <typename T>
bool try_get(const nlohmann::json& j, const char* key, T& out) {
    if (!j.contains(key)) return false;
    try { out = j.at(key).get<T>(); } catch (...) { return false; }
    return true;
}

std::variant<ParsedToolCall, std::string>
parse_one(const std::string& name, const nlohmann::json& args) {
    if (name == "accept_party_invite") {
        AcceptPartyInviteCall c;
        if (!try_get(args, "from", c.from)) return std::string{"missing 'from'"};
        return ParsedToolCall{c};
    }
    if (name == "leave_party") {
        return ParsedToolCall{LeavePartyCall{}};
    }
    if (name == "accept_quest") {
        AcceptQuestCall c;
        if (!try_get(args, "quest_id", c.quest_id))      return std::string{"missing 'quest_id'"};
        if (!try_get(args, "from_npc_name", c.from_npc_name))
            return std::string{"missing 'from_npc_name'"};
        return ParsedToolCall{c};
    }
    if (name == "turn_in_quest") {
        TurnInQuestCall c;
        if (!try_get(args, "quest_id", c.quest_id))    return std::string{"missing 'quest_id'"};
        if (!try_get(args, "to_npc_name", c.to_npc_name))
            return std::string{"missing 'to_npc_name'"};
        return ParsedToolCall{c};
    }
    if (name == "set_goal") {
        SetGoalCall c;
        auto parsed = ParseAndValidate(args.dump());
        if (!std::holds_alternative<ParsedGoal>(parsed))
            return std::string{"set_goal arguments did not parse as a valid goal"};
        c.goal = std::get<ParsedGoal>(parsed);
        return ParsedToolCall{c};
    }
    if (name == "vendor_junk") {
        VendorJunkCall c;
        if (!try_get(args, "vendor_npc_name", c.vendor_npc_name))
            return std::string{"missing 'vendor_npc_name'"};
        return ParsedToolCall{c};
    }
    if (name == "memory.remember") {
        MemoryRememberCall c;
        if (!try_get(args, "text", c.text))         return std::string{"missing 'text'"};
        if (!try_get(args, "salience", c.salience)) return std::string{"missing 'salience'"};
        if (args.contains("entities")) {
            for (const auto& e : args["entities"]) c.entities.push_back(e.get<std::string>());
        }
        if (args.contains("relations")) {
            for (const auto& r : args["relations"]) {
                c.relations.emplace_back(
                    r.value("src", std::string{}),
                    r.value("rel", std::string{}),
                    r.value("dst", std::string{})
                );
            }
        }
        return ParsedToolCall{c};
    }
    return std::string{"unknown tool name: " + name};
}

}  // namespace

const char* const kToolsJsonSchema = R"([
  {"type":"function","function":{
    "name":"accept_party_invite",
    "description":"Accept a pending party invite from a real player.",
    "parameters":{"type":"object","required":["from"],"additionalProperties":false,
      "properties":{"from":{"type":"string","description":"Inviter's character name."}}}}},
  {"type":"function","function":{
    "name":"leave_party",
    "description":"Leave the current party/group.",
    "parameters":{"type":"object","required":[],"additionalProperties":false,"properties":{}}}},
  {"type":"function","function":{
    "name":"accept_quest",
    "description":"Accept a quest from a named NPC. NPC must be in range.",
    "parameters":{"type":"object","required":["quest_id","from_npc_name"],"additionalProperties":false,
      "properties":{
        "quest_id":{"type":"integer","minimum":1},
        "from_npc_name":{"type":"string"}}}}},
  {"type":"function","function":{
    "name":"turn_in_quest",
    "description":"Turn in a completed quest at a named NPC.",
    "parameters":{"type":"object","required":["quest_id","to_npc_name"],"additionalProperties":false,
      "properties":{
        "quest_id":{"type":"integer","minimum":1},
        "to_npc_name":{"type":"string"}}}}},
  {"type":"function","function":{
    "name":"set_goal",
    "description":"Change the bot's high-level goal (rest, do_quest, go_grind, etc).",
    "parameters":{"type":"object","required":["goal","params","reasoning","ttl_minutes"],
      "additionalProperties":false,
      "properties":{
        "goal":{"type":"string","enum":["idle","go_grind","go_camp","wander_npc","wander_random","do_quest","travel_flight","rest","outdoor_pvp"]},
        "params":{"type":"object"},
        "reasoning":{"type":"string","maxLength":500},
        "ttl_minutes":{"type":"integer","minimum":1,"maximum":1440}}}}},
  {"type":"function","function":{
    "name":"vendor_junk",
    "description":"Sell low-value items at a named vendor NPC.",
    "parameters":{"type":"object","required":["vendor_npc_name"],"additionalProperties":false,
      "properties":{"vendor_npc_name":{"type":"string"}}}}},
  {"type":"function","function":{
    "name":"memory.remember",
    "description":"Write a structured memory for later recall. Tag entities + relations.",
    "parameters":{"type":"object","required":["text","entities","salience"],"additionalProperties":false,
      "properties":{
        "text":{"type":"string","maxLength":500},
        "entities":{"type":"array","items":{"type":"string"}},
        "salience":{"type":"number","minimum":0,"maximum":1},
        "relations":{"type":"array","items":{"type":"object",
          "required":["src","rel","dst"],"additionalProperties":false,
          "properties":{"src":{"type":"string"},"rel":{"type":"string"},"dst":{"type":"string"}}}}}}}}
])";

std::variant<std::vector<ParsedToolCall>, ParseError>
ParseToolCalls(const std::string& raw_json) {
    nlohmann::json j;
    try { j = nlohmann::json::parse(raw_json); }
    catch (const std::exception& e) { return ParseError{std::string{"top-level parse: "} + e.what()}; }
    if (!j.is_array()) return ParseError{"top-level not an array"};

    std::vector<ParsedToolCall> out;
    for (const auto& entry : j) {
        std::string name;
        if (!try_get(entry, "name", name)) return ParseError{"tool call missing 'name'"};

        std::string args_str;
        nlohmann::json args = nlohmann::json::object();
        if (entry.contains("arguments")) {
            // The OpenAI shape: "arguments" is a JSON-encoded string.
            if (entry["arguments"].is_string()) {
                try { args = nlohmann::json::parse(entry["arguments"].get<std::string>()); }
                catch (...) { return ParseError{"failed to parse 'arguments' as JSON for " + name}; }
            } else if (entry["arguments"].is_object()) {
                args = entry["arguments"];
            }
        }

        auto one = parse_one(name, args);
        if (std::holds_alternative<std::string>(one))
            return ParseError{std::get<std::string>(one)};
        out.push_back(std::get<ParsedToolCall>(one));
    }
    return out;
}
```

- [ ] **Step 7: Build and run**

```bash
cmake --build build/llmagent_tests
./build/llmagent_tests/llmagent_unit_tests
```

Expected: 99 + 6 + 8 = 113 cases.

- [ ] **Step 8: Commit**

```bash
git add src/Bot/LlmAgent/Tools/ToolCatalog.h \
        src/Bot/LlmAgent/Tools/ToolCatalog.cpp \
        tests/llmagent/test_tool_catalog.cpp \
        tests/llmagent/test_tool_parser.cpp \
        tests/llmagent/CMakeLists.txt
git commit -m "feat(llm-agent): Tool catalog + ParseToolCalls

kToolsJsonSchema is the 7-tool OpenAI function-call schema sent to
llama-server. ParseToolCalls returns std::variant<vector<ParsedToolCall>,
ParseError>. SetGoalCall reuses Phase 1's ParseAndValidate for its
nested params — zero new code for goal validation."
```

---

## Task 3: `InteractionContext` POD + `ToolValidators` (pure)

**Files:**
- Create: `src/Bot/LlmAgent/Tools/InteractionContext.h`
- Create: `src/Bot/LlmAgent/Tools/InteractionContext.cpp` (worldserver-side snapshot, guarded)
- Create: `src/Bot/LlmAgent/Tools/ToolValidators.h`
- Create: `src/Bot/LlmAgent/Tools/ToolValidators.cpp`
- Create: `tests/llmagent/test_tool_validators.cpp`
- Modify: `tests/llmagent/CMakeLists.txt`

TDD.

- [ ] **Step 1: Write failing test `tests/llmagent/test_tool_validators.cpp`**

```cpp
#include "doctest.h"
#include "Tools/ToolValidators.h"
#include "Tools/InteractionContext.h"
#include "Tools/ToolCatalog.h"

namespace {

InteractionContext make_ctx() {
    InteractionContext c;
    c.bot_level = 37;
    c.map_id = 0;
    c.map_min_x = -10000; c.map_max_x = 10000;
    c.map_min_y = -10000; c.map_max_y = 10000;
    c.pending_invites = {{"RealPlayerBob", 999, 1000}};
    c.quest_log.push_back({502, /*status=incomplete*/0, /*obj_count*/3});
    c.quest_log.push_back({488, /*status=complete*/1, /*obj_count*/1});
    c.in_group = false;
    c.nearby_creatures = {{1001, "Marshal Dughan", "humanoid", /*in_range_10y*/true,
                            /*is_quest_giver_502*/true,  /*is_turn_in_488*/false,
                            /*is_vendor*/false}};
    c.nearby_creatures.push_back({1002, "Innkeeper Anne", "humanoid", true,
                                   false, true, true});  // turn-in for 488, vendor
    return c;
}

}  // namespace

// accept_party_invite
TEST_CASE("Validate accept_party_invite happy") {
    auto r = Validate(AcceptPartyInviteCall{"RealPlayerBob"}, make_ctx());
    CHECK(r.accepted);
}
TEST_CASE("Validate accept_party_invite rejects unknown sender") {
    auto r = Validate(AcceptPartyInviteCall{"NoSuchPlayer"}, make_ctx());
    CHECK_FALSE(r.accepted);
    CHECK(r.reject_reason == "rejected_invite_from_unknown");
}
TEST_CASE("Validate accept_party_invite rejects when no pending invites") {
    InteractionContext c = make_ctx();
    c.pending_invites.clear();
    auto r = Validate(AcceptPartyInviteCall{"RealPlayerBob"}, c);
    CHECK_FALSE(r.accepted);
    CHECK(r.reject_reason == "rejected_no_pending_invite");
}

// leave_party
TEST_CASE("Validate leave_party rejects when not in group") {
    auto r = Validate(LeavePartyCall{}, make_ctx());
    CHECK_FALSE(r.accepted);
    CHECK(r.reject_reason == "rejected_not_in_group");
}
TEST_CASE("Validate leave_party happy when in group") {
    InteractionContext c = make_ctx();
    c.in_group = true;
    auto r = Validate(LeavePartyCall{}, c);
    CHECK(r.accepted);
}

// accept_quest
TEST_CASE("Validate accept_quest happy") {
    auto r = Validate(AcceptQuestCall{502, "Marshal Dughan"}, make_ctx());
    CHECK(r.accepted);
}
TEST_CASE("Validate accept_quest rejects unknown NPC") {
    auto r = Validate(AcceptQuestCall{502, "Mystery NPC"}, make_ctx());
    CHECK_FALSE(r.accepted);
    CHECK(r.reject_reason == "rejected_npc_not_in_range");
}
TEST_CASE("Validate accept_quest rejects when NPC is not quest giver for that id") {
    InteractionContext c = make_ctx();
    c.nearby_creatures[0].is_quest_giver_for = 999;  // different quest
    auto r = Validate(AcceptQuestCall{502, "Marshal Dughan"}, c);
    CHECK_FALSE(r.accepted);
    CHECK(r.reject_reason == "rejected_npc_not_quest_giver");
}
TEST_CASE("Validate accept_quest rejects when already in log") {
    auto r = Validate(AcceptQuestCall{488, "Innkeeper Anne"}, make_ctx());
    CHECK_FALSE(r.accepted);
    CHECK(r.reject_reason == "rejected_quest_already_in_log");
}

// turn_in_quest
TEST_CASE("Validate turn_in_quest happy") {
    auto r = Validate(TurnInQuestCall{488, "Innkeeper Anne"}, make_ctx());
    CHECK(r.accepted);
}
TEST_CASE("Validate turn_in_quest rejects not-in-log") {
    auto r = Validate(TurnInQuestCall{999, "Innkeeper Anne"}, make_ctx());
    CHECK_FALSE(r.accepted);
    CHECK(r.reject_reason == "rejected_quest_not_in_log");
}
TEST_CASE("Validate turn_in_quest rejects not-complete") {
    auto r = Validate(TurnInQuestCall{502, "Marshal Dughan"}, make_ctx());  // 502 is incomplete
    CHECK_FALSE(r.accepted);
    CHECK(r.reject_reason == "rejected_quest_not_complete");
}
TEST_CASE("Validate turn_in_quest rejects NPC not turn-in target") {
    auto r = Validate(TurnInQuestCall{488, "Marshal Dughan"}, make_ctx());  // Marshal doesn't take 488
    CHECK_FALSE(r.accepted);
    CHECK(r.reject_reason == "rejected_npc_not_turn_in_target");
}

// vendor_junk
TEST_CASE("Validate vendor_junk happy") {
    auto r = Validate(VendorJunkCall{"Innkeeper Anne"}, make_ctx());
    CHECK(r.accepted);
}
TEST_CASE("Validate vendor_junk rejects non-vendor") {
    auto r = Validate(VendorJunkCall{"Marshal Dughan"}, make_ctx());
    CHECK_FALSE(r.accepted);
    CHECK(r.reject_reason == "rejected_npc_not_vendor");
}
TEST_CASE("Validate vendor_junk rejects unknown NPC") {
    auto r = Validate(VendorJunkCall{"NoSuchNPC"}, make_ctx());
    CHECK_FALSE(r.accepted);
    CHECK(r.reject_reason == "rejected_npc_not_in_range");
}

// memory.remember
TEST_CASE("Validate memory.remember happy") {
    auto r = Validate(MemoryRememberCall{"text", {"x"}, 0.5, {}}, make_ctx());
    CHECK(r.accepted);
}
TEST_CASE("Validate memory.remember rejects text-too-long") {
    MemoryRememberCall c;
    c.text = std::string(600, 'x');
    c.salience = 0.5;
    auto r = Validate(c, make_ctx());
    CHECK_FALSE(r.accepted);
    CHECK(r.reject_reason == "rejected_text_too_long");
}
TEST_CASE("Validate memory.remember rejects salience out of range") {
    auto r = Validate(MemoryRememberCall{"x", {}, 1.5, {}}, make_ctx());
    CHECK_FALSE(r.accepted);
    CHECK(r.reject_reason == "rejected_salience_out_of_range");
}

// set_goal — reuses Phase 2 validator, smoke-check one case
TEST_CASE("Validate set_goal Idle is always accepted") {
    ParsedGoal g{GoalKind::Idle, IdleParams{}, "x", 5};
    auto r = Validate(SetGoalCall{g}, make_ctx());
    CHECK(r.accepted);
}
```

- [ ] **Step 2: Add to CMakeLists**

In `tests/llmagent/CMakeLists.txt`, add `test_tool_validators.cpp` and `${LLMAGENT_DIR}/Tools/ToolValidators.cpp` and `${LLMAGENT_DIR}/Tools/InteractionContext.cpp`.

- [ ] **Step 3: Run — expect FAIL**

```bash
cmake --build build/llmagent_tests 2>&1 | tail -5
```

- [ ] **Step 4: Write `src/Bot/LlmAgent/Tools/InteractionContext.h`**

```cpp
#ifndef _PLAYERBOT_LLMAGENT_INTERACTION_CONTEXT_H
#define _PLAYERBOT_LLMAGENT_INTERACTION_CONTEXT_H

#include "Validator/ValidationContext.h"   // for QuestLogContextEntry
#include "EventBuffer/InteractionEventBuffer.h"
#include <cstdint>
#include <string>
#include <vector>

struct NearbyCreature {
    uint64_t guid = 0;
    std::string name;
    std::string type;
    bool        in_range_10y = false;
    uint32_t    is_quest_giver_for = 0;   // 0 = none, else quest_id
    uint32_t    is_turn_in_for     = 0;
    bool        is_vendor = false;
};

struct InteractionContext {
    uint32_t bot_level = 0;
    uint32_t map_id = 0;
    double   map_min_x = -100000.0, map_max_x = 100000.0;
    double   map_min_y = -100000.0, map_max_y = 100000.0;

    std::vector<PendingInvite>           pending_invites;
    std::vector<QuestLogContextEntry>    quest_log;
    std::vector<NearbyCreature>          nearby_creatures;
    bool                                 in_group = false;
};

#ifndef LLMAGENT_UNIT_TESTS
class PlayerbotAI;
InteractionContext SnapshotInteractionContext(PlayerbotAI* botAI);
#endif

#endif
```

- [ ] **Step 5: Write `src/Bot/LlmAgent/Tools/InteractionContext.cpp`**

```cpp
#include "Tools/InteractionContext.h"

#ifndef LLMAGENT_UNIT_TESTS

#include "LlmAgentManager.h"
#include "PlayerbotAI.h"
#include "Player.h"
#include "Map.h"
#include "QuestDef.h"
#include "ObjectMgr.h"
#include "ObjectAccessor.h"
#include "AiObjectContext.h"

InteractionContext SnapshotInteractionContext(PlayerbotAI* botAI) {
    InteractionContext ctx;
    if (!botAI) return ctx;
    Player* bot = botAI->GetBot();
    if (!bot) return ctx;
    ctx.bot_level = bot->GetLevel();
    if (Map* m = bot->GetMap()) ctx.map_id = m->GetId();

    // Pull pending invites + recent whispers + recent joins from Phase 4's buffer.
    auto& mgr = LlmAgentManager::Instance();
    auto payload = mgr.Interactions().SnapshotFor(bot->GetGUID().GetRawValue());
    ctx.pending_invites = std::move(payload.pending_invites);

    // Quest log.
    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot) {
        uint32 qid = bot->GetQuestSlotQuestId(slot);
        if (!qid) continue;
        const Quest* q = sObjectMgr->GetQuestTemplate(qid);
        if (!q) continue;
        QuestLogContextEntry e;
        e.id = qid;
        QuestStatus st = bot->GetQuestStatus(qid);
        e.status = (st == QUEST_STATUS_COMPLETE) ? 1u : 0u;
        e.objective_count = 3;  // conservative
        ctx.quest_log.push_back(e);
    }

    ctx.in_group = bot->GetGroup() != nullptr;

    // Nearby creatures: use the existing "nearest npcs" AI value if present.
    // For Phase 4 we leave nearby_creatures empty; validators that depend on it
    // will hard-reject (which is the correct conservative behavior — the
    // rule-based path covers those cases). Phase 4.1 wires real scans.

    return ctx;
}

#endif
```

- [ ] **Step 6: Write `src/Bot/LlmAgent/Tools/ToolValidators.h`**

```cpp
#ifndef _PLAYERBOT_LLMAGENT_TOOL_VALIDATORS_H
#define _PLAYERBOT_LLMAGENT_TOOL_VALIDATORS_H

#include "Tools/InteractionContext.h"
#include "Tools/ToolCatalog.h"
#include "Validator/GoalValidator.h"   // for ValidationResult
#include <string>

ValidationResult Validate(const AcceptPartyInviteCall&, const InteractionContext&);
ValidationResult Validate(const LeavePartyCall&,         const InteractionContext&);
ValidationResult Validate(const AcceptQuestCall&,        const InteractionContext&);
ValidationResult Validate(const TurnInQuestCall&,        const InteractionContext&);
ValidationResult Validate(const SetGoalCall&,            const InteractionContext&);
ValidationResult Validate(const VendorJunkCall&,         const InteractionContext&);
ValidationResult Validate(const MemoryRememberCall&,     const InteractionContext&);

#endif
```

- [ ] **Step 7: Write `src/Bot/LlmAgent/Tools/ToolValidators.cpp`**

```cpp
#include "Tools/ToolValidators.h"
#include "Validator/GoalValidator.h"

namespace {

const NearbyCreature* find_nearby(const InteractionContext& ctx, const std::string& name) {
    for (const auto& c : ctx.nearby_creatures) {
        if (c.name == name) return &c;
    }
    return nullptr;
}

const QuestLogContextEntry* find_quest(const InteractionContext& ctx, uint32_t qid) {
    for (const auto& q : ctx.quest_log) {
        if (q.id == qid) return &q;
    }
    return nullptr;
}

}  // namespace

ValidationResult Validate(const AcceptPartyInviteCall& c, const InteractionContext& ctx) {
    if (ctx.pending_invites.empty()) return {false, "rejected_no_pending_invite"};
    for (const auto& inv : ctx.pending_invites) {
        if (inv.from_name == c.from) return {true, ""};
    }
    return {false, "rejected_invite_from_unknown"};
}

ValidationResult Validate(const LeavePartyCall&, const InteractionContext& ctx) {
    if (!ctx.in_group) return {false, "rejected_not_in_group"};
    return {true, ""};
}

ValidationResult Validate(const AcceptQuestCall& c, const InteractionContext& ctx) {
    const auto* npc = find_nearby(ctx, c.from_npc_name);
    if (!npc || !npc->in_range_10y) return {false, "rejected_npc_not_in_range"};
    if (npc->is_quest_giver_for != c.quest_id) return {false, "rejected_npc_not_quest_giver"};
    if (find_quest(ctx, c.quest_id) != nullptr) return {false, "rejected_quest_already_in_log"};
    return {true, ""};
}

ValidationResult Validate(const TurnInQuestCall& c, const InteractionContext& ctx) {
    const auto* q = find_quest(ctx, c.quest_id);
    if (!q) return {false, "rejected_quest_not_in_log"};
    if (q->status != 1) return {false, "rejected_quest_not_complete"};
    const auto* npc = find_nearby(ctx, c.to_npc_name);
    if (!npc || !npc->in_range_10y) return {false, "rejected_npc_not_in_range"};
    if (npc->is_turn_in_for != c.quest_id) return {false, "rejected_npc_not_turn_in_target"};
    return {true, ""};
}

ValidationResult Validate(const SetGoalCall& c, const InteractionContext& ctx) {
    BotValidationContext vctx;
    vctx.bot_level = ctx.bot_level;
    vctx.map_id = ctx.map_id;
    vctx.map_min_x = ctx.map_min_x; vctx.map_max_x = ctx.map_max_x;
    vctx.map_min_y = ctx.map_min_y; vctx.map_max_y = ctx.map_max_y;
    vctx.quest_log = ctx.quest_log;
    // nearby_creature_guids/known_flight_node_ids/capture_point_spawn_ids left empty
    // (same conservative posture as Phase 2). T2 set_goal calls for those variants
    // will hard-reject.
    return ValidateGoalDecision(c.goal, vctx);
}

ValidationResult Validate(const VendorJunkCall& c, const InteractionContext& ctx) {
    const auto* npc = find_nearby(ctx, c.vendor_npc_name);
    if (!npc || !npc->in_range_10y) return {false, "rejected_npc_not_in_range"};
    if (!npc->is_vendor) return {false, "rejected_npc_not_vendor"};
    return {true, ""};
}

ValidationResult Validate(const MemoryRememberCall& c, const InteractionContext&) {
    if (c.text.size() > 500) return {false, "rejected_text_too_long"};
    if (c.salience < 0.0 || c.salience > 1.0) return {false, "rejected_salience_out_of_range"};
    return {true, ""};
}
```

- [ ] **Step 8: Build and run**

```bash
cmake --build build/llmagent_tests
./build/llmagent_tests/llmagent_unit_tests
```

Expected: 113 + ~22 = ~135 cases pass.

- [ ] **Step 9: Commit**

```bash
git add src/Bot/LlmAgent/Tools/InteractionContext.h \
        src/Bot/LlmAgent/Tools/InteractionContext.cpp \
        src/Bot/LlmAgent/Tools/ToolValidators.h \
        src/Bot/LlmAgent/Tools/ToolValidators.cpp \
        tests/llmagent/test_tool_validators.cpp \
        tests/llmagent/CMakeLists.txt
git commit -m "feat(llm-agent): ToolValidators + InteractionContext POD

Pure decision functions per tool. SetGoalCall reuses Phase 2's
ValidateGoalDecision. SnapshotInteractionContext is the worldserver-
side adapter (guarded by LLMAGENT_UNIT_TESTS). Phase 4 leaves
nearby_creatures empty in the snapshot — accept_quest/turn_in_quest/
vendor_junk hard-reject until Phase 4.1 wires real grid scans."
```

---

## Task 4: `LlmCounters` extensions + `LlmAgentConfig` Tier2 keys

**Files:**
- Modify: `src/Bot/LlmAgent/LlmAgentConfig.h`
- Modify: `src/Bot/LlmAgent/Telemetry/LlmCounters.h`
- Modify: `src/Bot/LlmAgent/Telemetry/LlmCounters.cpp`
- Modify: `tests/llmagent/test_config_load.cpp`
- Modify: `tests/llmagent/test_counters.cpp`

- [ ] **Step 1: Add 2 new test cases to `tests/llmagent/test_config_load.cpp`**

Append:

```cpp
TEST_CASE("LlmAgentConfig Tier2 defaults") {
    StubConfigSource src;
    LlmAgentConfig cfg = LoadLlmAgentConfig(src);
    CHECK(cfg.Tier2_Enabled == true);
    CHECK(cfg.Tier2_MaxToolsPerResponse == 3u);
    CHECK(cfg.Tier2_WhisperWindowSeconds == 120u);
    CHECK(!cfg.Tier2_SystemPrompt.empty());
}

TEST_CASE("LlmAgentConfig Tier2 overrides applied") {
    StubConfigSource src;
    src.values["AiPlayerbot.LlmAgent.Tier2.Enabled"] = "0";
    src.values["AiPlayerbot.LlmAgent.Tier2.MaxToolsPerResponse"] = "5";
    src.values["AiPlayerbot.LlmAgent.Tier2.WhisperWindowSeconds"] = "300";
    src.values["AiPlayerbot.LlmAgent.Tier2.SystemPrompt"] = "custom";

    LlmAgentConfig cfg = LoadLlmAgentConfig(src);
    CHECK(cfg.Tier2_Enabled == false);
    CHECK(cfg.Tier2_MaxToolsPerResponse == 5u);
    CHECK(cfg.Tier2_WhisperWindowSeconds == 300u);
    CHECK(cfg.Tier2_SystemPrompt == "custom");
}
```

- [ ] **Step 2: Add 4 new fields to `LlmAgentConfig.h`**

Inside the struct (after Phase 3 fields, before closing brace):

```cpp
    // Phase 4 — Tier 2 interactive
    bool        Tier2_Enabled              = true;
    uint32_t    Tier2_MaxToolsPerResponse  = 3;
    uint32_t    Tier2_WhisperWindowSeconds = 120;
    std::string Tier2_SystemPrompt;
```

Add this `extern` near `kDefaultSystemPrompt`:

```cpp
extern const char* const kDefaultTier2SystemPrompt;
```

Inside `LoadLlmAgentConfig`, after the Phase 3 loader lines:

```cpp
    cfg.Tier2_Enabled              = src.template Get<bool>       ("AiPlayerbot.LlmAgent.Tier2.Enabled",              true);
    cfg.Tier2_MaxToolsPerResponse  = src.template Get<uint32_t>   ("AiPlayerbot.LlmAgent.Tier2.MaxToolsPerResponse",  uint32_t{3});
    cfg.Tier2_WhisperWindowSeconds = src.template Get<uint32_t>   ("AiPlayerbot.LlmAgent.Tier2.WhisperWindowSeconds", uint32_t{120});
    cfg.Tier2_SystemPrompt         = src.template Get<std::string>("AiPlayerbot.LlmAgent.Tier2.SystemPrompt",         std::string{kDefaultTier2SystemPrompt});
```

- [ ] **Step 3: Define `kDefaultTier2SystemPrompt` in `LlmAgentConfig.cpp`**

Append:

```cpp
const char* const kDefaultTier2SystemPrompt =
    "You are an in-world decision-maker for a World of Warcraft NPC. A real "
    "human player has just engaged with you (whisper, party invite, or group "
    "join). Use the provided tools to respond. Prefer concrete, helpful actions "
    "(accept the invite, change your goal to follow them, accept the quest "
    "they're offering). When unsure, do nothing — emit no tool calls. Keep "
    "memory writes brief and specific.";
```

- [ ] **Step 4: Add 2 new test cases to `tests/llmagent/test_counters.cpp`**

Append:

```cpp
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
```

- [ ] **Step 5: Extend `Snapshot_t` in `LlmCounters.h`**

Inside `struct Snapshot_t`, add:

```cpp
    std::unordered_map<std::string, uint64_t> tool_received;
    std::unordered_map<std::string, uint64_t> tool_applied;
    std::unordered_map<std::string, uint64_t> tool_rejected_by_reason;
    std::unordered_map<std::string, uint64_t> tool_threw;
    uint64_t tool_no_action = 0;
    uint64_t tool_truncated = 0;
    uint64_t tool_schema_error = 0;
```

Add public methods to `class LlmCounters`:

```cpp
    void IncToolReceived(const std::string& name);
    void IncToolApplied(const std::string& name);
    void IncToolRejected(const std::string& reason);
    void IncToolThrew(const std::string& name);
    void IncToolNoAction();
    void IncToolTruncated();
    void IncToolSchemaError();
```

Add private members:

```cpp
    mutable std::mutex                             tool_mu_;
    std::unordered_map<std::string, uint64_t>      tool_received_;
    std::unordered_map<std::string, uint64_t>      tool_applied_;
    std::unordered_map<std::string, uint64_t>      tool_rejected_;
    std::unordered_map<std::string, uint64_t>      tool_threw_;
    std::atomic<uint64_t>                          tool_no_action_{0};
    std::atomic<uint64_t>                          tool_truncated_{0};
    std::atomic<uint64_t>                          tool_schema_error_{0};
```

- [ ] **Step 6: Implement the new methods in `LlmCounters.cpp`**

Append:

```cpp
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
```

Also extend `Snapshot()` — add at the end:

```cpp
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
```

Extend `DumpToLog()` similarly — add per-tool counts to the output line.

- [ ] **Step 7: Build and run**

```bash
cmake --build build/llmagent_tests
./build/llmagent_tests/llmagent_unit_tests
```

Expected: ~135 + 4 = ~139 cases.

- [ ] **Step 8: Commit**

```bash
git add src/Bot/LlmAgent/LlmAgentConfig.h \
        src/Bot/LlmAgent/LlmAgentConfig.cpp \
        src/Bot/LlmAgent/Telemetry/LlmCounters.h \
        src/Bot/LlmAgent/Telemetry/LlmCounters.cpp \
        tests/llmagent/test_config_load.cpp \
        tests/llmagent/test_counters.cpp
git commit -m "feat(llm-agent): Phase 4 config keys + LlmCounters tool buckets

4 new Tier2 config keys (Enabled, MaxToolsPerResponse, WhisperWindowSeconds,
SystemPrompt). LlmCounters gains per-tool received/applied/rejected/threw
maps plus three scalar buckets (no_action, truncated, schema_error)."
```

---

## Task 5: Wire `InteractionEventBuffer` into `LlmAgentManager` + extend hooks

**Files:**
- Modify: `src/Bot/LlmAgent/LlmAgentManager.h`
- Modify: `src/Bot/LlmAgent/LlmAgentManager.cpp`
- Modify: `src/Bot/LlmAgent/Hooks/LlmAgentHooks.h`
- Modify: `src/Bot/LlmAgent/Hooks/LlmAgentHooks.cpp`

- [ ] **Step 1: Add `Interactions()` accessor to `LlmAgentManager.h`**

Add include:

```cpp
#include "EventBuffer/InteractionEventBuffer.h"
```

In `class LlmAgentManager` public section (near other accessors):

```cpp
    InteractionEventBuffer& Interactions() { return interactions_; }
```

Private member:

```cpp
    InteractionEventBuffer interactions_;
```

Inside `Shutdown()` (alongside the other `Clear()` calls):

```cpp
    interactions_.ClearAll();
```

- [ ] **Step 2: Extend `LlmAgentHooks.h`**

```cpp
void OnPartyInviteReceived(Player* bot, Player* inviter);
void OnGroupJoined(Player* bot, Player* leader);
```

- [ ] **Step 3: Update `LlmAgentHooks.cpp`**

In `OnWhisperReceived` (inside the `#ifndef LLMAGENT_UNIT_TESTS` block, after the Phase 3 `Remember` call), add:

```cpp
    mgr.Interactions().PushWhisper(
        bot_guid, sender->GetName(), sender->GetGUID().GetRawValue(),
        truncate_whisper(text), static_cast<int64_t>(time(nullptr)));
```

Add the two new functions (full bodies):

```cpp
void OnPartyInviteReceived(Player* bot, Player* inviter) {
#ifndef LLMAGENT_UNIT_TESTS
    if (!bot || !inviter) return;
    if (sPlayerbotsMgr.GetPlayerbotAI(inviter) != nullptr) return;  // bot-to-bot
    auto& mgr = LlmAgentManager::Instance();
    if (!mgr.Enabled()) return;
    const uint64_t bot_guid = bot->GetGUID().GetRawValue();
    if (mgr.Config().SocialOptIn) mgr.Selector().OptInBot(bot_guid);
    mgr.Interactions().PushInvite(
        bot_guid, inviter->GetName(), inviter->GetGUID().GetRawValue(),
        static_cast<int64_t>(time(nullptr)));
#endif
    (void)bot; (void)inviter;
}

void OnGroupJoined(Player* bot, Player* leader) {
#ifndef LLMAGENT_UNIT_TESTS
    if (!bot || !leader) return;
    auto& mgr = LlmAgentManager::Instance();
    if (!mgr.Enabled()) return;
    mgr.Interactions().PushJoin(
        bot->GetGUID().GetRawValue(),
        leader->GetName(),
        leader->GetGUID().GetRawValue(),
        static_cast<int64_t>(time(nullptr)));
#endif
    (void)bot; (void)leader;
}
```

- [ ] **Step 4: Build unit tests — still ~139 pass**

```bash
cmake --build build/llmagent_tests
./build/llmagent_tests/llmagent_unit_tests
```

Expected: ~139 cases.

- [ ] **Step 5: Commit**

```bash
git add src/Bot/LlmAgent/LlmAgentManager.h \
        src/Bot/LlmAgent/LlmAgentManager.cpp \
        src/Bot/LlmAgent/Hooks/LlmAgentHooks.h \
        src/Bot/LlmAgent/Hooks/LlmAgentHooks.cpp
git commit -m "feat(llm-agent): wire InteractionEventBuffer into Manager + new hooks

Manager owns interactions_. Whisper hook now also pushes to it.
New OnPartyInviteReceived and OnGroupJoined hooks for party-invite
and group-join events. Hooks gated by LlmAgent.Enabled + (for
invites) social opt-in."
```

---

## Task 6: AC hook wiring — call `OnPartyInviteReceived` + `OnGroupJoined`

**Files:**
- Modify: `src/Bot/PlayerbotAI.cpp` OR a script file — wherever AC dispatches party-invite events to bots
- Modify: `src/Script/Playerbots.cpp` if needed

This task wires Phase 4's new hooks into AzerothCore's actual event paths.

- [ ] **Step 1: Locate AC's party-invite reception path**

Run:

```bash
grep -rn "PartyInvite\|GroupInvite\|AcceptInvite" src/ --include="*.cpp" --include="*.h" 2>&1 | grep -i "handle\|script\|on" | head -15
```

Expected: candidates include `OnGroupInviteRequest` PlayerScript hook or AC's `HandleGroupInviteOpcode`.

- [ ] **Step 2: Add a PlayerScript override in `src/Script/Playerbots.cpp`**

Inside the existing `PlayerbotsScript` class (or add a new `PlayerbotsPartyScript` if cleaner), override the relevant hook. Pseudo-template (exact API depends on AC version):

```cpp
    // In the existing PlayerbotsScript class or a new BotPartyScript:
    void OnPlayerInviteToGroup(Player* inviter, Player* invitee) override {
        if (!invitee) return;
        auto& mgr = LlmAgentManager::Instance();
        if (PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(invitee)) {
            LlmAgentHooks::OnPartyInviteReceived(invitee, inviter);
        }
    }

    void OnPlayerJoinedGroup(Player* player, Player* leader) override {
        if (!player) return;
        if (PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(player)) {
            LlmAgentHooks::OnGroupJoined(player, leader);
        }
    }
```

(If AC's API doesn't expose those exact hooks, document the closest match — `OnAfterPlayerEnterGroup` or similar — in the implementation. The build will surface the right name.)

Add include:

```cpp
#include "Bot/LlmAgent/Hooks/LlmAgentHooks.h"
```

(already there from Phase 2; verify.)

- [ ] **Step 3: Build unit tests — still ~139 pass**

The new code is worldserver-only; unit tests unaffected.

- [ ] **Step 4: Commit**

```bash
git add src/Script/Playerbots.cpp
git commit -m "feat(llm-agent): wire AC group-invite + group-join events to T2 hooks

PlayerScript overrides forward party-invite and group-join events
to LlmAgentHooks::OnPartyInviteReceived / OnGroupJoined. Bots that
are LLM-opted-in get a T2 trigger on each event."
```

---

## Task 7: `Tier2_Interactive` digest builder

**Files:**
- Create: `src/Bot/LlmAgent/Tiers/Tier2_Interactive.h`
- Create: `src/Bot/LlmAgent/Tiers/Tier2_Interactive.cpp`

No new unit tests — exercised end-to-end in Layer 3.

- [ ] **Step 1: Write `src/Bot/LlmAgent/Tiers/Tier2_Interactive.h`**

```cpp
#ifndef _PLAYERBOT_LLMAGENT_TIER2_INTERACTIVE_H
#define _PLAYERBOT_LLMAGENT_TIER2_INTERACTIVE_H

#include "Vendor/nlohmann_json.hpp"

#ifndef LLMAGENT_UNIT_TESTS
class PlayerbotAI;

namespace LlmAgentTier2 {

// Returns the user-message JSON for a T2 LLM call.
nlohmann::json BuildT2Digest(PlayerbotAI* botAI);

// Returns the OpenAI-shaped request body (model, messages, tools, etc.)
std::string BuildT2RequestBody(PlayerbotAI* botAI);

}  // namespace LlmAgentTier2
#endif

#endif
```

- [ ] **Step 2: Write `src/Bot/LlmAgent/Tiers/Tier2_Interactive.cpp`**

```cpp
#include "Tiers/Tier2_Interactive.h"

#ifndef LLMAGENT_UNIT_TESTS

#include "Tiers/Tier0_StateDigest.h"
#include "Tools/ToolCatalog.h"
#include "LlmAgentManager.h"
#include "PlayerbotAI.h"
#include "Player.h"

namespace LlmAgentTier2 {

nlohmann::json BuildT2Digest(PlayerbotAI* botAI) {
    LlmBotState state = SnapshotBot(botAI);
    nlohmann::json base = BuildDigestJson(state);

    auto& mgr = LlmAgentManager::Instance();
    auto payload = mgr.Interactions().SnapshotFor(botAI->GetBot()->GetGUID().GetRawValue());

    nlohmann::json interactions = {
        {"pending_invites", nlohmann::json::array()},
        {"recent_whispers", nlohmann::json::array()},
        {"recent_group_joins", nlohmann::json::array()},
    };
    for (const auto& inv : payload.pending_invites)
        interactions["pending_invites"].push_back({{"from", inv.from_name}, {"ts", inv.ts}});
    for (const auto& w : payload.recent_whispers)
        interactions["recent_whispers"].push_back(
            {{"from", w.from_name}, {"text", w.text}, {"age_s",
             static_cast<int64_t>(time(nullptr)) - w.ts}});
    for (const auto& j : payload.recent_group_joins)
        interactions["recent_group_joins"].push_back(
            {{"leader", j.leader_name}, {"ts", j.ts}});

    base["interaction_context"] = interactions;
    return base;
}

std::string BuildT2RequestBody(PlayerbotAI* botAI) {
    auto& mgr = LlmAgentManager::Instance();
    const auto& cfg = mgr.Config();
    nlohmann::json digest = BuildT2Digest(botAI);

    nlohmann::json body;
    body["model"] = cfg.Model;
    body["messages"] = nlohmann::json::array();
    body["messages"].push_back({{"role","system"}, {"content", cfg.Tier2_SystemPrompt}});
    body["messages"].push_back({{"role","user"},   {"content", digest.dump()}});
    body["tools"] = nlohmann::json::parse(kToolsJsonSchema);
    body["tool_choice"] = "auto";
    body["temperature"] = 0.5;
    body["max_tokens"] = 512;
    return body.dump();
}

}  // namespace LlmAgentTier2

#endif
```

- [ ] **Step 3: Build unit tests — still ~139 pass**

```bash
cmake --build build/llmagent_tests
./build/llmagent_tests/llmagent_unit_tests
```

- [ ] **Step 4: Commit**

```bash
git add src/Bot/LlmAgent/Tiers/Tier2_Interactive.h src/Bot/LlmAgent/Tiers/Tier2_Interactive.cpp
git commit -m "feat(llm-agent): Tier2_Interactive digest + request builder

BuildT2Digest enriches Tier0's digest with the InteractionPayload
(pending_invites, recent_whispers with age_s, recent_group_joins).
BuildT2RequestBody constructs the OpenAI tool-calling body with
tools[] from kToolsJsonSchema and tool_choice='auto'."
```

---

## Task 8: `ToolExecutors`

**Files:**
- Create: `src/Bot/LlmAgent/Tools/ToolExecutors.h`
- Create: `src/Bot/LlmAgent/Tools/ToolExecutors.cpp`

Worldserver-only. No unit tests.

- [ ] **Step 1: Write `src/Bot/LlmAgent/Tools/ToolExecutors.h`**

```cpp
#ifndef _PLAYERBOT_LLMAGENT_TOOL_EXECUTORS_H
#define _PLAYERBOT_LLMAGENT_TOOL_EXECUTORS_H

#include "Tools/ToolCatalog.h"

class PlayerbotAI;

namespace LlmAgentTools {

// Each returns true on successful apply, false on exception (and bot is
// recovered to Idle so it never stays in a corrupt state).
bool ApplyAcceptPartyInvite(const AcceptPartyInviteCall&, PlayerbotAI*);
bool ApplyLeaveParty       (const LeavePartyCall&,         PlayerbotAI*);
bool ApplyAcceptQuest      (const AcceptQuestCall&,        PlayerbotAI*);
bool ApplyTurnInQuest      (const TurnInQuestCall&,        PlayerbotAI*);
bool ApplySetGoal          (const SetGoalCall&,            PlayerbotAI*);
bool ApplyVendorJunk       (const VendorJunkCall&,         PlayerbotAI*);
bool ApplyMemoryRemember   (const MemoryRememberCall&,     PlayerbotAI*);

// Dispatcher: applies a ParsedToolCall variant. Returns true on success.
bool ApplyToolCall(const ParsedToolCall& call, PlayerbotAI* botAI);

}  // namespace LlmAgentTools

#endif
```

- [ ] **Step 2: Write `src/Bot/LlmAgent/Tools/ToolExecutors.cpp`**

```cpp
#include "Tools/ToolExecutors.h"

#ifndef LLMAGENT_UNIT_TESTS

#include "Apply/ApplyGoal.h"
#include "LlmAgentManager.h"
#include "PlayerbotAI.h"
#include "Player.h"
#include "Group.h"
#include "ObjectMgr.h"
#include "ObjectAccessor.h"
#include "Log.h"

namespace LlmAgentTools {

namespace {

Player* find_nearby_player(Player* bot, const std::string& name) {
    if (!bot) return nullptr;
    // Most reliable in WoTLK AC: find by name across map. For Phase 4, fall
    // back to ObjectAccessor::FindPlayerByName.
    return ObjectAccessor::FindPlayerByName(name);
}

Creature* find_nearby_creature(Player* bot, const std::string& name) {
    if (!bot) return nullptr;
    // Simple linear scan of grid creatures (cheap; Phase 4.1 caches).
    Map* m = bot->GetMap();
    if (!m) return nullptr;
    for (auto const& pair : m->GetCreatureBySpawnIdStore()) {
        Creature* c = pair.second;
        if (c && c->GetName() == name && bot->GetDistance(c) <= 10.0f) return c;
    }
    return nullptr;
}

}  // namespace

bool ApplyAcceptPartyInvite(const AcceptPartyInviteCall& c, PlayerbotAI* botAI) {
    if (!botAI) return false;
    Player* bot = botAI->GetBot();
    if (!bot) return false;
    try {
        // The bot will accept the most-recent pending invite. AC's WorldPackets
        // for invite acceptance: SMSG_GROUP_INVITE / CMSG_GROUP_ACCEPT.
        // Pragmatic path: simulate the GROUP_ACCEPT packet handling.
        bot->GetSession()->HandleGroupAcceptOpcode(nullptr);
        return true;
    } catch (...) {
        LOG_ERROR("playerbots", "[LlmAgent] ApplyAcceptPartyInvite threw (from={})", c.from);
        bot->GetSession()->HandleGroupDeclineOpcode(nullptr);
        return false;
    }
}

bool ApplyLeaveParty(const LeavePartyCall&, PlayerbotAI* botAI) {
    if (!botAI) return false;
    Player* bot = botAI->GetBot();
    if (!bot) return false;
    try {
        bot->RemoveFromGroup();
        return true;
    } catch (...) {
        LOG_ERROR("playerbots", "[LlmAgent] ApplyLeaveParty threw");
        return false;
    }
}

bool ApplyAcceptQuest(const AcceptQuestCall& c, PlayerbotAI* botAI) {
    if (!botAI) return false;
    Player* bot = botAI->GetBot();
    if (!bot) return false;
    try {
        const Quest* q = sObjectMgr->GetQuestTemplate(c.quest_id);
        if (!q) return false;
        bot->AddQuestAndCheckCompletion(q, /*questGiver=*/nullptr);
        return true;
    } catch (...) {
        LOG_ERROR("playerbots", "[LlmAgent] ApplyAcceptQuest threw (quest_id={})", c.quest_id);
        return false;
    }
}

bool ApplyTurnInQuest(const TurnInQuestCall& c, PlayerbotAI* botAI) {
    if (!botAI) return false;
    Player* bot = botAI->GetBot();
    if (!bot) return false;
    try {
        const Quest* q = sObjectMgr->GetQuestTemplate(c.quest_id);
        if (!q) return false;
        // Pick the default reward (index 0) — bots can't choose between
        // alternatives in Phase 4. Future: validate against quest.GetRewChoiceItemsCount().
        bot->RewardQuest(q, /*reward=*/0, /*questGiver=*/nullptr);
        return true;
    } catch (...) {
        LOG_ERROR("playerbots", "[LlmAgent] ApplyTurnInQuest threw (quest_id={})", c.quest_id);
        return false;
    }
}

bool ApplySetGoal(const SetGoalCall& c, PlayerbotAI* botAI) {
    return LlmAgentApply::ApplyGoalToRpgInfo(c.goal, botAI);
}

bool ApplyVendorJunk(const VendorJunkCall&, PlayerbotAI* botAI) {
    if (!botAI) return false;
    Player* bot = botAI->GetBot();
    if (!bot) return false;
    try {
        // Iterate inventory; sell grey (quality 0) items.
        for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag) {
            if (Bag* b = bot->GetBagByPos(bag)) {
                uint32 size = b->GetBagSize();
                for (uint32 slot = 0; slot < size; ++slot) {
                    if (Item* item = b->GetItemByPos(slot)) {
                        if (item->GetTemplate() &&
                            item->GetTemplate()->Quality == ITEM_QUALITY_POOR) {
                            // Pragmatic: destroy and credit the sell value.
                            uint32 value = item->GetTemplate()->SellPrice;
                            bot->ModifyMoney(value);
                            bot->DestroyItemCount(item->GetEntry(), item->GetCount(), true);
                        }
                    }
                }
            }
        }
        return true;
    } catch (...) {
        LOG_ERROR("playerbots", "[LlmAgent] ApplyVendorJunk threw");
        return false;
    }
}

bool ApplyMemoryRemember(const MemoryRememberCall& c, PlayerbotAI* botAI) {
    if (!botAI) return false;
    Player* bot = botAI->GetBot();
    if (!bot) return false;
    return LlmAgentManager::Instance().MemoryClient().Remember(
        bot->GetGUID().GetRawValue(),
        c.text, c.entities, c.salience, c.relations);
}

bool ApplyToolCall(const ParsedToolCall& call, PlayerbotAI* botAI) {
    return std::visit([botAI](auto const& tool) {
        using T = std::decay_t<decltype(tool)>;
        if constexpr (std::is_same_v<T, AcceptPartyInviteCall>)
            return ApplyAcceptPartyInvite(tool, botAI);
        else if constexpr (std::is_same_v<T, LeavePartyCall>)
            return ApplyLeaveParty(tool, botAI);
        else if constexpr (std::is_same_v<T, AcceptQuestCall>)
            return ApplyAcceptQuest(tool, botAI);
        else if constexpr (std::is_same_v<T, TurnInQuestCall>)
            return ApplyTurnInQuest(tool, botAI);
        else if constexpr (std::is_same_v<T, SetGoalCall>)
            return ApplySetGoal(tool, botAI);
        else if constexpr (std::is_same_v<T, VendorJunkCall>)
            return ApplyVendorJunk(tool, botAI);
        else if constexpr (std::is_same_v<T, MemoryRememberCall>)
            return ApplyMemoryRemember(tool, botAI);
        return false;
    }, call);
}

}  // namespace LlmAgentTools

#endif
```

- [ ] **Step 3: Build unit tests — still ~139 pass**

The new code is `#ifndef LLMAGENT_UNIT_TESTS`-guarded.

- [ ] **Step 4: Commit**

```bash
git add src/Bot/LlmAgent/Tools/ToolExecutors.h src/Bot/LlmAgent/Tools/ToolExecutors.cpp
git commit -m "feat(llm-agent): ToolExecutors — apply each variant to AC primitives

One Apply<Tool> per tool, wrapped in try/catch. SetGoalCall reuses
Phase 2's ApplyGoalToRpgInfo. MemoryRememberCall delegates to
Phase 3's MemoryHttpClient. AcceptPartyInvite uses HandleGroupAccept
opcode handler; LeaveParty calls RemoveFromGroup. VendorJunk
iterates inventory and sells quality-0 items.

If AC API names have drifted on this base, the Heimdal build will
surface specific errors per the established Phase 1-3 pattern."
```

---

## Task 9: `LlmInteractTrigger` + `LlmInteractAction` + Strategy registration

**Files:**
- Create: `src/Bot/LlmAgent/Triggers/LlmInteractTrigger.h`
- Create: `src/Bot/LlmAgent/Triggers/LlmInteractTrigger.cpp`
- Create: `src/Bot/LlmAgent/Actions/LlmInteractAction.h`
- Create: `src/Bot/LlmAgent/Actions/LlmInteractAction.cpp`
- Create: `src/Bot/LlmAgent/Context/LlmAgentTier2ActionContext.h`
- Create: `src/Bot/LlmAgent/Context/LlmAgentTier2TriggerContext.h`
- Modify: `src/Bot/LlmAgent/Strategy/LlmAgentStrategy.cpp`
- Modify: `src/Bot/Engine/BuildSharedActionContexts.cpp`
- Modify: `src/Bot/Engine/BuildSharedTriggerContexts.cpp`

- [ ] **Step 1: Write `Triggers/LlmInteractTrigger.h`**

```cpp
#ifndef _PLAYERBOT_LLM_INTERACT_TRIGGER_H
#define _PLAYERBOT_LLM_INTERACT_TRIGGER_H

#include "Trigger.h"

class LlmInteractTrigger : public Trigger {
  public:
    LlmInteractTrigger(PlayerbotAI* ai) : Trigger(ai, "llm interact") {}
    bool IsActive() override;
};

#endif
```

- [ ] **Step 2: Write `Triggers/LlmInteractTrigger.cpp`**

```cpp
#include "Triggers/LlmInteractTrigger.h"
#include "LlmAgentManager.h"
#include "PlayerbotAI.h"

bool LlmInteractTrigger::IsActive() {
    if (!botAI) return false;
    auto& mgr = LlmAgentManager::Instance();
    const auto& cfg = mgr.Config();
    if (!mgr.Enabled() || !cfg.Tier2_Enabled) return false;
    Player* bot = botAI->GetBot();
    if (!bot) return false;
    uint64_t guid = bot->GetGUID().GetRawValue();

    if (!mgr.Selector().IsLlmBot(guid)) return false;
    if (!mgr.Cooldowns().Eligible(guid)) return false;

    // Expire stale events first.
    mgr.Interactions().ExpireOlderThan(
        guid, static_cast<int64_t>(time(nullptr)),
        cfg.Tier2_WhisperWindowSeconds);
    return mgr.Interactions().HasPending(guid);
}
```

- [ ] **Step 3: Write `Actions/LlmInteractAction.h`**

```cpp
#ifndef _PLAYERBOT_LLM_INTERACT_ACTION_H
#define _PLAYERBOT_LLM_INTERACT_ACTION_H

#include "Action.h"

class LlmInteractAction : public Action {
  public:
    LlmInteractAction(PlayerbotAI* ai) : Action(ai, "llm interact") {}
    bool Execute(Event event) override;
};

#endif
```

- [ ] **Step 4: Write `Actions/LlmInteractAction.cpp`**

```cpp
#include "Actions/LlmInteractAction.h"
#include "LlmAgentManager.h"
#include "Tiers/Tier2_Interactive.h"
#include "Tools/ToolCatalog.h"
#include "Tools/ToolValidators.h"
#include "Tools/ToolExecutors.h"
#include "Tools/InteractionContext.h"
#include "PlayerbotAI.h"
#include "Player.h"
#include "Log.h"

namespace {

const char* tool_call_name(const ParsedToolCall& c) {
    return std::visit([](auto const& t) -> const char* {
        using T = std::decay_t<decltype(t)>;
        if constexpr (std::is_same_v<T, AcceptPartyInviteCall>) return "accept_party_invite";
        else if constexpr (std::is_same_v<T, LeavePartyCall>)   return "leave_party";
        else if constexpr (std::is_same_v<T, AcceptQuestCall>)  return "accept_quest";
        else if constexpr (std::is_same_v<T, TurnInQuestCall>)  return "turn_in_quest";
        else if constexpr (std::is_same_v<T, SetGoalCall>)      return "set_goal";
        else if constexpr (std::is_same_v<T, VendorJunkCall>)   return "vendor_junk";
        else if constexpr (std::is_same_v<T, MemoryRememberCall>) return "memory.remember";
        return "unknown";
    }, c);
}

}  // namespace

bool LlmInteractAction::Execute(Event /*event*/) {
    if (!botAI) return false;
    auto& mgr = LlmAgentManager::Instance();
    const auto& cfg = mgr.Config();
    if (!mgr.Enabled() || !cfg.Tier2_Enabled) return false;
    Player* bot = botAI->GetBot();
    if (!bot) return false;
    const uint64_t guid = bot->GetGUID().GetRawValue();

    bool applied_any = false;

    // 1. Drain T2 results.
    auto results = mgr.DrainResults(guid, /*tier=*/2);
    for (const auto& r : results) {
        if (r.parsed_status != "ok") {
            mgr.Counters().IncFallbackUsed();
            mgr.Cooldowns().Set(guid,
                std::chrono::steady_clock::now() +
                std::chrono::milliseconds(cfg.FallbackCooldownMs));
            continue;
        }
        auto parsed = ParseToolCalls(r.raw_response);
        if (std::holds_alternative<ParseError>(parsed)) {
            mgr.Counters().IncToolSchemaError();
            mgr.Cooldowns().Set(guid,
                std::chrono::steady_clock::now() +
                std::chrono::milliseconds(cfg.FallbackCooldownMs));
            continue;
        }
        const auto& calls = std::get<std::vector<ParsedToolCall>>(parsed);
        if (calls.empty()) {
            mgr.Counters().IncToolNoAction();
            mgr.Cooldowns().Set(guid,
                std::chrono::steady_clock::now() +
                std::chrono::milliseconds(cfg.FallbackCooldownMs));
            continue;
        }
        uint32_t truncate_at = std::min<uint32_t>(
            cfg.Tier2_MaxToolsPerResponse, static_cast<uint32_t>(calls.size()));
        if (calls.size() > cfg.Tier2_MaxToolsPerResponse)
            mgr.Counters().IncToolTruncated();

        InteractionContext ctx = SnapshotInteractionContext(botAI);
        for (uint32_t i = 0; i < truncate_at; ++i) {
            const auto& call = calls[i];
            mgr.Counters().IncToolReceived(tool_call_name(call));

            auto decision = std::visit([&](auto const& t) {
                return Validate(t, ctx);
            }, call);
            if (!decision.accepted) {
                mgr.Counters().IncToolRejected(decision.reject_reason);
                // memory.remember rejection doesn't halt; everything else does.
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
        // Clear this bot's interaction events after a successful drain so we
        // don't re-fire on the next tick.
        mgr.Interactions().Clear(guid);
        // Cooldown after T2 — match the rule of thumb: 5 min so the bot can
        // resume rule-based behavior before being re-prompted by a human.
        mgr.Cooldowns().Set(guid,
            std::chrono::steady_clock::now() +
            std::chrono::milliseconds(cfg.FallbackCooldownMs));
    }

    // 2. Enqueue a new T2 request if eligible (no in-flight + has pending events).
    if (!applied_any &&
        mgr.Selector().IsLlmBot(guid) &&
        mgr.Cooldowns().Eligible(guid) &&
        !mgr.IsInFlight(guid, /*tier=*/2) &&
        mgr.Interactions().HasPending(guid))
    {
        LlmRequest req;
        req.bot_guid = guid;
        req.bot_name = bot->GetName();
        req.body_json = LlmAgentTier2::BuildT2RequestBody(botAI);
        req.digest_json = LlmAgentTier2::BuildT2Digest(botAI);
        req.tier = 2;
        mgr.Enqueue(std::move(req));
    }

    return applied_any;  // true suppresses T1 status_update
}
```

- [ ] **Step 5: Write the two Context headers**

`src/Bot/LlmAgent/Context/LlmAgentTier2ActionContext.h`:

```cpp
#ifndef _PLAYERBOT_LLMAGENT_TIER2_ACTION_CONTEXT_H
#define _PLAYERBOT_LLMAGENT_TIER2_ACTION_CONTEXT_H

#include "NamedObjectContext.h"
#include "Actions/LlmInteractAction.h"

class LlmAgentTier2ActionContext : public NamedObjectContext<Action> {
  public:
    LlmAgentTier2ActionContext() {
        creators["llm interact"] = &LlmAgentTier2ActionContext::llm_interact;
    }
  private:
    static Action* llm_interact(PlayerbotAI* ai) { return new LlmInteractAction(ai); }
};

#endif
```

`src/Bot/LlmAgent/Context/LlmAgentTier2TriggerContext.h`:

```cpp
#ifndef _PLAYERBOT_LLMAGENT_TIER2_TRIGGER_CONTEXT_H
#define _PLAYERBOT_LLMAGENT_TIER2_TRIGGER_CONTEXT_H

#include "NamedObjectContext.h"
#include "Triggers/LlmInteractTrigger.h"

class LlmAgentTier2TriggerContext : public NamedObjectContext<Trigger> {
  public:
    LlmAgentTier2TriggerContext() {
        creators["llm interact"] = &LlmAgentTier2TriggerContext::llm_interact;
    }
  private:
    static Trigger* llm_interact(PlayerbotAI* ai) { return new LlmInteractTrigger(ai); }
};

#endif
```

- [ ] **Step 6: Register the new trigger node in `LlmAgentStrategy.cpp`**

Inside `InitTriggers`, add (alongside the existing "llm replan idle" registration):

```cpp
    triggers.push_back(
        new TriggerNode(
            "llm interact",
            {NextAction("llm interact", 16.0f)}   // > T1's 15.0f
        )
    );
```

- [ ] **Step 7: Wire the new contexts in `BuildShared*Contexts.cpp`**

In `src/Bot/Engine/BuildSharedActionContexts.cpp`, add include and add-line for `LlmAgentTier2ActionContext`. Same for `BuildSharedTriggerContexts.cpp`.

- [ ] **Step 8: Build unit tests — still ~139 pass**

```bash
cmake --build build/llmagent_tests
./build/llmagent_tests/llmagent_unit_tests
```

- [ ] **Step 9: Commit**

```bash
git add src/Bot/LlmAgent/Triggers/LlmInteractTrigger.h \
        src/Bot/LlmAgent/Triggers/LlmInteractTrigger.cpp \
        src/Bot/LlmAgent/Actions/LlmInteractAction.h \
        src/Bot/LlmAgent/Actions/LlmInteractAction.cpp \
        src/Bot/LlmAgent/Context/LlmAgentTier2ActionContext.h \
        src/Bot/LlmAgent/Context/LlmAgentTier2TriggerContext.h \
        src/Bot/LlmAgent/Strategy/LlmAgentStrategy.cpp \
        src/Bot/Engine/BuildSharedActionContexts.cpp \
        src/Bot/Engine/BuildSharedTriggerContexts.cpp
git commit -m "feat(llm-agent): LlmInteractTrigger + LlmInteractAction + registration

T2 trigger at relevance 16.0 outranks T1's 15.0. Drains T2 results
(validating each tool call, applying accepted ones sequentially with
stop-on-reject — except memory.remember which never halts). Enqueues
a new T2 request when interaction events are pending and no T2
in-flight. Clears bot's interaction buffer post-drain to prevent
re-firing on the next tick."
```

---

## Task 10: Wire tier-aware in-flight + DrainResults into `LlmAgentManager`

**Files:**
- Modify: `src/Bot/LlmAgent/LlmAgentManager.h`
- Modify: `src/Bot/LlmAgent/LlmAgentManager.cpp`

T2 needs an in-flight set keyed by tier (a bot can have a T1 in-flight while also having a T2 result pending). The same for DrainResults.

- [ ] **Step 1: Update `LlmRequest` and `LlmResult` to carry `tier`**

In `LlmAgentManager.h`, add to both structs:

```cpp
    uint32_t tier = 1;   // 1 = T1 (replan), 2 = T2 (interactive)
```

- [ ] **Step 2: Extend `IsInFlight` and `DrainResults` signatures**

```cpp
    bool IsInFlight(uint64_t bot_guid, uint32_t tier = 1) const;
    std::vector<LlmResult> DrainResults(uint64_t bot_guid, uint32_t tier = 1);
```

- [ ] **Step 3: Update the manager's internal state**

Replace `std::unordered_set<uint64_t> inflight_` with:

```cpp
    std::unordered_map<uint64_t, std::unordered_set<uint32_t>> inflight_;  // bot_guid → tiers
```

And `results_` similarly keyed by tier:

```cpp
    std::unordered_map<std::pair<uint64_t, uint32_t>,
                       std::stack<LlmResult>,
                       PairHash> results_;
```

(Add a `PairHash` helper at the top of the cpp.)

- [ ] **Step 4: Update all callsites**

Phase 2's `LlmReplanIdleAction` passes `tier=1` implicitly via default. Phase 4's `LlmInteractAction` passes `tier=2` explicitly. The manager's worker thread sets `result.tier` from the request's tier before pushing.

- [ ] **Step 5: Run all unit tests — still ~139 pass**

The Phase 2 `test_manager_threading.cpp` uses default tier (no change). Phase 4 doesn't add new manager-level tests (covered indirectly via Tier2 action).

```bash
cmake --build build/llmagent_tests
./build/llmagent_tests/llmagent_unit_tests
```

- [ ] **Step 6: Commit**

```bash
git add src/Bot/LlmAgent/LlmAgentManager.h src/Bot/LlmAgent/LlmAgentManager.cpp
git commit -m "feat(llm-agent): tier-aware in-flight + DrainResults in Manager

LlmRequest and LlmResult carry tier (1 = T1, 2 = T2). A bot can have
both T1 and T2 in-flight independently. DrainResults filters by tier.
Phase 2's existing call paths use the default tier=1; Phase 4's action
passes tier=2 explicitly. Worker thread propagates request.tier to
result.tier."
```

---

## Task 11: Admin command `.playerbots t2 inject`

**Files:**
- Modify: `src/Script/PlayerbotCommandScript.cpp`

Lets us drive a T2 trigger without needing a real player in-game (per the spec deviation note).

- [ ] **Step 1: Add command + handler in `PlayerbotCommandScript.cpp`**

Inside the existing `playerbotsDebugCommandTable` (or a new sub-table for `t2`):

```cpp
    static ChatCommandTable playerbotsT2CommandTable = {
        { "inject_whisper", HandleT2InjectWhisper, SEC_GAMEMASTER, Console::Yes },
        { "inject_invite",  HandleT2InjectInvite,  SEC_GAMEMASTER, Console::Yes },
    };
    // ... at top level:
    playerbotsCommandTable.push_back({ "t2", playerbotsT2CommandTable });
```

Handlers:

```cpp
    static bool HandleT2InjectWhisper(ChatHandler* handler, char const* args) {
        // args = "<bot_name> <text>"
        std::string a(args);
        auto sp = a.find(' ');
        if (sp == std::string::npos) {
            handler->PSendSysMessage("usage: .playerbots t2 inject_whisper <bot_name> <text>");
            return true;
        }
        std::string bot_name = a.substr(0, sp);
        std::string text     = a.substr(sp + 1);
        Player* bot = ObjectAccessor::FindPlayerByName(bot_name);
        if (!bot) {
            handler->PSendSysMessage("bot %s not found", bot_name.c_str());
            return true;
        }
        auto& mgr = LlmAgentManager::Instance();
        if (mgr.Config().SocialOptIn)
            mgr.Selector().OptInBot(bot->GetGUID().GetRawValue());
        mgr.Interactions().PushWhisper(
            bot->GetGUID().GetRawValue(),
            handler->GetSession() ? handler->GetSession()->GetPlayerName() : "GM",
            handler->GetSession() && handler->GetSession()->GetPlayer()
                ? handler->GetSession()->GetPlayer()->GetGUID().GetRawValue() : 0,
            text,
            static_cast<int64_t>(time(nullptr)));
        handler->PSendSysMessage("T2 whisper injected for %s", bot_name.c_str());
        return true;
    }

    static bool HandleT2InjectInvite(ChatHandler* handler, char const* args) {
        std::string bot_name(args);
        Player* bot = ObjectAccessor::FindPlayerByName(bot_name);
        if (!bot) {
            handler->PSendSysMessage("bot %s not found", bot_name.c_str());
            return true;
        }
        auto& mgr = LlmAgentManager::Instance();
        mgr.Selector().OptInBot(bot->GetGUID().GetRawValue());
        mgr.Interactions().PushInvite(
            bot->GetGUID().GetRawValue(),
            handler->GetSession() ? handler->GetSession()->GetPlayerName() : "GM",
            0,
            static_cast<int64_t>(time(nullptr)));
        handler->PSendSysMessage("T2 invite injected for %s", bot_name.c_str());
        return true;
    }
```

Include at the top:

```cpp
#include "Bot/LlmAgent/LlmAgentManager.h"
```

- [ ] **Step 2: Build unit tests — still ~139 pass**

```bash
cmake --build build/llmagent_tests
./build/llmagent_tests/llmagent_unit_tests
```

- [ ] **Step 3: Commit**

```bash
git add src/Script/PlayerbotCommandScript.cpp
git commit -m "feat(llm-agent): .playerbots t2 inject_{whisper,invite} admin commands

Out-of-band trigger for T2 — pushes synthetic events into
InteractionEventBuffer without needing a real connected player.
Useful for Phase 4 mechanical validation (smoke test from GM
console) before doing the real-player demo."
```

---

## Task 12: `conf/playerbots.conf.dist` append

**Files:**
- Modify: `conf/playerbots.conf.dist`

- [ ] **Step 1: Append**

```ini

####################################################################################################
# LLM AGENT (Phase 4 — T2 interactive tool-calling)
#
# T2 fires when a real player engages with a bot via whisper, party invite,
# or group join. The bot can emit tool calls — accept_party_invite, set_goal,
# accept_quest, etc. — which are validated server-side before execution.

# Default: 1 (enabled when LlmAgent.Enabled=1)
AiPlayerbot.LlmAgent.Tier2.Enabled = 1

# Hard cap on tool calls applied per LLM response (parsed in order, stop on
# first validation failure except memory.remember which is non-blocking).
# Default: 3
AiPlayerbot.LlmAgent.Tier2.MaxToolsPerResponse = 3

# Whispers older than this don't trigger T2.
# Default: 120 (2 minutes)
AiPlayerbot.LlmAgent.Tier2.WhisperWindowSeconds = 120

# Override the default T2 system prompt (leave empty to use the built-in).
AiPlayerbot.LlmAgent.Tier2.SystemPrompt = ""
```

- [ ] **Step 2: Commit**

```bash
git add conf/playerbots.conf.dist
git commit -m "feat(llm-agent): playerbots.conf.dist gets four Tier2 keys

Tier2.{Enabled, MaxToolsPerResponse, WhisperWindowSeconds, SystemPrompt}."
```

---

## Task 13: Build worldserver on Heimdal + smoke test

**Files:**
- Create: `results/2026-05-13-llm-phase-4-smoke/summary.md` (Step 7)

- [ ] **Step 1: Push branch**

```bash
git push -u origin claude/llm-agent-phase-4-t2-interactive
```

- [ ] **Step 2: Trigger the Heimdal rebuild**

```bash
cd ~/Documents/Projects/azerothcore-heimdal && \
  PLAYERBOTS_BRANCH=claude/llm-agent-phase-4-t2-interactive ./image/build.sh 2>&1 | \
  tee /tmp/wow-build-llm-phase4.log
```

Expected: ~3-8 min with warm ccache. AC API mismatches surface here (especially the `HandleGroupAcceptOpcode` / `AddQuestAndCheckCompletion` / `OnPlayerInviteToGroup` script-hook names). Fix inline and re-trigger. Same Phase 1-3 pattern.

- [ ] **Step 3: Update Heimdal playerbots.conf for the smoke test**

```bash
cat > /tmp/p4_conf_block.txt <<'EOF'

####################################################################################################
# LLM AGENT — Phase 4 smoke
####################################################################################################
AiPlayerbot.LlmAgent.Enabled = 1
AiPlayerbot.LlmAgent.Endpoint = "http://192.168.1.3:8080"
AiPlayerbot.LlmAgent.Model = "qwen2.5-7b-instruct-q4_k_m-00001-of-00002.gguf"
AiPlayerbot.LlmAgent.WorkerThreads = 4
AiPlayerbot.LlmAgent.RequestTimeoutMs = 15000
AiPlayerbot.LlmAgent.JsonlPath = "/azerothcore/env/dist/logs/llm_agent_phase4.jsonl"
AiPlayerbot.LlmAgent.SystemPrompt = ""
AiPlayerbot.LlmAgent.ApplyMode = "apply"
AiPlayerbot.LlmAgent.SamplePct = 100
AiPlayerbot.LlmAgent.SocialOptIn = 1
AiPlayerbot.LlmAgent.MaxCooldownMinutes = 60
AiPlayerbot.LlmAgent.FallbackCooldownMs = 300000
AiPlayerbot.LlmAgent.EventLogSize = 20
AiPlayerbot.MemorySidecar.Endpoint = "http://192.168.1.3:8090"
AiPlayerbot.MemorySidecar.RequestTimeoutMs = 2000
AiPlayerbot.MemorySidecar.EnableWrites = 1
AiPlayerbot.MemorySidecar.RecallTopK = 3
AiPlayerbot.MemorySidecar.HintMaxChars = 1200
AiPlayerbot.LlmAgent.Tier2.Enabled = 1
AiPlayerbot.LlmAgent.Tier2.MaxToolsPerResponse = 3
AiPlayerbot.LlmAgent.Tier2.WhisperWindowSeconds = 120
AiPlayerbot.LlmAgent.Tier2.SystemPrompt = ""
EOF
scp /tmp/p4_conf_block.txt heimdal:/tmp/p4_conf_block.txt
ssh heimdal 'echo "101423" | sudo -S bash -c "
  LINE=\$(grep -n \"^####.*LLM AGENT . Phase\" /opt/containers/wow/etc/modules/playerbots.conf | head -1 | cut -d: -f1 || true)
  if [ -n \"\$LINE\" ]; then sed -i \"\$LINE,\\\$d\" /opt/containers/wow/etc/modules/playerbots.conf; fi
  cat /tmp/p4_conf_block.txt >> /opt/containers/wow/etc/modules/playerbots.conf
"'
```

`SamplePct=100` so any bot you whisper is eligible.

- [ ] **Step 4: Restart worldserver**

```bash
ssh heimdal 'echo "101423" | sudo -S rm -f /opt/containers/wow/logs/llm_agent_phase4.jsonl && echo "101423" | sudo -S systemctl restart wow-worldserver.service && sleep 35 && systemctl is-active wow-worldserver.service'
```

- [ ] **Step 5: Mechanical smoke via admin command**

From the GM console (via `scripts/gm.sh` or a direct podman attach):

```
.playerbots t2 inject_whisper <some-bot-name> "come group up at SW gates"
```

Wait ~10 seconds. Then:

```bash
ssh heimdal 'wc -l /opt/containers/wow/logs/llm_agent_phase4.jsonl && jq -c "select(.tier == 2)" /opt/containers/wow/logs/llm_agent_phase4.jsonl | head -3'
```

Expected: at least one record with `tier=2` and `tool_calls` populated. The bot's state in-game should reflect the LLM-dispatched action (set_goal applied, or whatever the LLM chose).

- [ ] **Step 6: (Optional) Real-player demo**

The user logs into the WoW client, whispers a different bot, observes the bot react. Same JSONL inspection as Step 5.

- [ ] **Step 7: Capture results**

```bash
mkdir -p results/2026-05-13-llm-phase-4-smoke
ssh heimdal 'jq -c "select(.tier == 2) | {bot_name, parsed_status, inference_ms, tool_calls}" /opt/containers/wow/logs/llm_agent_phase4.jsonl | head -10' > results/2026-05-13-llm-phase-4-smoke/sample_records.jsonl
```

Write `results/2026-05-13-llm-phase-4-smoke/summary.md` following the established shape:
- TL;DR + Phase 4 invariant check
- Run setup
- Headline numbers (records, tool-call distribution by name, validator decisions, inference latency, tick latency)
- §11 success criteria check (6 items)
- Findings
- Operator hand-off state

- [ ] **Step 8: Disable T2 before walking away (optional)**

```bash
ssh heimdal 'echo "101423" | sudo -S sed -i "s|AiPlayerbot.LlmAgent.Enabled = 1|AiPlayerbot.LlmAgent.Enabled = 0|" /opt/containers/wow/etc/modules/playerbots.conf && echo "101423" | sudo -S systemctl restart wow-worldserver.service'
```

- [ ] **Step 9: Commit results**

```bash
git add results/2026-05-13-llm-phase-4-smoke/
git commit -m "test(llm-agent): record Phase 4 T2 smoke-test results"
git push origin claude/llm-agent-phase-4-t2-interactive
```

---

## Phase 4 success criteria

All six must hold:

1. **C++ tests:** ≈ 143 / 143 (Phase 3's 93 + ~50 Phase 4).
2. **Python sidecar tests:** still 45 / 45 (unchanged from Phase 3).
3. **`Tier2.Enabled = 0`:** zero behavior change from Phase 3.
4. **`LlmAgent.Enabled = 0`:** byte-identical baseline.
5. **At least one observed end-to-end:** real human (or admin-command-injected whisper) → T2 fires → tool call validates + applies → bot acts. Logged in JSONL with `tool_calls.applied >= 1`.
6. **No worldserver-tick regression:** mean ≤ 20 ms, p95 ≤ 50 ms.

When all six hold, Phase 4 is done. Phase 5 (T3 chat brain) is the natural next phase per parent design §12.

---

## Plan deviations from spec

Two intentional simplifications versus the spec:

1. **`InteractionContext.nearby_creatures` stays empty in Phase 4's `SnapshotInteractionContext`.** Validators for `accept_quest`, `turn_in_quest`, `vendor_junk` will hard-reject for now (rule-based fallback continues to handle those flows). Phase 4.1 wires real grid scans + creature template lookups for quest-giver / turn-in / vendor flags. Same posture as Phase 3 (which left `nearby_creature_guids` empty for the same reason).

2. **`.playerbots t2 inject_*` admin commands** are added beyond the spec's scope. The spec lists this as a Phase 4.1 follow-up; the plan promotes it to Phase 4 because it's the only practical way to validate the path mechanically without needing a real connected player in the smoke test.

Both deviations are documented in the corresponding commits.
