# LLM-Agent Phase 1 Plumbing Spike Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Wire the C++ plumbing that lets a worldserver-side action build a T0 state digest, send it to llama-server, parse the response, and record the proposed `NewRpgInfo` transition — log-only, never applied.

**Architecture:** New module subtree `src/Bot/LlmAgent/` registered via three new context classes (`LlmAgentActionContext`, `LlmAgentTriggerContext`, `LlmAgentStrategyContext`). A singleton `LlmAgentManager` owns a 4-worker thread pool, MPSC request queue, per-bot in-flight set, per-bot result stack, and a JSONL writer. `LlmReplanIdleAction` fires on `RPG_IDLE` to enqueue requests and drain results; gameplay is never modified.

**Tech Stack:** C++17 (existing codebase standard). Three vendored single-header libraries: `cpp-httplib`, `nlohmann/json`, `doctest`. Test target builds with the system CMake separately from the worldserver build.

**Spec:** [`docs/superpowers/specs/2026-05-12-llm-agent-phase-1-plumbing-spike-design.md`](../specs/2026-05-12-llm-agent-phase-1-plumbing-spike-design.md)

---

## File structure

**New files (created by this plan):**

```
src/Bot/LlmAgent/
  LlmAgentManager.h
  LlmAgentManager.cpp
  LlmAgentConfig.h
  LlmAgentConfig.cpp
  Client/
    LlmHttpClient.h
    LlmHttpClient.cpp
  Tiers/
    Tier0_StateDigest.h
    Tier0_StateDigest.cpp
  Schemas/
    Goal.h
    Goal.cpp
  Strategy/
    LlmAgentStrategy.h
    LlmAgentStrategy.cpp
  Triggers/
    LlmReplanIdleTrigger.h
    LlmReplanIdleTrigger.cpp
  Actions/
    LlmReplanIdleAction.h
    LlmReplanIdleAction.cpp
  Context/
    LlmAgentActionContext.h
    LlmAgentTriggerContext.h
    LlmAgentStrategyContext.h
  Vendor/
    httplib.h            (vendored, ~10k lines)
    nlohmann_json.hpp    (vendored, ~25k lines)

tests/llmagent/
  CMakeLists.txt
  doctest_main.cpp
  doctest.h              (vendored, ~7k lines)
  test_goal_parser.cpp
  test_digest_shape.cpp
  test_http_client.cpp
  test_manager_threading.cpp
  test_config_load.cpp
```

**Existing files modified:**

- `src/Bot/Engine/BuildSharedActionContexts.cpp` — register `LlmAgentActionContext`
- `src/Bot/Engine/BuildSharedTriggerContexts.cpp` — register `LlmAgentTriggerContext`
- `src/Bot/Engine/BuildSharedStrategyContexts.cpp` — register `LlmAgentStrategyContext`
- `src/Bot/Factory/AiFactory.cpp` — conditionally add `"llm agent"` strategy to `nonCombatEngine`
- `src/PlayerbotAIConfig.h` and `src/PlayerbotAIConfig.cpp` — wire `LlmAgentConfig::Load()` from the existing config loader
- `conf/playerbots.conf.dist` — seven new `AiPlayerbot.LlmAgent.*` keys
- Worldserver shutdown hook — call `LlmAgentManager::Instance().Shutdown()` (location identified in Task 13)

---

## Task 0: Create feature branch

**Files:** none

- [ ] **Step 1: Verify working tree clean**

```bash
git status
```

Expected: `nothing to commit, working tree clean`. If not clean, stop and resolve before continuing.

- [ ] **Step 2: Branch off main**

```bash
git checkout -b claude/llm-agent-phase-1-plumbing-spike
git status
```

Expected: `On branch claude/llm-agent-phase-1-plumbing-spike`.

- [ ] **Step 3: No commit (branch creation only)**

The branch exists locally. Future tasks commit onto it.

---

## Task 1: Vendor cpp-httplib, nlohmann/json, doctest

**Files:**
- Create: `src/Bot/LlmAgent/Vendor/httplib.h`
- Create: `src/Bot/LlmAgent/Vendor/nlohmann_json.hpp`
- Create: `tests/llmagent/doctest.h`
- Create: `src/Bot/LlmAgent/Vendor/LICENSE-cpp-httplib.txt`
- Create: `src/Bot/LlmAgent/Vendor/LICENSE-nlohmann-json.txt`
- Create: `tests/llmagent/LICENSE-doctest.txt`

- [ ] **Step 1: Create vendor directories**

```bash
mkdir -p src/Bot/LlmAgent/Vendor
mkdir -p tests/llmagent
```

- [ ] **Step 2: Download cpp-httplib v0.18.5**

```bash
curl -L -o src/Bot/LlmAgent/Vendor/httplib.h \
  https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.18.5/httplib.h
curl -L -o src/Bot/LlmAgent/Vendor/LICENSE-cpp-httplib.txt \
  https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.18.5/LICENSE
wc -l src/Bot/LlmAgent/Vendor/httplib.h
```

Expected: roughly 10000+ lines downloaded.

- [ ] **Step 3: Download nlohmann/json v3.11.3**

```bash
curl -L -o src/Bot/LlmAgent/Vendor/nlohmann_json.hpp \
  https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp
curl -L -o src/Bot/LlmAgent/Vendor/LICENSE-nlohmann-json.txt \
  https://raw.githubusercontent.com/nlohmann/json/v3.11.3/LICENSE.MIT
wc -l src/Bot/LlmAgent/Vendor/nlohmann_json.hpp
```

Expected: roughly 25000+ lines downloaded.

- [ ] **Step 4: Download doctest v2.4.11**

```bash
curl -L -o tests/llmagent/doctest.h \
  https://raw.githubusercontent.com/doctest/doctest/v2.4.11/doctest/doctest.h
curl -L -o tests/llmagent/LICENSE-doctest.txt \
  https://raw.githubusercontent.com/doctest/doctest/v2.4.11/LICENSE.txt
wc -l tests/llmagent/doctest.h
```

Expected: roughly 7000+ lines downloaded.

- [ ] **Step 5: Sanity check headers compile**

```bash
echo '#include "Vendor/httplib.h"
#include "Vendor/nlohmann_json.hpp"
int main(){return 0;}' > /tmp/check.cpp
g++ -std=c++17 -I src/Bot/LlmAgent /tmp/check.cpp -lpthread -o /tmp/check
echo "OK"
```

Expected: `OK` printed; no compile errors.

- [ ] **Step 6: Commit**

```bash
git add src/Bot/LlmAgent/Vendor/ tests/llmagent/doctest.h tests/llmagent/LICENSE-doctest.txt
git commit -m "vendor(llm-agent): pin cpp-httplib 0.18.5, nlohmann/json 3.11.3, doctest 2.4.11

Three single-header libraries vendored for the Phase 1 plumbing spike.
Licenses preserved verbatim; all MIT (compatible with AGPL).
Pinned commit hashes:
- cpp-httplib v0.18.5
- nlohmann/json v3.11.3
- doctest v2.4.11"
```

---

## Task 2: Test scaffold — empty doctest binary

**Files:**
- Create: `tests/llmagent/CMakeLists.txt`
- Create: `tests/llmagent/doctest_main.cpp`

- [ ] **Step 1: Write `tests/llmagent/CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.16)
project(llmagent_unit_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT DEFINED VENDOR_DIR)
  set(VENDOR_DIR "${CMAKE_SOURCE_DIR}/../../src/Bot/LlmAgent/Vendor")
endif()

if(NOT DEFINED LLMAGENT_DIR)
  set(LLMAGENT_DIR "${CMAKE_SOURCE_DIR}/../../src/Bot/LlmAgent")
endif()

find_package(Threads REQUIRED)

add_executable(llmagent_unit_tests
  doctest_main.cpp
)

target_include_directories(llmagent_unit_tests PRIVATE
  ${CMAKE_SOURCE_DIR}
  ${VENDOR_DIR}
  ${LLMAGENT_DIR}
)

target_link_libraries(llmagent_unit_tests PRIVATE
  Threads::Threads
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(llmagent_unit_tests PRIVATE
    -Wall -Wextra -Wpedantic -Wno-unused-parameter
  )
endif()
```

- [ ] **Step 2: Write `tests/llmagent/doctest_main.cpp`**

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

TEST_CASE("doctest scaffold smoke") {
    CHECK(1 + 1 == 2);
}
```

- [ ] **Step 3: Build and run**

```bash
cmake -S tests/llmagent -B build/llmagent_tests
cmake --build build/llmagent_tests
./build/llmagent_tests/llmagent_unit_tests
```

Expected last line: `[doctest] Status: SUCCESS!`

- [ ] **Step 4: Add `build/` to `.gitignore` if not already present**

```bash
grep -q '^build/$' .gitignore || echo 'build/' >> .gitignore
git diff .gitignore
```

- [ ] **Step 5: Commit**

```bash
git add tests/llmagent/CMakeLists.txt tests/llmagent/doctest_main.cpp .gitignore
git commit -m "test(llm-agent): empty doctest binary scaffold

CMake target that builds standalone (not driven by AzerothCore's parent
build). One smoke test verifies the binary runs end-to-end.

Build with: cmake -S tests/llmagent -B build/llmagent_tests && cmake --build build/llmagent_tests
Run with:   ./build/llmagent_tests/llmagent_unit_tests"
```

---

## Task 3: LlmAgentConfig — POD + loader

**Files:**
- Create: `src/Bot/LlmAgent/LlmAgentConfig.h`
- Create: `src/Bot/LlmAgent/LlmAgentConfig.cpp`
- Create: `tests/llmagent/test_config_load.cpp`
- Modify: `tests/llmagent/CMakeLists.txt`

- [ ] **Step 1: Write failing test `tests/llmagent/test_config_load.cpp`**

```cpp
#include "doctest.h"
#include "LlmAgentConfig.h"
#include <unordered_map>
#include <string>

namespace {

struct StubConfigSource {
    std::unordered_map<std::string, std::string> values;

    template <typename T>
    T Get(const char* key, T default_value) const {
        auto it = values.find(key);
        if (it == values.end()) return default_value;
        if constexpr (std::is_same_v<T, bool>) {
            return it->second == "1" || it->second == "true";
        } else if constexpr (std::is_same_v<T, int> || std::is_same_v<T, uint32_t>) {
            return static_cast<T>(std::stoi(it->second));
        } else {
            return it->second;
        }
    }
};

} // namespace

TEST_CASE("LlmAgentConfig defaults when nothing set") {
    StubConfigSource src;
    LlmAgentConfig cfg = LoadLlmAgentConfig(src);

    CHECK(cfg.Enabled == false);
    CHECK(cfg.Endpoint == "http://127.0.0.1:8080");
    CHECK(cfg.Model == "qwen2.5-7b-instruct-q4_k_m.gguf");
    CHECK(cfg.WorkerThreads == 4u);
    CHECK(cfg.RequestTimeoutMs == 15000u);
    CHECK(cfg.JsonlPath == "logs/llm_agent_phase1.jsonl");
    CHECK(!cfg.SystemPrompt.empty());
}

TEST_CASE("LlmAgentConfig overrides applied") {
    StubConfigSource src;
    src.values["AiPlayerbot.LlmAgent.Enabled"] = "1";
    src.values["AiPlayerbot.LlmAgent.Endpoint"] = "http://10.0.0.5:9000";
    src.values["AiPlayerbot.LlmAgent.Model"] = "custom.gguf";
    src.values["AiPlayerbot.LlmAgent.WorkerThreads"] = "8";
    src.values["AiPlayerbot.LlmAgent.RequestTimeoutMs"] = "30000";
    src.values["AiPlayerbot.LlmAgent.JsonlPath"] = "/var/log/llm.jsonl";
    src.values["AiPlayerbot.LlmAgent.SystemPrompt"] = "Custom prompt.";

    LlmAgentConfig cfg = LoadLlmAgentConfig(src);

    CHECK(cfg.Enabled == true);
    CHECK(cfg.Endpoint == "http://10.0.0.5:9000");
    CHECK(cfg.Model == "custom.gguf");
    CHECK(cfg.WorkerThreads == 8u);
    CHECK(cfg.RequestTimeoutMs == 30000u);
    CHECK(cfg.JsonlPath == "/var/log/llm.jsonl");
    CHECK(cfg.SystemPrompt == "Custom prompt.");
}
```

- [ ] **Step 2: Add test source to CMakeLists**

Edit `tests/llmagent/CMakeLists.txt`. Change the `add_executable` line:

```cmake
add_executable(llmagent_unit_tests
  doctest_main.cpp
  test_config_load.cpp
  ${LLMAGENT_DIR}/LlmAgentConfig.cpp
)
```

- [ ] **Step 3: Run test — expect FAIL (file not found)**

```bash
cmake --build build/llmagent_tests 2>&1 | tail -5
```

Expected: error about `LlmAgentConfig.h` not found OR `LlmAgentConfig.cpp` not found.

- [ ] **Step 4: Write `src/Bot/LlmAgent/LlmAgentConfig.h`**

```cpp
#ifndef _PLAYERBOT_LLMAGENT_CONFIG_H
#define _PLAYERBOT_LLMAGENT_CONFIG_H

#include <cstdint>
#include <string>

struct LlmAgentConfig {
    bool        Enabled          = false;
    std::string Endpoint         = "http://127.0.0.1:8080";
    std::string Model            = "qwen2.5-7b-instruct-q4_k_m.gguf";
    uint32_t    WorkerThreads    = 4;
    uint32_t    RequestTimeoutMs = 15000;
    std::string JsonlPath        = "logs/llm_agent_phase1.jsonl";
    std::string SystemPrompt;
};

extern const char* const kDefaultSystemPrompt;

// Generic loader. Source must have a templated Get<T>(const char* key, T default).
template <typename Source>
LlmAgentConfig LoadLlmAgentConfig(const Source& src) {
    LlmAgentConfig cfg;
    cfg.Enabled          = src.template Get<bool>       ("AiPlayerbot.LlmAgent.Enabled",          false);
    cfg.Endpoint         = src.template Get<std::string>("AiPlayerbot.LlmAgent.Endpoint",         std::string{"http://127.0.0.1:8080"});
    cfg.Model            = src.template Get<std::string>("AiPlayerbot.LlmAgent.Model",            std::string{"qwen2.5-7b-instruct-q4_k_m.gguf"});
    cfg.WorkerThreads    = src.template Get<uint32_t>   ("AiPlayerbot.LlmAgent.WorkerThreads",    uint32_t{4});
    cfg.RequestTimeoutMs = src.template Get<uint32_t>   ("AiPlayerbot.LlmAgent.RequestTimeoutMs", uint32_t{15000});
    cfg.JsonlPath        = src.template Get<std::string>("AiPlayerbot.LlmAgent.JsonlPath",        std::string{"logs/llm_agent_phase1.jsonl"});
    cfg.SystemPrompt     = src.template Get<std::string>("AiPlayerbot.LlmAgent.SystemPrompt",     std::string{kDefaultSystemPrompt});
    return cfg;
}

#endif  // _PLAYERBOT_LLMAGENT_CONFIG_H
```

- [ ] **Step 5: Write `src/Bot/LlmAgent/LlmAgentConfig.cpp`**

```cpp
#include "LlmAgentConfig.h"

const char* const kDefaultSystemPrompt =
    "You are an in-world decision-maker for a World of Warcraft NPC. "
    "Given the attached state digest, choose what the character should do next. "
    "Respond with a single JSON object matching the provided schema. "
    "Pick a goal that is plausible for the character's level, location, and "
    "current objectives. Prefer continuing existing quests over starting new "
    "ones when progress is partial. Be concise in `reasoning`.";
```

- [ ] **Step 6: Build and run tests**

```bash
cmake --build build/llmagent_tests
./build/llmagent_tests/llmagent_unit_tests
```

Expected: `[doctest] Status: SUCCESS!` with 3 test cases (scaffold + 2 config).

- [ ] **Step 7: Commit**

```bash
git add src/Bot/LlmAgent/LlmAgentConfig.h src/Bot/LlmAgent/LlmAgentConfig.cpp tests/llmagent/test_config_load.cpp tests/llmagent/CMakeLists.txt
git commit -m "feat(llm-agent): LlmAgentConfig POD + templated loader

Config source is generic (templated Get<T>) so tests use a stub map
and production wires sConfigMgr in a later task. Default system prompt
lives as a const char* in the .cpp."
```

---

## Task 4: Goal schema + parser

**Files:**
- Create: `src/Bot/LlmAgent/Schemas/Goal.h`
- Create: `src/Bot/LlmAgent/Schemas/Goal.cpp`
- Create: `tests/llmagent/test_goal_parser.cpp`
- Modify: `tests/llmagent/CMakeLists.txt`

- [ ] **Step 1: Write failing test `tests/llmagent/test_goal_parser.cpp`**

```cpp
#include "doctest.h"
#include "Schemas/Goal.h"

TEST_CASE("ParseAndValidate accepts valid do_quest goal") {
    const std::string raw = R"({
        "goal": "do_quest",
        "params": {"quest_id": 502, "starting_objective_idx": 0},
        "reasoning": "Continue the existing quest",
        "ttl_minutes": 30
    })";
    auto result = ParseAndValidate(raw);
    REQUIRE(std::holds_alternative<ParsedGoal>(result));
    const auto& g = std::get<ParsedGoal>(result);
    CHECK(g.goal == GoalKind::DoQuest);
    CHECK(g.ttl_minutes == 30);
    REQUIRE(std::holds_alternative<DoQuestParams>(g.params));
    CHECK(std::get<DoQuestParams>(g.params).quest_id == 502u);
    CHECK(std::get<DoQuestParams>(g.params).starting_objective_idx == 0);
}

TEST_CASE("ParseAndValidate accepts valid go_grind goal") {
    const std::string raw = R"({
        "goal": "go_grind",
        "params": {"x": 100.5, "y": -50.0, "z": 12.0, "map_id": 0},
        "reasoning": "Nearby grinding spot",
        "ttl_minutes": 20
    })";
    auto result = ParseAndValidate(raw);
    REQUIRE(std::holds_alternative<ParsedGoal>(result));
    CHECK(std::get<ParsedGoal>(result).goal == GoalKind::GoGrind);
}

TEST_CASE("ParseAndValidate accepts valid rest goal") {
    const std::string raw = R"({
        "goal": "rest",
        "params": {},
        "reasoning": "Take a break",
        "ttl_minutes": 10
    })";
    auto result = ParseAndValidate(raw);
    REQUIRE(std::holds_alternative<ParsedGoal>(result));
    CHECK(std::get<ParsedGoal>(result).goal == GoalKind::Rest);
}

TEST_CASE("ParseAndValidate rejects unknown goal enum") {
    const std::string raw = R"({
        "goal": "do_pvp_arena",
        "params": {},
        "reasoning": "x",
        "ttl_minutes": 10
    })";
    auto result = ParseAndValidate(raw);
    REQUIRE(std::holds_alternative<ParseError>(result));
    CHECK(std::get<ParseError>(result).message.find("goal") != std::string::npos);
}

TEST_CASE("ParseAndValidate rejects missing required field") {
    const std::string raw = R"({
        "goal": "do_quest",
        "params": {"quest_id": 502}
    })";
    auto result = ParseAndValidate(raw);
    REQUIRE(std::holds_alternative<ParseError>(result));
}

TEST_CASE("ParseAndValidate rejects do_quest missing quest_id") {
    const std::string raw = R"({
        "goal": "do_quest",
        "params": {"starting_objective_idx": 0},
        "reasoning": "x",
        "ttl_minutes": 10
    })";
    auto result = ParseAndValidate(raw);
    REQUIRE(std::holds_alternative<ParseError>(result));
}

TEST_CASE("ParseAndValidate rejects malformed JSON") {
    const std::string raw = R"({"goal": "rest", "params":)";
    auto result = ParseAndValidate(raw);
    REQUIRE(std::holds_alternative<ParseError>(result));
}

TEST_CASE("ParseAndValidate rejects empty string") {
    auto result = ParseAndValidate("");
    REQUIRE(std::holds_alternative<ParseError>(result));
}

TEST_CASE("ParseAndValidate rejects ttl_minutes out of range") {
    const std::string raw = R"({
        "goal": "rest",
        "params": {},
        "reasoning": "x",
        "ttl_minutes": 100000
    })";
    auto result = ParseAndValidate(raw);
    REQUIRE(std::holds_alternative<ParseError>(result));
}

TEST_CASE("kGoalSchemaJson is non-empty JSON object") {
    auto j = nlohmann::json::parse(kGoalSchemaJson);
    CHECK(j.is_object());
    CHECK(j.contains("type"));
}
```

- [ ] **Step 2: Add to CMakeLists**

Edit `tests/llmagent/CMakeLists.txt`. Update `add_executable`:

```cmake
add_executable(llmagent_unit_tests
  doctest_main.cpp
  test_config_load.cpp
  test_goal_parser.cpp
  ${LLMAGENT_DIR}/LlmAgentConfig.cpp
  ${LLMAGENT_DIR}/Schemas/Goal.cpp
)
```

- [ ] **Step 3: Run test — expect FAIL (header not found)**

```bash
cmake --build build/llmagent_tests 2>&1 | tail -5
```

Expected: build error about `Schemas/Goal.h`.

- [ ] **Step 4: Write `src/Bot/LlmAgent/Schemas/Goal.h`**

```cpp
#ifndef _PLAYERBOT_LLMAGENT_GOAL_SCHEMA_H
#define _PLAYERBOT_LLMAGENT_GOAL_SCHEMA_H

#include "Vendor/nlohmann_json.hpp"
#include <cstdint>
#include <string>
#include <variant>

enum class GoalKind {
    Idle, GoGrind, GoCamp, WanderNpc, WanderRandom,
    DoQuest, TravelFlight, Rest, OutdoorPvp
};

struct IdleParams {};
struct GoGrindParams      { double x{}; double y{}; double z{}; int32_t map_id{}; };
struct GoCampParams       { double x{}; double y{}; double z{}; int32_t map_id{}; };
struct WanderNpcParams    { uint64_t npc_guid{}; };
struct WanderRandomParams {};
struct DoQuestParams      { uint32_t quest_id{}; int32_t starting_objective_idx{}; };
struct TravelFlightParams { uint64_t from_flightmaster_guid{}; uint32_t destination_node_id{}; };
struct RestParams         {};
struct OutdoorPvpParams   { uint32_t capture_point_spawn_id{}; };

using GoalParams = std::variant<
    IdleParams, GoGrindParams, GoCampParams, WanderNpcParams, WanderRandomParams,
    DoQuestParams, TravelFlightParams, RestParams, OutdoorPvpParams
>;

struct ParsedGoal {
    GoalKind    goal;
    GoalParams  params;
    std::string reasoning;
    uint32_t    ttl_minutes;
};

struct ParseError {
    std::string message;
};

extern const char* const kGoalSchemaJson;

std::variant<ParsedGoal, ParseError> ParseAndValidate(const std::string& raw_json);

#endif  // _PLAYERBOT_LLMAGENT_GOAL_SCHEMA_H
```

- [ ] **Step 5: Write `src/Bot/LlmAgent/Schemas/Goal.cpp`**

```cpp
#include "Schemas/Goal.h"

namespace {

constexpr uint32_t kMinTtlMinutes = 1;
constexpr uint32_t kMaxTtlMinutes = 1440;  // 24 hours

GoalKind kind_from_string(const std::string& s, bool& ok) {
    ok = true;
    if (s == "idle")          return GoalKind::Idle;
    if (s == "go_grind")      return GoalKind::GoGrind;
    if (s == "go_camp")       return GoalKind::GoCamp;
    if (s == "wander_npc")    return GoalKind::WanderNpc;
    if (s == "wander_random") return GoalKind::WanderRandom;
    if (s == "do_quest")      return GoalKind::DoQuest;
    if (s == "travel_flight") return GoalKind::TravelFlight;
    if (s == "rest")          return GoalKind::Rest;
    if (s == "outdoor_pvp")   return GoalKind::OutdoorPvp;
    ok = false;
    return GoalKind::Idle;
}

template <typename T>
bool try_get(const nlohmann::json& j, const char* key, T& out) {
    if (!j.contains(key)) return false;
    try { out = j.at(key).get<T>(); } catch (...) { return false; }
    return true;
}

bool parse_params(GoalKind k, const nlohmann::json& p, GoalParams& out, std::string& err) {
    switch (k) {
        case GoalKind::Idle:         out = IdleParams{};         return true;
        case GoalKind::WanderRandom: out = WanderRandomParams{}; return true;
        case GoalKind::Rest:         out = RestParams{};         return true;
        case GoalKind::GoGrind: {
            GoGrindParams gp;
            if (!try_get(p, "x", gp.x) || !try_get(p, "y", gp.y) ||
                !try_get(p, "z", gp.z) || !try_get(p, "map_id", gp.map_id)) {
                err = "go_grind params require x, y, z, map_id"; return false;
            }
            out = gp; return true;
        }
        case GoalKind::GoCamp: {
            GoCampParams gp;
            if (!try_get(p, "x", gp.x) || !try_get(p, "y", gp.y) ||
                !try_get(p, "z", gp.z) || !try_get(p, "map_id", gp.map_id)) {
                err = "go_camp params require x, y, z, map_id"; return false;
            }
            out = gp; return true;
        }
        case GoalKind::WanderNpc: {
            WanderNpcParams gp;
            if (!try_get(p, "npc_guid", gp.npc_guid)) {
                err = "wander_npc params require npc_guid"; return false;
            }
            out = gp; return true;
        }
        case GoalKind::DoQuest: {
            DoQuestParams gp;
            if (!try_get(p, "quest_id", gp.quest_id)) {
                err = "do_quest params require quest_id"; return false;
            }
            try_get(p, "starting_objective_idx", gp.starting_objective_idx);
            out = gp; return true;
        }
        case GoalKind::TravelFlight: {
            TravelFlightParams gp;
            if (!try_get(p, "from_flightmaster_guid", gp.from_flightmaster_guid) ||
                !try_get(p, "destination_node_id", gp.destination_node_id)) {
                err = "travel_flight params require from_flightmaster_guid and destination_node_id";
                return false;
            }
            out = gp; return true;
        }
        case GoalKind::OutdoorPvp: {
            OutdoorPvpParams gp;
            if (!try_get(p, "capture_point_spawn_id", gp.capture_point_spawn_id)) {
                err = "outdoor_pvp params require capture_point_spawn_id"; return false;
            }
            out = gp; return true;
        }
    }
    err = "unhandled goal kind"; return false;
}

}  // namespace

const char* const kGoalSchemaJson = R"({
  "type": "object",
  "required": ["goal", "params", "reasoning", "ttl_minutes"],
  "additionalProperties": false,
  "properties": {
    "goal": {
      "type": "string",
      "enum": ["idle","go_grind","go_camp","wander_npc","wander_random","do_quest","travel_flight","rest","outdoor_pvp"]
    },
    "params": {"type": "object"},
    "reasoning": {"type": "string", "maxLength": 500},
    "ttl_minutes": {"type": "integer", "minimum": 1, "maximum": 1440}
  }
})";

std::variant<ParsedGoal, ParseError> ParseAndValidate(const std::string& raw_json) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(raw_json);
    } catch (const std::exception& e) {
        return ParseError{std::string{"json parse error: "} + e.what()};
    }
    if (!j.is_object()) return ParseError{"top-level value is not an object"};

    std::string goal_str;
    if (!try_get(j, "goal", goal_str)) return ParseError{"missing required field: goal"};

    bool kind_ok = false;
    GoalKind kind = kind_from_string(goal_str, kind_ok);
    if (!kind_ok) return ParseError{"unknown goal enum: " + goal_str};

    if (!j.contains("params") || !j.at("params").is_object())
        return ParseError{"missing or non-object params"};
    if (!j.contains("reasoning")) return ParseError{"missing required field: reasoning"};
    if (!j.contains("ttl_minutes")) return ParseError{"missing required field: ttl_minutes"};

    uint32_t ttl{};
    if (!try_get(j, "ttl_minutes", ttl))
        return ParseError{"ttl_minutes is not an integer"};
    if (ttl < kMinTtlMinutes || ttl > kMaxTtlMinutes)
        return ParseError{"ttl_minutes out of range [1, 1440]"};

    std::string reasoning;
    try_get(j, "reasoning", reasoning);
    if (reasoning.size() > 500) return ParseError{"reasoning too long"};

    GoalParams params;
    std::string err;
    if (!parse_params(kind, j.at("params"), params, err))
        return ParseError{err};

    return ParsedGoal{kind, std::move(params), std::move(reasoning), ttl};
}
```

- [ ] **Step 6: Build and run**

```bash
cmake --build build/llmagent_tests
./build/llmagent_tests/llmagent_unit_tests
```

Expected: all tests pass; `[doctest] Status: SUCCESS!` with 13 cases total.

- [ ] **Step 7: Commit**

```bash
git add src/Bot/LlmAgent/Schemas/ tests/llmagent/test_goal_parser.cpp tests/llmagent/CMakeLists.txt
git commit -m "feat(llm-agent): Goal schema + ParseAndValidate

kGoalSchemaJson is the response_format payload for llama-server.
ParseAndValidate returns std::variant<ParsedGoal, ParseError>.
10 doctest cases cover all nine goal variants, missing-field errors,
malformed JSON, and ttl_minutes range."
```

---

## Task 5: LlmHttpClient (cpp-httplib wrapper)

**Files:**
- Create: `src/Bot/LlmAgent/Client/LlmHttpClient.h`
- Create: `src/Bot/LlmAgent/Client/LlmHttpClient.cpp`
- Create: `tests/llmagent/test_http_client.cpp`
- Modify: `tests/llmagent/CMakeLists.txt`

- [ ] **Step 1: Write failing test `tests/llmagent/test_http_client.cpp`**

```cpp
#include "doctest.h"
#include "Client/LlmHttpClient.h"
#include "Vendor/httplib.h"
#include <thread>
#include <atomic>
#include <chrono>

namespace {

struct StubServer {
    httplib::Server svr;
    std::thread th;
    int port = 0;

    StubServer() {
        port = svr.bind_to_any_port("127.0.0.1");
        REQUIRE(port > 0);
        th = std::thread([this]{ svr.listen_after_bind(); });
        // tiny wait for the server thread to be ready
        for (int i = 0; i < 50 && !svr.is_running(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ~StubServer() {
        svr.stop();
        if (th.joinable()) th.join();
    }
    std::string base_url() const { return "http://127.0.0.1:" + std::to_string(port); }
};

}  // namespace

TEST_CASE("LlmHttpClient returns 200 body unchanged") {
    StubServer srv;
    srv.svr.Post("/v1/chat/completions", [](const httplib::Request& req, httplib::Response& res) {
        res.set_content(R"({"choices":[{"message":{"content":"ok"}}]})", "application/json");
    });

    LlmHttpClient client(srv.base_url());
    auto result = client.PostChatCompletion(R"({"x":1})", std::chrono::milliseconds(2000));
    REQUIRE(result.has_value());
    CHECK(result->status == 200);
    CHECK(result->body.find("choices") != std::string::npos);
    CHECK(result->error.empty());
}

TEST_CASE("LlmHttpClient surfaces HTTP non-2xx") {
    StubServer srv;
    srv.svr.Post("/v1/chat/completions", [](const httplib::Request& req, httplib::Response& res) {
        res.status = 500;
        res.set_content("boom", "text/plain");
    });

    LlmHttpClient client(srv.base_url());
    auto result = client.PostChatCompletion(R"({"x":1})", std::chrono::milliseconds(2000));
    REQUIRE(result.has_value());
    CHECK(result->status == 500);
    CHECK(result->body == "boom");
}

TEST_CASE("LlmHttpClient returns nullopt on connect error") {
    // Port 1 is reserved / unbound; expect connection refused.
    LlmHttpClient client("http://127.0.0.1:1");
    auto result = client.PostChatCompletion(R"({"x":1})", std::chrono::milliseconds(500));
    CHECK(!result.has_value());
}

TEST_CASE("LlmHttpClient honors read timeout") {
    StubServer srv;
    srv.svr.Post("/v1/chat/completions", [](const httplib::Request& req, httplib::Response& res) {
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        res.set_content("late", "text/plain");
    });

    LlmHttpClient client(srv.base_url());
    auto t0 = std::chrono::steady_clock::now();
    auto result = client.PostChatCompletion(R"({"x":1})", std::chrono::milliseconds(200));
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - t0).count();
    CHECK(!result.has_value());
    CHECK(elapsed < 600);  // timeout took effect well before the 800ms response
}

TEST_CASE("LlmHttpClient posts body unchanged") {
    StubServer srv;
    std::atomic<bool> got_body{false};
    std::string received;
    srv.svr.Post("/v1/chat/completions", [&](const httplib::Request& req, httplib::Response& res) {
        received = req.body;
        got_body.store(true);
        res.set_content(R"({"ok":true})", "application/json");
    });

    LlmHttpClient client(srv.base_url());
    const std::string body = R"({"model":"qwen","messages":[]})";
    auto result = client.PostChatCompletion(body, std::chrono::milliseconds(2000));
    REQUIRE(result.has_value());
    CHECK(got_body.load());
    CHECK(received == body);
}
```

- [ ] **Step 2: Add to CMakeLists**

Edit `tests/llmagent/CMakeLists.txt`. Update `add_executable`:

```cmake
add_executable(llmagent_unit_tests
  doctest_main.cpp
  test_config_load.cpp
  test_goal_parser.cpp
  test_http_client.cpp
  ${LLMAGENT_DIR}/LlmAgentConfig.cpp
  ${LLMAGENT_DIR}/Schemas/Goal.cpp
  ${LLMAGENT_DIR}/Client/LlmHttpClient.cpp
)
```

- [ ] **Step 3: Run test — expect FAIL (header not found)**

```bash
cmake --build build/llmagent_tests 2>&1 | tail -5
```

Expected: build error about `Client/LlmHttpClient.h`.

- [ ] **Step 4: Write `src/Bot/LlmAgent/Client/LlmHttpClient.h`**

```cpp
#ifndef _PLAYERBOT_LLMAGENT_HTTP_CLIENT_H
#define _PLAYERBOT_LLMAGENT_HTTP_CLIENT_H

#include <chrono>
#include <optional>
#include <string>

struct RawResponse {
    int         status = 0;
    std::string body;
    std::string error;  // populated when status != 200
};

class LlmHttpClient {
  public:
    explicit LlmHttpClient(std::string endpoint);  // e.g. "http://127.0.0.1:8080"

    std::optional<RawResponse> PostChatCompletion(
        const std::string& body_json,
        std::chrono::milliseconds timeout);

  private:
    std::string endpoint_;  // host[:port], no path, no scheme stripped at construction
};

#endif  // _PLAYERBOT_LLMAGENT_HTTP_CLIENT_H
```

- [ ] **Step 5: Write `src/Bot/LlmAgent/Client/LlmHttpClient.cpp`**

```cpp
#include "Client/LlmHttpClient.h"
#include "Vendor/httplib.h"

#include <stdexcept>

namespace {

struct ParsedEndpoint {
    std::string host;
    int port = 80;
};

ParsedEndpoint parse_endpoint(const std::string& url) {
    // Accepts "http://host[:port]"; strips scheme; defaults port to 80.
    std::string s = url;
    constexpr const char* kPrefix = "http://";
    if (s.rfind(kPrefix, 0) == 0) s = s.substr(std::char_traits<char>::length(kPrefix));

    ParsedEndpoint out;
    auto colon = s.find(':');
    if (colon == std::string::npos) {
        out.host = s;
    } else {
        out.host = s.substr(0, colon);
        try { out.port = std::stoi(s.substr(colon + 1)); } catch (...) { out.port = 80; }
    }
    return out;
}

}  // namespace

LlmHttpClient::LlmHttpClient(std::string endpoint) : endpoint_(std::move(endpoint)) {}

std::optional<RawResponse> LlmHttpClient::PostChatCompletion(
    const std::string& body_json,
    std::chrono::milliseconds timeout)
{
    ParsedEndpoint ep = parse_endpoint(endpoint_);
    httplib::Client cli(ep.host, ep.port);
    cli.set_connection_timeout(timeout);
    cli.set_read_timeout(timeout);
    cli.set_write_timeout(timeout);

    auto res = cli.Post("/v1/chat/completions", body_json, "application/json");
    if (!res) return std::nullopt;  // transport or timeout

    RawResponse out;
    out.status = res->status;
    out.body = res->body;
    if (res->status < 200 || res->status >= 300) out.error = res->reason;
    return out;
}
```

- [ ] **Step 6: Build and run**

```bash
cmake --build build/llmagent_tests
./build/llmagent_tests/llmagent_unit_tests
```

Expected: all tests pass. Roughly 18 total.

- [ ] **Step 7: Commit**

```bash
git add src/Bot/LlmAgent/Client/ tests/llmagent/test_http_client.cpp tests/llmagent/CMakeLists.txt
git commit -m "feat(llm-agent): LlmHttpClient on cpp-httplib

PostChatCompletion(body, timeout) returns optional<RawResponse>.
Returns nullopt on transport/timeout; populates RawResponse on
all HTTP responses including non-2xx. Tested against an in-process
httplib::Server fixture covering 200, 500, connect refused, and
read-timeout paths."
```

---

## Task 6: BotState + BuildDigestJson (pure)

**Files:**
- Create: `src/Bot/LlmAgent/Tiers/Tier0_StateDigest.h`
- Create: `src/Bot/LlmAgent/Tiers/Tier0_StateDigest.cpp`
- Create: `tests/llmagent/test_digest_shape.cpp`
- Modify: `tests/llmagent/CMakeLists.txt`

This task delivers the pure JSON-building half. The worldserver-side `SnapshotBot` that fills the `BotState` POD is Task 9.

- [ ] **Step 1: Write failing test `tests/llmagent/test_digest_shape.cpp`**

```cpp
#include "doctest.h"
#include "Tiers/Tier0_StateDigest.h"

namespace {

BotState make_grimblade() {
    BotState s;
    s.self.name = "Grimblade";
    s.self.race = "orc";
    s.self.character_class = "warrior";
    s.self.spec = "arms";
    s.self.level = 37;
    s.self.hp_pct = 84;
    s.self.gold_copper = 32841;
    s.self.is_in_combat = false;
    s.self.is_resting = true;
    s.self.is_dead = false;
    s.location.map = "Eastern Kingdoms";
    s.location.zone = "Hillsbrad Foothills";
    s.location.subzone = "Tarren Mill";
    s.location.position = {123.4, -56.7, 12.1};
    s.location.near_npcs = {"Innkeeper Anchorite Truuen"};
    s.goal.current = "DoQuest";
    s.goal.progress_pct = 40;
    s.goal.elapsed_minutes = 8;
    s.goal.ttl_minutes = 22;
    s.goal.params_json = R"({"quest_id":502,"objective_idx":1})";
    s.quest_log.push_back({502, "Syndicate Assassins", "12/20"});
    s.quest_log.push_back({488, "The Killing Fields", "complete, turn in"});
    s.inventory.bag_used = "22/24";
    s.inventory.junk_value_copper = 4200;
    s.inventory.consumables = {"8x healing potion (lvl 35)"};
    s.inventory.gear_vs_level_score = 0.78;
    s.social.in_group = false;
    s.social.nearby_humans.push_back({"RealPlayerBob", 38, 18.2});
    s.social.recent_whispers.push_back({"RealPlayerBob", "wanna group?", 3});
    s.event_log = {"Killed Syndicate Footpad (+1 progress)", "RealPlayerBob whispered: wanna group?"};
    return s;
}

}  // namespace

TEST_CASE("BuildDigestJson has all top-level §6 fields") {
    auto j = BuildDigestJson(make_grimblade());
    CHECK(j.contains("self"));
    CHECK(j.contains("location"));
    CHECK(j.contains("goal"));
    CHECK(j.contains("quest_log"));
    CHECK(j.contains("inventory_highlights"));
    CHECK(j.contains("social"));
    CHECK(j.contains("event_log"));
    CHECK(j.contains("memory_hints"));
}

TEST_CASE("BuildDigestJson memory_hints is empty array in Phase 1") {
    auto j = BuildDigestJson(make_grimblade());
    CHECK(j["memory_hints"].is_array());
    CHECK(j["memory_hints"].empty());
}

TEST_CASE("BuildDigestJson self block has expected types") {
    auto j = BuildDigestJson(make_grimblade());
    CHECK(j["self"]["name"].get<std::string>() == "Grimblade");
    CHECK(j["self"]["level"].get<int>() == 37);
    CHECK(j["self"]["hp_pct"].get<int>() == 84);
    CHECK(j["self"]["is_resting"].get<bool>() == true);
}

TEST_CASE("BuildDigestJson location block includes position array") {
    auto j = BuildDigestJson(make_grimblade());
    REQUIRE(j["location"]["position"].is_array());
    CHECK(j["location"]["position"].size() == 3);
    CHECK(j["location"]["zone"].get<std::string>() == "Hillsbrad Foothills");
}

TEST_CASE("BuildDigestJson quest_log preserves order and shape") {
    auto j = BuildDigestJson(make_grimblade());
    REQUIRE(j["quest_log"].is_array());
    REQUIRE(j["quest_log"].size() == 2);
    CHECK(j["quest_log"][0]["id"].get<int>() == 502);
    CHECK(j["quest_log"][1]["title"].get<std::string>() == "The Killing Fields");
}

TEST_CASE("BuildDigestJson social.recent_whispers preserved") {
    auto j = BuildDigestJson(make_grimblade());
    REQUIRE(j["social"]["recent_whispers"].is_array());
    REQUIRE(j["social"]["recent_whispers"].size() == 1);
    CHECK(j["social"]["recent_whispers"][0]["from"].get<std::string>() == "RealPlayerBob");
}

TEST_CASE("BuildDigestJson event_log is array of strings") {
    auto j = BuildDigestJson(make_grimblade());
    REQUIRE(j["event_log"].is_array());
    CHECK(j["event_log"].size() == 2);
}

TEST_CASE("BuildDigestJson handles empty BotState without crashing") {
    BotState empty;
    auto j = BuildDigestJson(empty);
    CHECK(j["quest_log"].is_array());
    CHECK(j["event_log"].is_array());
    CHECK(j["social"]["nearby_humans"].is_array());
}
```

- [ ] **Step 2: Add to CMakeLists**

Edit `tests/llmagent/CMakeLists.txt`. Update `add_executable`:

```cmake
add_executable(llmagent_unit_tests
  doctest_main.cpp
  test_config_load.cpp
  test_goal_parser.cpp
  test_http_client.cpp
  test_digest_shape.cpp
  ${LLMAGENT_DIR}/LlmAgentConfig.cpp
  ${LLMAGENT_DIR}/Schemas/Goal.cpp
  ${LLMAGENT_DIR}/Client/LlmHttpClient.cpp
  ${LLMAGENT_DIR}/Tiers/Tier0_StateDigest.cpp
)
```

- [ ] **Step 3: Run test — expect FAIL (header not found)**

```bash
cmake --build build/llmagent_tests 2>&1 | tail -5
```

Expected: build error about `Tiers/Tier0_StateDigest.h`.

- [ ] **Step 4: Write `src/Bot/LlmAgent/Tiers/Tier0_StateDigest.h`**

```cpp
#ifndef _PLAYERBOT_LLMAGENT_TIER0_DIGEST_H
#define _PLAYERBOT_LLMAGENT_TIER0_DIGEST_H

#include "Vendor/nlohmann_json.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

struct BotSelf {
    std::string name;
    std::string race;
    std::string character_class;  // "class" is a C++ keyword
    std::string spec;
    int32_t  level = 0;
    int32_t  hp_pct = 100;
    int32_t  mana_pct = -1;       // -1 = nullable (mapped to JSON null)
    int64_t  gold_copper = 0;
    bool     is_in_combat = false;
    bool     is_resting = false;
    bool     is_dead = false;
};

struct BotLocation {
    std::string map;
    std::string zone;
    std::string subzone;
    std::array<double, 3> position {0.0, 0.0, 0.0};
    std::vector<std::string> near_npcs;
};

struct BotGoal {
    std::string current;          // e.g. "Idle", "DoQuest"
    std::string params_json;      // verbatim JSON string of params
    int32_t  progress_pct = 0;
    int32_t  elapsed_minutes = 0;
    int32_t  ttl_minutes = 0;
};

struct QuestLogEntry {
    uint32_t    id = 0;
    std::string title;
    std::string progress;
};

struct InventoryHighlights {
    std::string bag_used;
    int64_t  junk_value_copper = 0;
    std::vector<std::string> consumables;
    double   gear_vs_level_score = 0.0;
};

struct NearbyHuman {
    std::string name;
    int32_t  level = 0;
    double   distance = 0.0;
};

struct RecentWhisper {
    std::string from;
    std::string text;
    int32_t  age_s = 0;
};

struct BotSocial {
    bool in_group = false;
    std::vector<std::string> group_members;
    std::string guild;            // empty = JSON null
    std::vector<NearbyHuman>  nearby_humans;
    std::vector<RecentWhisper> recent_whispers;
};

struct BotState {
    BotSelf                     self;
    BotLocation                 location;
    BotGoal                     goal;
    std::vector<QuestLogEntry>  quest_log;
    InventoryHighlights         inventory;
    BotSocial                   social;
    std::vector<std::string>    event_log;
};

nlohmann::json BuildDigestJson(const BotState& s);

#endif  // _PLAYERBOT_LLMAGENT_TIER0_DIGEST_H
```

- [ ] **Step 5: Write `src/Bot/LlmAgent/Tiers/Tier0_StateDigest.cpp`**

```cpp
#include "Tiers/Tier0_StateDigest.h"

nlohmann::json BuildDigestJson(const BotState& s) {
    nlohmann::json j;

    j["self"] = {
        {"name",          s.self.name},
        {"race",          s.self.race},
        {"class",         s.self.character_class},
        {"spec",          s.self.spec},
        {"level",         s.self.level},
        {"hp_pct",        s.self.hp_pct},
        {"mana_pct",      s.self.mana_pct < 0 ? nlohmann::json(nullptr) : nlohmann::json(s.self.mana_pct)},
        {"gold_copper",   s.self.gold_copper},
        {"is_in_combat",  s.self.is_in_combat},
        {"is_resting",    s.self.is_resting},
        {"is_dead",       s.self.is_dead},
    };

    j["location"] = {
        {"map",       s.location.map},
        {"zone",      s.location.zone},
        {"subzone",   s.location.subzone},
        {"position",  s.location.position},
        {"near_npcs", s.location.near_npcs},
    };

    // goal.params is a verbatim JSON string; parse so the digest doesn't
    // contain a string that itself contains JSON.
    nlohmann::json goal_params = nlohmann::json::object();
    if (!s.goal.params_json.empty()) {
        try { goal_params = nlohmann::json::parse(s.goal.params_json); }
        catch (...) { goal_params = nlohmann::json::object(); }
    }
    j["goal"] = {
        {"current",         s.goal.current},
        {"params",          goal_params},
        {"progress_pct",    s.goal.progress_pct},
        {"elapsed_minutes", s.goal.elapsed_minutes},
        {"ttl_minutes",     s.goal.ttl_minutes},
    };

    j["quest_log"] = nlohmann::json::array();
    for (const auto& q : s.quest_log) {
        j["quest_log"].push_back({{"id", q.id}, {"title", q.title}, {"progress", q.progress}});
    }

    j["inventory_highlights"] = {
        {"bag_used",            s.inventory.bag_used},
        {"junk_value_copper",   s.inventory.junk_value_copper},
        {"consumables",         s.inventory.consumables},
        {"gear_vs_level_score", s.inventory.gear_vs_level_score},
    };

    nlohmann::json humans = nlohmann::json::array();
    for (const auto& h : s.social.nearby_humans) {
        humans.push_back({{"name", h.name}, {"level", h.level}, {"distance", h.distance}});
    }
    nlohmann::json whispers = nlohmann::json::array();
    for (const auto& w : s.social.recent_whispers) {
        whispers.push_back({{"from", w.from}, {"text", w.text}, {"age_s", w.age_s}});
    }
    j["social"] = {
        {"in_group",         s.social.in_group},
        {"group_members",    s.social.group_members},
        {"guild",            s.social.guild.empty() ? nlohmann::json(nullptr) : nlohmann::json(s.social.guild)},
        {"nearby_humans",    humans},
        {"recent_whispers",  whispers},
    };

    j["event_log"] = s.event_log;
    j["memory_hints"] = nlohmann::json::array();  // Phase 1: always empty

    return j;
}
```

- [ ] **Step 6: Build and run**

```bash
cmake --build build/llmagent_tests
./build/llmagent_tests/llmagent_unit_tests
```

Expected: all tests pass.

- [ ] **Step 7: Commit**

```bash
git add src/Bot/LlmAgent/Tiers/ tests/llmagent/test_digest_shape.cpp tests/llmagent/CMakeLists.txt
git commit -m "feat(llm-agent): Tier0 BotState POD + pure BuildDigestJson

BotState is the cross-thread POD that gets snapshotted on the
worldserver thread and consumed on a worker. BuildDigestJson is a
pure function over the POD — easy to unit-test. 8 doctest cases
cover all §6 blocks plus a zero-init smoke case.

The Player*-touching SnapshotBot lands in a later task once the
plumbing around it exists."
```

---

## Task 7: LlmAgentManager — request/response/worker plumbing

**Files:**
- Create: `src/Bot/LlmAgent/LlmAgentManager.h`
- Create: `src/Bot/LlmAgent/LlmAgentManager.cpp`
- Create: `tests/llmagent/test_manager_threading.cpp`
- Modify: `tests/llmagent/CMakeLists.txt`

This is the most complex task. It owns the thread pool, queues, JSONL writer, and the integration test that validates the spike's stated goal.

- [ ] **Step 1: Write failing test `tests/llmagent/test_manager_threading.cpp`**

```cpp
#include "doctest.h"
#include "LlmAgentManager.h"
#include "Vendor/httplib.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

namespace {

struct StubServer {
    httplib::Server svr;
    std::thread th;
    int port = 0;
    std::atomic<int> hit_count{0};
    std::chrono::milliseconds sleep{0};

    StubServer() {
        port = svr.bind_to_any_port("127.0.0.1");
        REQUIRE(port > 0);
        svr.Post("/v1/chat/completions", [this](const httplib::Request&, httplib::Response& res) {
            hit_count.fetch_add(1);
            if (sleep.count() > 0) std::this_thread::sleep_for(sleep);
            res.set_content(R"({"choices":[{"message":{"content":"{\"goal\":\"rest\",\"params\":{},\"reasoning\":\"x\",\"ttl_minutes\":5}"}}]})", "application/json");
        });
        th = std::thread([this]{ svr.listen_after_bind(); });
        for (int i = 0; i < 50 && !svr.is_running(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ~StubServer() { svr.stop(); if (th.joinable()) th.join(); }
    std::string base_url() const { return "http://127.0.0.1:" + std::to_string(port); }
};

LlmAgentConfig test_cfg(const std::string& url, const std::string& jsonl_path) {
    LlmAgentConfig c;
    c.Enabled = true;
    c.Endpoint = url;
    c.WorkerThreads = 4;
    c.RequestTimeoutMs = 2000;
    c.JsonlPath = jsonl_path;
    c.SystemPrompt = "test";
    return c;
}

}  // namespace

TEST_CASE("LlmAgentManager processes 100 requests with 4 workers") {
    StubServer srv;
    auto tmpdir = std::filesystem::temp_directory_path() / "llmagent_test_t1";
    std::filesystem::create_directories(tmpdir);
    auto jsonl = (tmpdir / "out.jsonl").string();
    if (std::filesystem::exists(jsonl)) std::filesystem::remove(jsonl);

    LlmAgentManager mgr;
    mgr.Start(test_cfg(srv.base_url(), jsonl));

    for (int i = 0; i < 100; ++i) {
        LlmRequest req;
        req.bot_guid = static_cast<uint64_t>(i + 1);
        req.bot_name = "bot" + std::to_string(i);
        req.body_json = R"({"model":"test","messages":[]})";
        req.digest_json = nlohmann::json::object();
        mgr.Enqueue(std::move(req));
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (srv.hit_count.load() < 100 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

    mgr.Shutdown();
    CHECK(srv.hit_count.load() == 100);
}

TEST_CASE("LlmAgentManager in-flight cap rejects second enqueue for same bot") {
    StubServer srv;
    srv.sleep = std::chrono::milliseconds(300);  // slow enough to overlap
    auto tmpdir = std::filesystem::temp_directory_path() / "llmagent_test_t2";
    std::filesystem::create_directories(tmpdir);
    auto jsonl = (tmpdir / "out.jsonl").string();

    LlmAgentManager mgr;
    mgr.Start(test_cfg(srv.base_url(), jsonl));

    LlmRequest req1;
    req1.bot_guid = 42;
    req1.bot_name = "Grimblade";
    req1.body_json = "{}";
    req1.digest_json = nlohmann::json::object();
    CHECK(mgr.Enqueue(req1) == true);

    LlmRequest req2 = req1;
    CHECK(mgr.Enqueue(std::move(req2)) == false);  // already in-flight

    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    CHECK(srv.hit_count.load() == 1);

    mgr.Shutdown();
}

TEST_CASE("LlmAgentManager DrainResults is per-bot and clears stack") {
    StubServer srv;
    auto tmpdir = std::filesystem::temp_directory_path() / "llmagent_test_t3";
    std::filesystem::create_directories(tmpdir);
    auto jsonl = (tmpdir / "out.jsonl").string();

    LlmAgentManager mgr;
    mgr.Start(test_cfg(srv.base_url(), jsonl));

    for (int i = 0; i < 5; ++i) {
        LlmRequest req;
        req.bot_guid = 100 + i;
        req.bot_name = "bot" + std::to_string(i);
        req.body_json = "{}";
        req.digest_json = nlohmann::json::object();
        mgr.Enqueue(std::move(req));
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (srv.hit_count.load() < 5 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

    for (int i = 0; i < 5; ++i) {
        auto results = mgr.DrainResults(100 + i);
        CHECK(results.size() == 1);
        CHECK(results[0].bot_guid == static_cast<uint64_t>(100 + i));
    }
    auto empty = mgr.DrainResults(999);
    CHECK(empty.empty());

    mgr.Shutdown();
}

TEST_CASE("LlmAgentManager writes one JSONL line per response") {
    StubServer srv;
    auto tmpdir = std::filesystem::temp_directory_path() / "llmagent_test_t4";
    std::filesystem::create_directories(tmpdir);
    auto jsonl = (tmpdir / "out.jsonl").string();
    if (std::filesystem::exists(jsonl)) std::filesystem::remove(jsonl);

    LlmAgentManager mgr;
    mgr.Start(test_cfg(srv.base_url(), jsonl));

    for (int i = 0; i < 10; ++i) {
        LlmRequest req;
        req.bot_guid = 200 + i;
        req.bot_name = "n";
        req.body_json = "{}";
        req.digest_json = nlohmann::json::object();
        mgr.Enqueue(std::move(req));
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (srv.hit_count.load() < 10 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

    mgr.Shutdown();

    std::ifstream f(jsonl);
    int lines = 0;
    std::string line;
    while (std::getline(f, line)) {
        ++lines;
        auto j = nlohmann::json::parse(line);
        CHECK(j.contains("bot_guid"));
        CHECK(j.contains("inference_ms"));
        CHECK(j.contains("parsed_status"));
    }
    CHECK(lines == 10);
}

TEST_CASE("LlmAgentManager records transport_error on bad endpoint") {
    auto tmpdir = std::filesystem::temp_directory_path() / "llmagent_test_t5";
    std::filesystem::create_directories(tmpdir);
    auto jsonl = (tmpdir / "out.jsonl").string();
    if (std::filesystem::exists(jsonl)) std::filesystem::remove(jsonl);

    LlmAgentManager mgr;
    mgr.Start(test_cfg("http://127.0.0.1:1", jsonl));  // unreachable

    LlmRequest req;
    req.bot_guid = 1;
    req.bot_name = "n";
    req.body_json = "{}";
    req.digest_json = nlohmann::json::object();
    mgr.Enqueue(std::move(req));

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    auto results = mgr.DrainResults(1);
    REQUIRE(results.size() == 1);
    CHECK(results[0].parsed_status == "transport_error");

    mgr.Shutdown();
}

TEST_CASE("LlmAgentManager Shutdown is idempotent") {
    LlmAgentManager mgr;
    LlmAgentConfig cfg;
    cfg.Enabled = false;
    mgr.Start(cfg);
    mgr.Shutdown();
    mgr.Shutdown();  // second call must be a no-op
    CHECK(true);     // reaching here proves no crash
}
```

- [ ] **Step 2: Add to CMakeLists**

Edit `tests/llmagent/CMakeLists.txt`. Update `add_executable`:

```cmake
add_executable(llmagent_unit_tests
  doctest_main.cpp
  test_config_load.cpp
  test_goal_parser.cpp
  test_http_client.cpp
  test_digest_shape.cpp
  test_manager_threading.cpp
  ${LLMAGENT_DIR}/LlmAgentConfig.cpp
  ${LLMAGENT_DIR}/Schemas/Goal.cpp
  ${LLMAGENT_DIR}/Client/LlmHttpClient.cpp
  ${LLMAGENT_DIR}/Tiers/Tier0_StateDigest.cpp
  ${LLMAGENT_DIR}/LlmAgentManager.cpp
)
```

- [ ] **Step 3: Run test — expect FAIL**

```bash
cmake --build build/llmagent_tests 2>&1 | tail -5
```

Expected: build error about `LlmAgentManager.h`.

- [ ] **Step 4: Write `src/Bot/LlmAgent/LlmAgentManager.h`**

```cpp
#ifndef _PLAYERBOT_LLMAGENT_MANAGER_H
#define _PLAYERBOT_LLMAGENT_MANAGER_H

#include "Vendor/nlohmann_json.hpp"
#include "LlmAgentConfig.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
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
```

- [ ] **Step 5: Write `src/Bot/LlmAgent/LlmAgentManager.cpp`**

```cpp
#include "LlmAgentManager.h"
#include "Client/LlmHttpClient.h"
#include "Schemas/Goal.h"

#include <chrono>
#include <filesystem>
#include <sstream>
#include <utility>

namespace {

uint64_t ms_since(std::chrono::steady_clock::time_point t0) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count());
}

uint64_t epoch_ms() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string truncate(const std::string& s, size_t max) {
    return s.size() <= max ? s : s.substr(0, max) + "...[truncated]";
}

std::string goal_kind_to_string(GoalKind k) {
    switch (k) {
        case GoalKind::Idle:         return "idle";
        case GoalKind::GoGrind:      return "go_grind";
        case GoalKind::GoCamp:       return "go_camp";
        case GoalKind::WanderNpc:    return "wander_npc";
        case GoalKind::WanderRandom: return "wander_random";
        case GoalKind::DoQuest:      return "do_quest";
        case GoalKind::TravelFlight: return "travel_flight";
        case GoalKind::Rest:         return "rest";
        case GoalKind::OutdoorPvp:   return "outdoor_pvp";
    }
    return "unknown";
}

nlohmann::json parsed_goal_to_json(const ParsedGoal& g) {
    return {
        {"goal", goal_kind_to_string(g.goal)},
        {"reasoning", g.reasoning},
        {"ttl_minutes", g.ttl_minutes},
    };
}

}  // namespace

LlmAgentManager& LlmAgentManager::Instance() {
    static LlmAgentManager instance;
    return instance;
}

void LlmAgentManager::Start(LlmAgentConfig cfg) {
    Shutdown();  // safe even if never started

    cfg_ = std::move(cfg);
    if (!cfg_.Enabled) return;

    // Open JSONL (parent dir created if missing).
    if (!cfg_.JsonlPath.empty()) {
        auto p = std::filesystem::path(cfg_.JsonlPath);
        if (p.has_parent_path()) {
            std::error_code ec;
            std::filesystem::create_directories(p.parent_path(), ec);
        }
        jsonl_.open(cfg_.JsonlPath, std::ios::app);
    }

    running_.store(true);
    workers_.reserve(cfg_.WorkerThreads);
    for (uint32_t i = 0; i < cfg_.WorkerThreads; ++i)
        workers_.emplace_back(&LlmAgentManager::WorkerLoop, this);
}

void LlmAgentManager::Shutdown() {
    if (!running_.exchange(false) && workers_.empty()) return;
    queue_cv_.notify_all();
    for (auto& t : workers_) if (t.joinable()) t.join();
    workers_.clear();
    if (jsonl_.is_open()) jsonl_.close();
}

bool LlmAgentManager::IsInFlight(uint64_t bot_guid) const {
    std::lock_guard<std::mutex> g(inflight_mu_);
    return inflight_.count(bot_guid) > 0;
}

bool LlmAgentManager::HasPendingResults(uint64_t bot_guid) const {
    std::lock_guard<std::mutex> g(results_mu_);
    auto it = results_.find(bot_guid);
    return it != results_.end() && !it->second.empty();
}

bool LlmAgentManager::Enqueue(LlmRequest request) {
    {
        std::lock_guard<std::mutex> g(inflight_mu_);
        if (inflight_.count(request.bot_guid)) return false;
        inflight_.insert(request.bot_guid);
    }
    request.ts_enqueued = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> g(queue_mu_);
        queue_.push_back(std::move(request));
    }
    queue_cv_.notify_one();
    return true;
}

std::vector<LlmResult> LlmAgentManager::DrainResults(uint64_t bot_guid) {
    std::vector<LlmResult> out;
    std::lock_guard<std::mutex> g(results_mu_);
    auto it = results_.find(bot_guid);
    if (it == results_.end()) return out;
    while (!it->second.empty()) {
        out.push_back(std::move(it->second.top()));
        it->second.pop();
    }
    results_.erase(it);
    return out;
}

void LlmAgentManager::WorkerLoop() {
    while (running_.load()) {
        LlmRequest req;
        {
            std::unique_lock<std::mutex> lk(queue_mu_);
            queue_cv_.wait(lk, [&]{ return !running_.load() || !queue_.empty(); });
            if (!running_.load() && queue_.empty()) return;
            if (queue_.empty()) continue;
            req = std::move(queue_.front());
            queue_.pop_front();
        }
        HandleRequest(std::move(req));
    }
}

void LlmAgentManager::HandleRequest(LlmRequest req) {
    auto t_dispatch = std::chrono::steady_clock::now();
    uint64_t queue_wait_ms = ms_since(req.ts_enqueued);

    LlmHttpClient client(cfg_.Endpoint);
    auto raw = client.PostChatCompletion(
        req.body_json,
        std::chrono::milliseconds(cfg_.RequestTimeoutMs));
    uint64_t inference_ms = ms_since(t_dispatch);

    LlmResult result;
    result.bot_guid = req.bot_guid;
    result.bot_name = req.bot_name;
    result.queue_wait_ms = queue_wait_ms;
    result.inference_ms = inference_ms;
    result.total_latency_ms = queue_wait_ms + inference_ms;

    if (!raw.has_value()) {
        result.parsed_status = "transport_error";
        result.validator_error = "connect/timeout";
    } else if (raw->status < 200 || raw->status >= 300) {
        result.parsed_status = "http_error";
        result.raw_response = truncate(raw->body, 4096);
        result.validator_error = std::to_string(raw->status) + " " + raw->error;
    } else {
        // Extract choices[0].message.content
        std::string content;
        try {
            auto env = nlohmann::json::parse(raw->body);
            content = env.at("choices").at(0).at("message").at("content").get<std::string>();
        } catch (const std::exception& e) {
            result.parsed_status = "schema_error";
            result.raw_response = truncate(raw->body, 4096);
            result.validator_error = std::string{"envelope: "} + e.what();
        }
        if (result.parsed_status.empty()) {
            result.raw_response = truncate(content, 4096);
            auto parsed = ParseAndValidate(content);
            if (std::holds_alternative<ParsedGoal>(parsed)) {
                result.parsed_status = "ok";
                result.parsed_goal = parsed_goal_to_json(std::get<ParsedGoal>(parsed));
            } else {
                result.parsed_status = "schema_error";
                result.validator_error = std::get<ParseError>(parsed).message;
            }
        }
    }

    // Append JSONL line.
    nlohmann::json record;
    record["ts_completed_ms_epoch"] = epoch_ms();
    record["bot_guid"] = req.bot_guid;
    record["bot_name"] = req.bot_name;
    record["queue_wait_ms"] = result.queue_wait_ms;
    record["inference_ms"] = result.inference_ms;
    record["total_latency_ms"] = result.total_latency_ms;
    record["digest"] = req.digest_json;
    record["raw_response"] = result.raw_response;
    record["parsed_status"] = result.parsed_status;
    record["parsed_goal"] = result.parsed_goal.is_null() ? nlohmann::json(nullptr) : result.parsed_goal;
    record["validator_error"] = result.validator_error.empty() ? nlohmann::json(nullptr) : nlohmann::json(result.validator_error);

    AppendJsonl(record.dump());

    // Push to result stack.
    {
        std::lock_guard<std::mutex> g(results_mu_);
        results_[req.bot_guid].push(std::move(result));
    }
    // Clear in-flight.
    {
        std::lock_guard<std::mutex> g(inflight_mu_);
        inflight_.erase(req.bot_guid);
    }
}

void LlmAgentManager::AppendJsonl(const std::string& line) {
    std::lock_guard<std::mutex> g(jsonl_mu_);
    if (!jsonl_.is_open()) return;
    jsonl_ << line << '\n';
    jsonl_.flush();
}
```

- [ ] **Step 6: Build and run**

```bash
cmake --build build/llmagent_tests
./build/llmagent_tests/llmagent_unit_tests
```

Expected: all tests pass; total ~32 cases.

- [ ] **Step 7: Commit**

```bash
git add src/Bot/LlmAgent/LlmAgentManager.h src/Bot/LlmAgent/LlmAgentManager.cpp tests/llmagent/test_manager_threading.cpp tests/llmagent/CMakeLists.txt
git commit -m "feat(llm-agent): LlmAgentManager — worker pool + queues + JSONL

Singleton via Instance(). Start(cfg) opens the JSONL file and spawns
WorkerThreads workers; Shutdown() joins them. Enqueue marks in-flight
atomically and rejects duplicates. DrainResults is per-bot and clears
the stack. Six integration tests cover 100-request throughput, the
in-flight cap, per-bot drain, JSONL line shape, transport_error path,
and Shutdown idempotency."
```

---

## Task 8: SnapshotBot — worldserver-thread digest builder

**Files:**
- Modify: `src/Bot/LlmAgent/Tiers/Tier0_StateDigest.h`
- Modify: `src/Bot/LlmAgent/Tiers/Tier0_StateDigest.cpp`

This task lives outside the unit-test target — it touches `PlayerbotAI*`/`Player*` and only compiles inside the worldserver build.

- [ ] **Step 1: Append `SnapshotBot` declaration to `Tier0_StateDigest.h`**

After the existing `BuildDigestJson` declaration, add:

```cpp
class PlayerbotAI;
BotState  SnapshotBot(PlayerbotAI* botAI);  // touches game state; worldserver-thread only
nlohmann::json BuildDigest(PlayerbotAI* botAI);  // convenience: snapshot + build
```

- [ ] **Step 2: Append `SnapshotBot` definition to `Tier0_StateDigest.cpp`**

At the bottom of the file, add:

```cpp
// ===========================================================================
// Worldserver-thread only. NOT linked into unit tests.
// ===========================================================================
#ifndef LLMAGENT_UNIT_TESTS

#include "Playerbots/PlayerbotAI.h"
#include "Player.h"
#include "Map.h"
#include "QuestDef.h"

namespace {

std::string class_name_lower(uint8 cls) {
    switch (cls) {
        case CLASS_WARRIOR: return "warrior";
        case CLASS_PALADIN: return "paladin";
        case CLASS_HUNTER:  return "hunter";
        case CLASS_ROGUE:   return "rogue";
        case CLASS_PRIEST:  return "priest";
        case CLASS_DEATH_KNIGHT: return "death_knight";
        case CLASS_SHAMAN:  return "shaman";
        case CLASS_MAGE:    return "mage";
        case CLASS_WARLOCK: return "warlock";
        case CLASS_DRUID:   return "druid";
        default: return "unknown";
    }
}

std::string race_name_lower(uint8 r) {
    switch (r) {
        case RACE_HUMAN:    return "human";
        case RACE_ORC:      return "orc";
        case RACE_DWARF:    return "dwarf";
        case RACE_NIGHTELF: return "night_elf";
        case RACE_UNDEAD_PLAYER: return "undead";
        case RACE_TAUREN:   return "tauren";
        case RACE_GNOME:    return "gnome";
        case RACE_TROLL:    return "troll";
        case RACE_BLOODELF: return "blood_elf";
        case RACE_DRAENEI:  return "draenei";
        default: return "unknown";
    }
}

}  // anon

BotState SnapshotBot(PlayerbotAI* botAI) {
    BotState s;
    if (!botAI) return s;
    Player* bot = botAI->GetBot();
    if (!bot) return s;

    s.self.name             = bot->GetName();
    s.self.race             = race_name_lower(bot->getRace());
    s.self.character_class  = class_name_lower(bot->getClass());
    s.self.level            = bot->GetLevel();
    s.self.hp_pct           = bot->GetMaxHealth() > 0 ? int(100.0f * bot->GetHealth() / bot->GetMaxHealth()) : 0;
    s.self.mana_pct         = bot->GetMaxPower(POWER_MANA) > 0 ? int(100.0f * bot->GetPower(POWER_MANA) / bot->GetMaxPower(POWER_MANA)) : -1;
    s.self.gold_copper      = static_cast<int64_t>(bot->GetMoney());
    s.self.is_in_combat     = bot->IsInCombat();
    s.self.is_resting       = bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_RESTING);
    s.self.is_dead          = bot->isDead();

    if (Map* m = bot->GetMap()) {
        s.location.map      = m->GetMapName() ? m->GetMapName() : "";
    }
    s.location.zone         = sAreaTableStore.LookupEntry(bot->GetZoneId()) ?
                              sAreaTableStore.LookupEntry(bot->GetZoneId())->area_name[0] : "";
    s.location.subzone      = sAreaTableStore.LookupEntry(bot->GetAreaId()) ?
                              sAreaTableStore.LookupEntry(bot->GetAreaId())->area_name[0] : "";
    s.location.position     = {bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};

    // Goal block from rpgInfo.
    auto& rpg = botAI->rpgInfo;
    switch (rpg.GetStatus()) {
        case RPG_IDLE:          s.goal.current = "Idle";         break;
        case RPG_GO_GRIND:      s.goal.current = "GoGrind";      break;
        case RPG_GO_CAMP:       s.goal.current = "GoCamp";       break;
        case RPG_WANDER_NPC:    s.goal.current = "WanderNpc";    break;
        case RPG_WANDER_RANDOM: s.goal.current = "WanderRandom"; break;
        case RPG_DO_QUEST:      s.goal.current = "DoQuest";      break;
        case RPG_TRAVEL_FLIGHT: s.goal.current = "TravelFlight"; break;
        case RPG_REST:          s.goal.current = "Rest";         break;
        case RPG_OUTDOOR_PVP:   s.goal.current = "OutdoorPvp";   break;
        default:                s.goal.current = "Idle";         break;
    }
    if (auto* dq = std::get_if<NewRpgInfo::DoQuest>(&rpg.data)) {
        nlohmann::json p;
        p["quest_id"] = dq->questId;
        p["objective_idx"] = dq->objectiveIdx;
        s.goal.params_json = p.dump();
    }

    // Quest log: walk the first N slots.
    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE && s.quest_log.size() < 10; ++slot) {
        uint32 qid = bot->GetQuestSlotQuestId(slot);
        if (!qid) continue;
        const Quest* q = sObjectMgr->GetQuestTemplate(qid);
        if (!q) continue;
        QuestLogEntry e;
        e.id = qid;
        e.title = q->GetTitle();
        QuestStatus st = bot->GetQuestStatus(qid);
        e.progress = (st == QUEST_STATUS_COMPLETE) ? "complete, turn in" : "in progress";
        s.quest_log.push_back(std::move(e));
    }

    // Inventory highlights: bag-used summary; consumable detection deferred.
    uint32 used = 0, total = 0;
    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag) {
        if (Bag* b = bot->GetBagByPos(bag)) {
            used += b->GetItemCount();
            total += b->GetBagSize();
        }
    }
    s.inventory.bag_used = std::to_string(used) + "/" + std::to_string(total);

    // Social, event_log: leave empty for Phase 1. Phase 2 wires these.
    return s;
}

nlohmann::json BuildDigest(PlayerbotAI* botAI) {
    return BuildDigestJson(SnapshotBot(botAI));
}

#endif  // LLMAGENT_UNIT_TESTS
```

- [ ] **Step 3: Add `LLMAGENT_UNIT_TESTS` define to test CMakeLists**

Edit `tests/llmagent/CMakeLists.txt`. Inside `target_compile_options` block (or add it):

```cmake
target_compile_definitions(llmagent_unit_tests PRIVATE LLMAGENT_UNIT_TESTS)
```

Put this immediately after the existing `target_compile_options` block.

- [ ] **Step 4: Run unit tests — must still pass**

```bash
cmake --build build/llmagent_tests
./build/llmagent_tests/llmagent_unit_tests
```

Expected: all tests still pass; the `SnapshotBot` block is excluded by the `#ifndef LLMAGENT_UNIT_TESTS` guard.

- [ ] **Step 5: Commit**

```bash
git add src/Bot/LlmAgent/Tiers/Tier0_StateDigest.h src/Bot/LlmAgent/Tiers/Tier0_StateDigest.cpp tests/llmagent/CMakeLists.txt
git commit -m "feat(llm-agent): SnapshotBot — worldserver-thread digest reader

Reads from Player* / PlayerbotAI / Map / AreaTableStore / quest log /
inventory into the BotState POD. Guarded by LLMAGENT_UNIT_TESTS so
the unit-test target stays free of worldserver headers. Social and
event_log fields stay empty in Phase 1; Phase 2 wires them."
```

---

## Task 9: LlmReplanIdleTrigger + LlmReplanIdleAction

**Files:**
- Create: `src/Bot/LlmAgent/Triggers/LlmReplanIdleTrigger.h`
- Create: `src/Bot/LlmAgent/Triggers/LlmReplanIdleTrigger.cpp`
- Create: `src/Bot/LlmAgent/Actions/LlmReplanIdleAction.h`
- Create: `src/Bot/LlmAgent/Actions/LlmReplanIdleAction.cpp`

These files only compile inside the worldserver build. No unit tests — they're glue.

- [ ] **Step 1: Write `Triggers/LlmReplanIdleTrigger.h`**

```cpp
#ifndef _PLAYERBOT_LLM_REPLAN_IDLE_TRIGGER_H
#define _PLAYERBOT_LLM_REPLAN_IDLE_TRIGGER_H

#include "Trigger.h"

class LlmReplanIdleTrigger : public Trigger {
  public:
    LlmReplanIdleTrigger(PlayerbotAI* ai) : Trigger(ai, "llm replan idle") {}
    bool IsActive() override;
};

#endif
```

- [ ] **Step 2: Write `Triggers/LlmReplanIdleTrigger.cpp`**

```cpp
#include "Triggers/LlmReplanIdleTrigger.h"
#include "LlmAgentManager.h"
#include "Playerbots/PlayerbotAI.h"
#include "NewRpgInfo.h"

bool LlmReplanIdleTrigger::IsActive() {
    if (!botAI) return false;
    auto& mgr = LlmAgentManager::Instance();
    if (!mgr.Enabled()) return false;
    Player* bot = botAI->GetBot();
    if (!bot) return false;
    uint64_t guid = bot->GetGUID().GetRawValue();

    if (mgr.HasPendingResults(guid)) return true;

    if (botAI->rpgInfo.GetStatus() != RPG_IDLE) return false;
    if (mgr.IsInFlight(guid)) return false;
    return true;
}
```

- [ ] **Step 3: Write `Actions/LlmReplanIdleAction.h`**

```cpp
#ifndef _PLAYERBOT_LLM_REPLAN_IDLE_ACTION_H
#define _PLAYERBOT_LLM_REPLAN_IDLE_ACTION_H

#include "Action.h"

class LlmReplanIdleAction : public Action {
  public:
    LlmReplanIdleAction(PlayerbotAI* ai) : Action(ai, "llm replan idle") {}
    bool Execute(Event event) override;
};

#endif
```

- [ ] **Step 4: Write `Actions/LlmReplanIdleAction.cpp`**

```cpp
#include "Actions/LlmReplanIdleAction.h"
#include "LlmAgentManager.h"
#include "Tiers/Tier0_StateDigest.h"
#include "Schemas/Goal.h"
#include "Playerbots/PlayerbotAI.h"
#include "Log.h"
#include "NewRpgInfo.h"

#include <atomic>
#include <chrono>

bool LlmReplanIdleAction::Execute(Event /*event*/) {
    if (!botAI) return false;
    auto& mgr = LlmAgentManager::Instance();
    if (!mgr.Enabled()) return false;
    Player* bot = botAI->GetBot();
    if (!bot) return false;
    uint64_t guid = bot->GetGUID().GetRawValue();

    // 1. Drain any completed results.
    auto results = mgr.DrainResults(guid);
    for (const auto& r : results) {
        if (r.parsed_status == "ok" && !r.parsed_goal.is_null()) {
            LOG_INFO("playerbots",
                     "[LlmAgent] bot={} proposed={} lat={}ms",
                     r.bot_name,
                     r.parsed_goal.value("goal", std::string{"?"}),
                     r.inference_ms);
        } else if (r.parsed_status == "schema_error") {
            // schema_error is rare and interesting — never throttle.
            LOG_WARN("playerbots",
                     "[LlmAgent] bot={} status=schema_error err='{}' lat={}ms",
                     r.bot_name, r.validator_error, r.inference_ms);
        } else {
            // transport_error / http_error / timeout — throttle to 1/60s server-wide.
            using clock = std::chrono::steady_clock;
            static std::atomic<int64_t> last_warn_ms{0};
            auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              clock::now().time_since_epoch()).count();
            int64_t prev = last_warn_ms.load();
            if (now_ms - prev >= 60000 &&
                last_warn_ms.compare_exchange_strong(prev, now_ms)) {
                LOG_WARN("playerbots",
                         "[LlmAgent] bot={} status={} err='{}' lat={}ms (further warnings suppressed 60s)",
                         r.bot_name, r.parsed_status, r.validator_error, r.inference_ms);
            }
        }
    }

    // 2. Enqueue a new request if eligible.
    if (botAI->rpgInfo.GetStatus() != RPG_IDLE) return false;
    if (mgr.IsInFlight(guid)) return false;

    BotState state;
    try {
        state = SnapshotBot(botAI);
    } catch (...) {
        LOG_WARN("playerbots", "[LlmAgent] SnapshotBot threw; skipping enqueue");
        return false;
    }
    nlohmann::json digest = BuildDigestJson(state);

    const LlmAgentConfig& cfg = mgr.Config();
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

    return false;  // do not suppress the rule-based NewRpgStatusUpdateAction
}
```

- [ ] **Step 5: Commit (no test step — these are worldserver-only glue)**

```bash
git add src/Bot/LlmAgent/Triggers/ src/Bot/LlmAgent/Actions/
git commit -m "feat(llm-agent): LlmReplanIdleTrigger + LlmReplanIdleAction

Trigger fires on Enabled AND (HasPendingResults OR (RPG_IDLE AND
!IsInFlight)). Action drains results first, then enqueues a new
request only if still Idle and not in-flight. Returns false so the
rule-based NewRpgStatusUpdateAction continues to fire alongside.

Unit-test coverage for these classes is deferred — they're thin
glue over already-tested manager/digest/schema components."
```

---

## Task 10: LlmAgentStrategy + context classes

**Files:**
- Create: `src/Bot/LlmAgent/Strategy/LlmAgentStrategy.h`
- Create: `src/Bot/LlmAgent/Strategy/LlmAgentStrategy.cpp`
- Create: `src/Bot/LlmAgent/Context/LlmAgentActionContext.h`
- Create: `src/Bot/LlmAgent/Context/LlmAgentTriggerContext.h`
- Create: `src/Bot/LlmAgent/Context/LlmAgentStrategyContext.h`
- Modify: `src/Bot/Engine/BuildSharedActionContexts.cpp`
- Modify: `src/Bot/Engine/BuildSharedTriggerContexts.cpp`
- Modify: `src/Bot/Engine/BuildSharedStrategyContexts.cpp`

- [ ] **Step 1: Write `Strategy/LlmAgentStrategy.h`**

```cpp
#ifndef _PLAYERBOT_LLMAGENT_STRATEGY_H
#define _PLAYERBOT_LLMAGENT_STRATEGY_H

#include "Strategy.h"

class LlmAgentStrategy : public Strategy {
  public:
    LlmAgentStrategy(PlayerbotAI* ai) : Strategy(ai) {}
    std::string const getName() override { return "llm agent"; }
    std::vector<NextAction> getDefaultActions() override { return {}; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    void InitMultipliers(std::vector<Multiplier*>&) override {}
};

#endif
```

- [ ] **Step 2: Write `Strategy/LlmAgentStrategy.cpp`**

```cpp
#include "Strategy/LlmAgentStrategy.h"

void LlmAgentStrategy::InitTriggers(std::vector<TriggerNode*>& triggers) {
    triggers.push_back(
        new TriggerNode(
            "llm replan idle",
            {NextAction("llm replan idle", 4.0f)}
        )
    );
}
```

- [ ] **Step 3: Write `Context/LlmAgentActionContext.h`**

```cpp
#ifndef _PLAYERBOT_LLMAGENT_ACTION_CONTEXT_H
#define _PLAYERBOT_LLMAGENT_ACTION_CONTEXT_H

#include "NamedObjectContext.h"
#include "Actions/LlmReplanIdleAction.h"

class LlmAgentActionContext : public NamedObjectContext<Action> {
  public:
    LlmAgentActionContext() {
        creators["llm replan idle"] = &LlmAgentActionContext::llm_replan_idle;
    }
  private:
    static Action* llm_replan_idle(PlayerbotAI* ai) { return new LlmReplanIdleAction(ai); }
};

#endif
```

- [ ] **Step 4: Write `Context/LlmAgentTriggerContext.h`**

```cpp
#ifndef _PLAYERBOT_LLMAGENT_TRIGGER_CONTEXT_H
#define _PLAYERBOT_LLMAGENT_TRIGGER_CONTEXT_H

#include "NamedObjectContext.h"
#include "Triggers/LlmReplanIdleTrigger.h"

class LlmAgentTriggerContext : public NamedObjectContext<Trigger> {
  public:
    LlmAgentTriggerContext() {
        creators["llm replan idle"] = &LlmAgentTriggerContext::llm_replan_idle;
    }
  private:
    static Trigger* llm_replan_idle(PlayerbotAI* ai) { return new LlmReplanIdleTrigger(ai); }
};

#endif
```

- [ ] **Step 5: Write `Context/LlmAgentStrategyContext.h`**

```cpp
#ifndef _PLAYERBOT_LLMAGENT_STRATEGY_CONTEXT_H
#define _PLAYERBOT_LLMAGENT_STRATEGY_CONTEXT_H

#include "NamedObjectContext.h"
#include "Strategy/LlmAgentStrategy.h"

class LlmAgentStrategyContext : public NamedObjectContext<Strategy> {
  public:
    LlmAgentStrategyContext() {
        creators["llm agent"] = &LlmAgentStrategyContext::llm_agent;
    }
  private:
    static Strategy* llm_agent(PlayerbotAI* ai) { return new LlmAgentStrategy(ai); }
};

#endif
```

- [ ] **Step 6: Wire `LlmAgentActionContext` into `BuildSharedActionContexts.cpp`**

In `src/Bot/Engine/BuildSharedActionContexts.cpp`:

Add include near the others (alphabetical order with the other includes):

```cpp
#include "Bot/LlmAgent/Context/LlmAgentActionContext.h"
```

Inside the `BuildSharedActionContexts` function body, append one line at the bottom (just before the closing brace):

```cpp
    actionContexts.Add(new LlmAgentActionContext());
```

- [ ] **Step 7: Wire `LlmAgentTriggerContext` into `BuildSharedTriggerContexts.cpp`**

Same pattern: add the include and the `triggerContexts.Add(new LlmAgentTriggerContext())` line at the end of the function.

- [ ] **Step 8: Wire `LlmAgentStrategyContext` into `BuildSharedStrategyContexts.cpp`**

Same pattern: add the include and the `strategyContexts.Add(new LlmAgentStrategyContext())` line at the end of the function.

- [ ] **Step 9: Commit**

```bash
git add src/Bot/LlmAgent/Strategy/ src/Bot/LlmAgent/Context/ \
        src/Bot/Engine/BuildSharedActionContexts.cpp \
        src/Bot/Engine/BuildSharedTriggerContexts.cpp \
        src/Bot/Engine/BuildSharedStrategyContexts.cpp
git commit -m "feat(llm-agent): register strategy/trigger/action with AiObjectContext

Three new NamedObjectContext subclasses map the names \"llm agent\",
\"llm replan idle\" (trigger), and \"llm replan idle\" (action) to
their constructors. Wired into the existing BuildShared*Contexts
files alongside ChatActionContext etc."
```

---

## Task 11: Config keys + factory wiring

**Files:**
- Modify: `src/PlayerbotAIConfig.h`
- Modify: `src/PlayerbotAIConfig.cpp`
- Modify: `src/Bot/Factory/AiFactory.cpp`
- Modify: `conf/playerbots.conf.dist`

- [ ] **Step 1: Add an sConfigMgr-shaped adapter to PlayerbotAIConfig**

In `src/PlayerbotAIConfig.h`, add includes and a member declaration. Near the top:

```cpp
#include "Bot/LlmAgent/LlmAgentConfig.h"
```

Inside the `PlayerbotAIConfig` class declaration, add a public member:

```cpp
    LlmAgentConfig llmAgent;
```

- [ ] **Step 2: Wire load logic in PlayerbotAIConfig.cpp**

Near the top of the file, add:

```cpp
#include "Configuration/Config.h"

namespace {
struct SConfigMgrSource {
    template <typename T>
    T Get(const char* key, T default_value) const {
        return sConfigMgr->GetOption<T>(key, default_value);
    }
};
}  // anon
```

Inside the config loader function (near lines 652-653 where `autoDoQuests` and `enableNewRpgStrategy` are loaded), add:

```cpp
    llmAgent = LoadLlmAgentConfig(SConfigMgrSource{});
```

- [ ] **Step 3: Initialize the manager at end of `PlayerbotAIConfig::Initialize()`**

The load function is `bool PlayerbotAIConfig::Initialize()` at `src/PlayerbotAIConfig.cpp:60`. Add the include at the top of `PlayerbotAIConfig.cpp`:

```cpp
#include "Bot/LlmAgent/LlmAgentManager.h"
```

Then immediately before the `return true;` at the end of `PlayerbotAIConfig::Initialize()` (currently around line 714, right after the three `"---..."` `LOG_INFO("server.loading", ...)` banner lines), add:

```cpp
    LlmAgentManager::Instance().Start(llmAgent);
```

The resulting tail of the function reads:

```cpp
    LOG_INFO("server.loading", "---------------------------------------");
    LOG_INFO("server.loading", "       mod-playerbots initialized      ");
    LOG_INFO("server.loading", "---------------------------------------");

    LlmAgentManager::Instance().Start(llmAgent);

    return true;
}
```

- [ ] **Step 4: Conditionally add the strategy in AiFactory.cpp**

In `src/Bot/Factory/AiFactory.cpp`, find the block around line 615:

```cpp
            if (sPlayerbotAIConfig.enableNewRpgStrategy)
                nonCombatEngine->addStrategy("new rpg", false);
```

Immediately after that line (before the `else if`), add:

```cpp

            if (sPlayerbotAIConfig.llmAgent.Enabled)
                nonCombatEngine->addStrategy("llm agent", false);
```

This keeps the LLM strategy as an additive overlay rather than a replacement for `new rpg`.

- [ ] **Step 5: Append config keys to `conf/playerbots.conf.dist`**

Add at the end of the file:

```ini

####################################################################################################
# LLM AGENT (Phase 1 — plumbing spike)
#
# Sends bot state digests to a local llama-server on every Idle transition.
# When disabled (default), zero LlmAgent code path executes. When enabled,
# the LLM call happens in parallel with the existing rule-based transition
# but the LLM's output is NEVER applied — Phase 1 is log-only.
#
# All responses are written as JSON Lines to AiPlayerbot.LlmAgent.JsonlPath.

# Default: 0 (disabled)
AiPlayerbot.LlmAgent.Enabled = 0

# llama-server base URL. POST goes to ${Endpoint}/v1/chat/completions.
AiPlayerbot.LlmAgent.Endpoint = "http://127.0.0.1:8080"

# Model label sent in the OpenAI-shaped request body.
AiPlayerbot.LlmAgent.Model = "qwen2.5-7b-instruct-q4_k_m.gguf"

# Worker threads (validated by Phase 0.5: slots=4 is goldilocks).
AiPlayerbot.LlmAgent.WorkerThreads = 4

# Per-request timeout. p95 measured at 2.7s; default leaves headroom.
AiPlayerbot.LlmAgent.RequestTimeoutMs = 15000

# Where the JSONL audit log goes (relative to worldserver CWD).
AiPlayerbot.LlmAgent.JsonlPath = "logs/llm_agent_phase1.jsonl"

# Override the default system prompt (leave empty to use the built-in).
AiPlayerbot.LlmAgent.SystemPrompt = ""
```

- [ ] **Step 6: Commit**

```bash
git add src/PlayerbotAIConfig.h src/PlayerbotAIConfig.cpp src/Bot/Factory/AiFactory.cpp conf/playerbots.conf.dist
git commit -m "feat(llm-agent): wire config + factory registration

PlayerbotAIConfig.llmAgent is loaded via the templated source
adapter on sConfigMgr. LlmAgentManager::Start is called once at
end of config load. AiFactory adds the \"llm agent\" strategy to
the non-combat engine when Enabled — as an additive overlay,
not a replacement for \"new rpg\".

Seven new config keys documented in playerbots.conf.dist."
```

---

## Task 12: Shutdown hook

**Files:**
- Modify: one of `src/PlayerbotsMgr.cpp`, `src/RandomPlayerbotMgr.cpp`, or the world-stop hook location identified below.

- [ ] **Step 1: Identify the shutdown seam**

Run:

```bash
grep -rn "OnShutdown\|UnloadAll\|world stop\|world::stop\|WorldShutdown" src/ --include="*.cpp" --include="*.h" | head -20
```

Pick the function that runs once at server shutdown. AzerothCore module hooks typically include `WorldScript::OnShutdown` or similar.

If no clear hook exists, fall back to: extend `RandomPlayerbotMgr::~RandomPlayerbotMgr()` or the static destructor pattern. The Manager singleton already calls `Shutdown()` in its destructor, but explicit early shutdown is preferred so JSONL is flushed before file descriptors are reclaimed.

- [ ] **Step 2: Add explicit Shutdown call**

In the identified shutdown function, add:

```cpp
#include "Bot/LlmAgent/LlmAgentManager.h"
// ... inside the shutdown function:
    LlmAgentManager::Instance().Shutdown();
```

- [ ] **Step 3: Commit**

```bash
git add <the-modified-file>
git commit -m "feat(llm-agent): explicit Shutdown at world stop

Ensures workers join and JSONL flushes before process exit.
The singleton destructor would catch this regardless, but calling
Shutdown explicitly avoids depending on static-destruction order."
```

---

## Task 13: Build worldserver on Heimdal + run unit tests

**Files:** none (this is execution)

- [ ] **Step 1: Push branch**

```bash
git push -u origin claude/llm-agent-phase-1-plumbing-spike
```

- [ ] **Step 2: SSH to Heimdal and pull**

```bash
ssh heimdal "cd ~/mod-playerbots && git fetch origin && git checkout claude/llm-agent-phase-1-plumbing-spike && git pull"
```

(Adjust `heimdal` host alias and path to match user's setup.)

- [ ] **Step 3: Build the unit-test binary on Heimdal**

```bash
ssh heimdal "cd ~/mod-playerbots && cmake -S tests/llmagent -B build/llmagent_tests && cmake --build build/llmagent_tests -j && ./build/llmagent_tests/llmagent_unit_tests"
```

Expected: `[doctest] Status: SUCCESS!` All ~32 test cases pass.

- [ ] **Step 4: Build the worldserver**

```bash
ssh heimdal "cd ~/azerothcore/build && cmake --build . -j --target worldserver"
```

(Adjust to user's AzerothCore build path.)

Expected: clean build with no warnings introduced by the new code.

- [ ] **Step 5: Commit any fixes needed for Heimdal compile errors**

If the build surfaced compile errors (likely some include paths or AC version-specific APIs), fix them inline and commit each fix as a separate commit with a clear message describing what AzerothCore symbol/API needed to change.

---

## Task 14: Smoke test on Heimdal

**Files:** none (documentation in this plan + execution)

- [ ] **Step 1: Confirm llama-server is up**

```bash
ssh heimdal "systemctl --user status llama-server.service || sudo systemctl status llama-server.service"
ssh heimdal "curl -s http://127.0.0.1:8080/v1/models | head -20"
```

Expected: service is `active (running)` and `curl` returns a JSON list including a Qwen model.

- [ ] **Step 2: Set `Enabled = 1` in playerbots.conf**

Edit `conf/playerbots.conf` (the live, non-dist copy) and set:

```ini
AiPlayerbot.LlmAgent.Enabled = 1
```

- [ ] **Step 3: Restart worldserver**

```bash
ssh heimdal "sudo systemctl restart worldserver.service"
```

(Or however the user runs the worldserver.)

- [ ] **Step 4: Confirm baseline tick health BEFORE bots load**

Tail `Server.log` for `Tick` warnings or unusual latency for 60 s. Note the baseline.

- [ ] **Step 5: Spawn 20 bots and let them idle**

Connect as admin, run the playerbot spawn command(s) that bring up 20 random-level bots in a major city. Let them sit for 10 min minimum.

- [ ] **Step 6: Tail the JSONL log**

```bash
ssh heimdal "tail -f ~/server/logs/llm_agent_phase1.jsonl"
```

(Path may differ; resolve to `worldserver_cwd/logs/llm_agent_phase1.jsonl`.)

- [ ] **Step 7: Validate the JSONL stream**

After 10 min, run:

```bash
ssh heimdal "wc -l ~/server/logs/llm_agent_phase1.jsonl && \
             jq -r '.parsed_status' ~/server/logs/llm_agent_phase1.jsonl | sort | uniq -c && \
             jq -r '.inference_ms' ~/server/logs/llm_agent_phase1.jsonl | sort -n | awk '{ a[NR]=\$1 } END { print \"p50=\" a[int(NR*0.5)] \" p95=\" a[int(NR*0.95)] \" n=\" NR }'"
```

Expected:
- `wc -l` ≥ 20 (at least one call per bot over 10 minutes).
- `parsed_status` distribution: majority `ok`. Some `schema_error` is acceptable (Phase 0.5 saw 0% on this combo; in-game digests may vary).
- p50 inference 1000-2500 ms, p95 ≤ 5000 ms (Phase 0.5 measured p50=1.6s, p95=2.7s).

- [ ] **Step 8: Confirm no tick regression**

Tail `Server.log` over the smoke-test window. Compare tick warning count to baseline. Acceptable: ≤1 tick warning per minute on a normal Heimdal load.

- [ ] **Step 9: Toggle off**

Set `Enabled = 0`, restart, confirm zero new JSONL lines and zero `[LlmAgent]` INFO lines in `Server.log`.

- [ ] **Step 10: Document the run**

Create `results/2026-05-12-llm-phase-1-smoke/summary.md` capturing:

- Date/time of the run
- Number of bots, duration
- JSONL line count, parsed_status distribution, p50/p95 inference_ms
- Any tick warnings observed
- Any unexpected failures
- A copy of the first 20 JSONL lines for sanity reference

- [ ] **Step 11: Commit**

```bash
git add results/2026-05-12-llm-phase-1-smoke/
git commit -m "test(llm-agent): record Phase 1 smoke test results

Bot count: <N>
Duration: <X> minutes
parsed_status distribution: <ok=N1, schema_error=N2, ...>
Latency: p50=<X>ms, p95=<Y>ms
Tick regression: <none / N warnings>"
```

- [ ] **Step 12: Push**

```bash
git push origin claude/llm-agent-phase-1-plumbing-spike
```

---

## Phase 1 success criteria

All three must hold before invoking `finishing-a-development-branch`:

1. `make llmagent_unit_tests && ./llmagent_unit_tests` reports all ~32 test cases passing on Heimdal.
2. With `LlmAgent.Enabled = 0`, the built worldserver binary runs without executing any LlmAgent code path. The proxy: zero `[LlmAgent]` lines in Server.log, JSONL file does not get created.
3. With `LlmAgent.Enabled = 1`, a 30-min Heimdal run with 20 bots produces a JSONL file with the expected cadence (≥20 records) and latency distribution (p50 1.0-2.5 s, p95 ≤ 5 s), and no measurable worldserver-tick regression.

When all three hold, Phase 1 is done. Phase 2 (validator + apply path) is then designed against the JSONL corpus this smoke test produces.

---

## Notes for the executing engineer

- The unit-test target builds standalone and does NOT depend on AzerothCore. You can iterate on the test pieces (Tasks 1-7) entirely on your laptop. Worldserver-linking tasks (8-12) require Heimdal.
- The `LLMAGENT_UNIT_TESTS` preprocessor flag (set in `tests/llmagent/CMakeLists.txt`) is the contract that prevents worldserver headers from leaking into the unit-test target. Anytime you add a file that touches `PlayerbotAI*` / `Player*`, wrap the Player-touching code in `#ifndef LLMAGENT_UNIT_TESTS`.
- If you hit a compile error on Heimdal about a specific AzerothCore API name (e.g. `GetGUID().GetRawValue()`), check the local fork's symbol — names occasionally drift. Fix in place and add a note in the commit.
- Do not push commits to `origin/main` from this branch. Use `finishing-a-development-branch` to merge when done.
