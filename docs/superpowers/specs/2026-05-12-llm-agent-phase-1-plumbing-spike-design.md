# LLM-Agent Phase 1 — Plumbing Spike Design

**Status:** Approved (2026-05-12)
**Parent design:** [`docs/llm_agent_design.md`](../../llm_agent_design.md)
**Predecessors:** Phase 0.5 Vulkan characterization
([spec](2026-05-12-llm-agent-phase-0.5-hardware-validation-design.md),
[results](../../../results/2026-05-12-vulkan/summary.md)); Phase 0.5 ROCm
comparison ([spec](2026-05-12-llm-agent-phase-0.5-rocm-comparison-design.md),
[results](../../../results/2026-05-12-rocm/summary.md)).

## 1. Goal

Wire the C++ plumbing that lets a worldserver-side action build a T0 state
digest, send it to llama-server, parse the response, and record the proposed
`NewRpgInfo` transition — **without ever applying it**. Validate the threading
model end-to-end against the production cell Phase 0.5 settled on
(Qwen 2.5 7B Q4_K_M + JSON-Schema response_format + `--parallel 4`).

## 2. Phase 1 invariant

The LLM's success, failure, latency, or absence has zero effect on bot
gameplay. The existing rule-based transition path
(`NewRpgStatusUpdateAction.RandomChangeStatus(...)` at
`src/Ai/World/Rpg/Action/NewRpgAction.cpp:63-65`) runs unconditionally.
Disabling the feature (`AiPlayerbot.LlmAgent.Enabled = 0`) returns the binary
to byte-identical runtime behavior, because no LlmAgent code path executes.

This invariant is what makes Phase 1 safe to merge to a live server.

## 3. Scope decisions

Settled during brainstorming (2026-05-12):

| Decision | Choice | Rationale |
|---|---|---|
| Enablement model | Single config flag, all bots or none | Simplest; no admin command needed in Phase 1. |
| T0 digest scope | **Full §6 implementation** | The digest is the most-iterated piece of the design; getting feedback in Phase 1 is worth the extra work. **Expands Phase 1+ beyond the §12 "stub spike" framing.** |
| Logging mechanism | Structured JSONL file + one INFO line per response in the worldserver log | JSONL gives Phase 2 a replay corpus; INFO line lets `tail -f` confirm plumbing health. |
| Build environment | Heimdal-only | AzerothCore already builds and runs on Bazzite; llama-server is local on `127.0.0.1:8080`. |
| Test methodology | Lightweight unit tests + manual smoke test | Sets a test pattern; mod-playerbots currently has zero C++ tests. |
| HTTP library | **cpp-httplib** (header-only, vendored) | Per parent design §9; fits AzerothCore's "no CMake changes for module sources" pattern. |
| JSON library | **nlohmann/json** (header-only, vendored) | Same vendoring pattern; widely used; BSD-compatible with AGPL. |
| Worker threads | **4** | Validated by Phase 0.5 (slots=4 is goldilocks; slots=1 queue-broken at 1 RPS, slots=8 has no decode advantage). |

## 4. Architecture overview

New subtree `src/Bot/LlmAgent/` per parent design §4. Auto-discovered by
AzerothCore's parent build; no top-level CMake change. One new test target
gets a small `tests/llmagent/CMakeLists.txt`.

```
Worldserver tick                 LlmAgentManager (background pool)        llama-server (Heimdal)
─────────────────                ─────────────────────────────────         ─────────────────────
LlmReplanIdleTrigger fires
  ↓
LlmReplanIdleAction
  ├─ build Tier0 digest ────────→ LlmRequest queued
  ├─ no-op result stack drain                    ↓ (worker thread)
  └─ return true                       LlmHttpClient::PostChatCompletion
                                                  │  POST /v1/chat/completions
                                                  ├──────────────────────────→ (1.6-2.7s)
                                                  ←──────────────────────────  JSON response
                                       parse + validate against Goal schema
                                       append full record to JSONL
                                       push LlmResult onto per-bot stack
                                       log summary INFO line

(next tick that touches the bot)
LlmReplanIdleAction
  └─ drain result stack
       ├─ if a result is present: log "would have set X" but DO NOT modify rpgInfo
       └─ existing NewRpgStatusUpdateAction.RandomChangeStatus(...) still runs as before
```

Critical invariants:

- Worker threads **never** touch `Player*` / `Unit*` / `Map*`. They receive a
  fully-built digest (POD) and return a fully-parsed response (POD).
- The worldserver tick **never** blocks on HTTP. It enqueues and returns
  immediately.
- The existing rule-based path keeps working unchanged with `Enabled=false`;
  with `Enabled=true`, the LLM call happens in parallel but **only the
  rule-based path's output is acted on**.

## 5. Components

### 5.1 `LlmAgentManager` (`LlmAgentManager.h/cpp`)

Singleton accessor `sLlmAgentManager`. Owns:

- The worker thread pool (4 by default, configurable).
- The request queue (`std::deque` + mutex + condvar, multi-producer
  single-consumer style at the worker side).
- The per-bot result map
  (`std::unordered_map<ObjectGuid, std::stack<LlmResult>>` + mutex).
- The in-flight set (`std::unordered_set<ObjectGuid>` + mutex), preventing a
  bot from enqueuing a second request while one is pending.

Public API:

```cpp
class LlmAgentManager {
 public:
    static LlmAgentManager& Instance();

    bool Enabled() const;                                 // reads config snapshot
    bool IsInFlight(ObjectGuid bot) const;
    bool HasPendingResults(ObjectGuid bot) const;
    void Enqueue(LlmRequest request);                     // marks in-flight
    std::vector<LlmResult> DrainResults(ObjectGuid bot);  // clears the bot's stack
    void Shutdown();                                      // joins all workers
};
```

Lifecycle: lazy-init on first use; `Shutdown()` called from
`PlayerbotsMgr` or equivalent server-stop hook.

The manager also owns a single `std::mutex` guarding JSONL appends, since
multiple workers write to the same file. Workers call a manager helper
`AppendJsonl(const std::string& line)` rather than opening the file
themselves; the file is opened once on first append and held open for the
process lifetime.

### 5.2 `LlmHttpClient` (`Client/LlmHttpClient.h/cpp`)

Thin wrapper around `httplib::Client`. One method:

```cpp
std::optional<RawResponse> PostChatCompletion(
    const std::string& body_json,
    std::chrono::milliseconds timeout);
```

Returns `nullopt` on transport error/timeout; populates `RawResponse::error`
for HTTP non-2xx. No retry logic in Phase 1.

### 5.3 `Tier0_StateDigest` (`Tiers/Tier0_StateDigest.h/cpp`)

Pure function `nlohmann::json BuildDigest(PlayerbotAI* botAI)`. Reads from
`Player*`, `botAI->rpgInfo`, the quest log, inventory, social state, and a
recent-event ring buffer. Produces the §6 JSON object from the parent
design. `memory_hints` is always `[]` in Phase 1.

The function is **decomposed** so the JSON-building step is pure:

```cpp
struct BotState { /* POD: name, race, class, level, hp_pct, ... */ };

BotState SnapshotBot(PlayerbotAI* botAI);            // touches game state
nlohmann::json BuildDigestJson(const BotState& s);   // pure; unit-testable
nlohmann::json BuildDigest(PlayerbotAI* botAI) {
    return BuildDigestJson(SnapshotBot(botAI));
}
```

Called on the worldserver thread. The resulting JSON gets serialized to a
string and stored in the `LlmRequest`.

### 5.4 `Schemas/Goal` (`Schemas/Goal.h/cpp`)

- `kGoalSchemaJson` — string constant containing the JSON Schema from
  parent design §7.1 (9-enum `goal` field, `params` per variant,
  `reasoning`, `ttl_minutes`).
- `ParseAndValidate(const std::string& raw_json) -> std::variant<ParsedGoal, ParseError>`
  — validates the LLM's response against the schema. Returns `ParsedGoal`
  (typed struct mirroring `NewRpgInfo::RpgData` variants) or `ParseError`
  with a message.

Phase 1 uses this to **tag JSONL records** as valid/invalid. It never feeds
back into game state.

### 5.5 `Strategy/LlmAgentStrategy` (`Strategy/LlmAgentStrategy.h/cpp`)

Standard `Strategy` subclass following the `NewRpgStrategy` pattern
(`src/Ai/World/Rpg/Strategy/NewRpgStrategy.cpp`).

- `getName()` returns `"llm agent"`.
- `getDefaultActions()` returns empty.
- `InitTriggers()` wires one `TriggerNode`:
  `"llm replan idle"` → `NextAction("llm replan idle", 4.0f)`.
- `InitMultipliers()` empty.

Registered via the same path other RPG strategies use
(`BuildSharedRpgContexts.cpp`).

### 5.6 `Triggers/LlmReplanIdleTrigger` (`Triggers/LlmReplanIdleTrigger.h/cpp`)

```cpp
class LlmReplanIdleTrigger : public Trigger {
  public:
    LlmReplanIdleTrigger(PlayerbotAI* botAI) : Trigger(botAI, "llm replan idle") {}
    bool IsActive() override;
};
```

`IsActive()` returns true when `LlmAgentManager::Instance().Enabled()` AND
**either**:

- `botAI->rpgInfo.GetStatus() == RPG_IDLE`
  AND `!LlmAgentManager::Instance().IsInFlight(bot->GetGUID())`
  (eligible to enqueue), **OR**
- `LlmAgentManager::Instance().HasPendingResults(bot->GetGUID())`
  (results to drain even if no longer idle).

The OR clause prevents queued responses from getting stuck behind state
transitions and prevents the per-bot result stack from leaking entries when
a bot leaves Idle before the response returns.

### 5.7 `Actions/LlmReplanIdleAction` (`Actions/LlmReplanIdleAction.h/cpp`)

```cpp
class LlmReplanIdleAction : public Action {
  public:
    LlmReplanIdleAction(PlayerbotAI* botAI) : Action(botAI, "llm replan idle") {}
    bool Execute(Event event) override;
};
```

`Execute()`:

1. **Drain results.** For each `LlmResult` from
   `LlmAgentManager::Instance().DrainResults(bot->GetGUID())`:
    - Build the summary `LOG_INFO` line (single line, ≤200 chars).
    - The full JSONL record was already written by the worker.
2. **Enqueue new request** if `RPG_IDLE` and not already in-flight:
    - Build digest via `Tier0_StateDigest::BuildDigest(botAI)`.
    - Build OpenAI-compatible request body (see §6).
    - `LlmAgentManager::Instance().Enqueue(...)`.
3. Return `false` unconditionally — we do **not** suppress the sibling
   `NewRpgStatusUpdateAction`.

### 5.8 `LlmAgentConfig` (`LlmAgentConfig.h/cpp`)

Typed view onto `sConfigMgr`. Reads keys once at startup, caches as members.
Fields per §8 below.

### 5.9 Vendored single-header libraries

- `Vendor/httplib.h` — cpp-httplib (current pinned release).
  License: MIT (compatible with AGPL).
- `Vendor/nlohmann_json.hpp` — nlohmann/json (current pinned release).
  License: MIT.
- `Vendor/doctest.h` — doctest, used only by the test target.
  License: MIT.

Pin specific commit hashes / release tags in the implementation plan.
License notices preserved verbatim.

## 6. Request / response shape

### 6.1 Outbound to llama-server

```json
{
  "model": "qwen2.5-7b-instruct-q4_k_m.gguf",
  "messages": [
    {"role": "system", "content": "<configured system prompt>"},
    {"role": "user",   "content": "<digest JSON string>"}
  ],
  "response_format": {
    "type": "json_schema",
    "json_schema": {
      "name": "BotGoal",
      "schema": { /* kGoalSchemaJson */ }
    }
  },
  "temperature": 0.4,
  "max_tokens": 256
}
```

`response_format.json_schema` is the form Phase 0.5 validated as the winner
over hand-written GBNF.

### 6.2 Inbound from llama-server

OpenAI-compatible envelope:

```json
{
  "id": "chatcmpl-...",
  "model": "...",
  "choices": [{
    "message": {
      "role": "assistant",
      "content": "{\"goal\":\"do_quest\",\"params\":{\"quest_id\":502,\"starting_objective_idx\":0},\"reasoning\":\"...\",\"ttl_minutes\":30}"
    },
    "finish_reason": "stop"
  }],
  "usage": {...}
}
```

`LlmHttpClient` extracts `choices[0].message.content` and hands it to
`Schemas/Goal::ParseAndValidate`.

### 6.3 JSONL record (one line per response)

```json
{
  "ts_enqueued_ms": 173123456789,
  "ts_dispatched_ms": 173123456839,
  "ts_completed_ms": 173123458451,
  "bot_guid": "12345",
  "bot_name": "Grimblade",
  "queue_wait_ms": 50,
  "inference_ms": 1612,
  "total_latency_ms": 1662,
  "digest": { /* full §6 object */ },
  "raw_response": "{\"goal\":\"do_quest\", ...}",
  "parsed_status": "ok",
  "parsed_goal": {
    "goal": "do_quest",
    "params": {"quest_id": 502, "starting_objective_idx": 0},
    "ttl_minutes": 30,
    "reasoning": "..."
  },
  "validator_error": null
}
```

On failure, `parsed_status` ∈ `{schema_error, transport_error, http_error,
timeout}`, `parsed_goal: null`, `validator_error` carries the message.
`raw_response` is truncated to 4 KB to keep lines bounded.

## 7. Data flow

```
t = 0 ms    Worldserver tick. Bot enters RPG_IDLE.
            NewRpgStatusTrigger fires its rule-based subtree as before.
            In parallel, LlmAgentStrategy's tree fires LlmReplanIdleTrigger.

t = 0 ms    LlmReplanIdleTrigger::IsActive():
              - Enabled? yes
              - GetStatus() == RPG_IDLE? yes
              - !IsInFlight(botGuid)? yes
              → true.

t = 0 ms    LlmReplanIdleAction::Execute() (worldserver thread):
              1. Drain results (empty on first call).
              2. digest = BuildDigest(botAI)
                 digest_str = digest.dump()
              3. Build OpenAI body (§6.1).
              4. Build LlmRequest POD.
              5. MarkInFlight(bot_guid); Enqueue(req).
              6. Return false.

t = 0 ms    Worldserver tick continues. NewRpgStatusUpdateAction runs.
            RandomChangeStatus(...) picks the new state. Bot transitions normally.

t ≈ 50 ms   Worker thread picks up request:
              - PostChatCompletion(body, 15000ms)
              - blocks on HTTP (NOT the tick)

t ≈ 1.6 s   llama-server returns (Qwen 7B json_schema slots=4 p50 per §15.1):
              - Parse envelope, extract content.
              - ParseAndValidate(content).
              - Build LlmResult POD with latency breakdown.
              - Append JSONL line (per-file mutex).
              - PushResult; ClearInFlight.

t ≈ next tick    LlmReplanIdleAction::Execute() runs again:
                   1. DrainResults returns one LlmResult.
                      - LOG_INFO summary line.
                   2. Bot may no longer be RPG_IDLE; if it is, enqueue another.
                   3. Return false.
```

## 8. Configuration keys

New entries in `conf/playerbots.conf.dist`:

| Key | Default | Notes |
|---|---|---|
| `AiPlayerbot.LlmAgent.Enabled` | `0` | Master switch. When 0, no LlmAgent code runs. |
| `AiPlayerbot.LlmAgent.Endpoint` | `http://127.0.0.1:8080` | llama-server base URL. POST → `${Endpoint}/v1/chat/completions`. |
| `AiPlayerbot.LlmAgent.Model` | `qwen2.5-7b-instruct-q4_k_m.gguf` | Sent in the `model` field. |
| `AiPlayerbot.LlmAgent.WorkerThreads` | `4` | Per Phase 0.5 validated config. |
| `AiPlayerbot.LlmAgent.RequestTimeoutMs` | `15000` | Per parent design §15.6. |
| `AiPlayerbot.LlmAgent.JsonlPath` | `logs/llm_agent_phase1.jsonl` | Resolved relative to worldserver CWD. |
| `AiPlayerbot.LlmAgent.SystemPrompt` | `(constexpr default)` | Configurable for A/B without recompile. Default in code. |

Seven keys total. Loaded once at startup by `LlmAgentConfig::Load()`,
cached as members. No live-reload in Phase 1.

The default system prompt (pinned for review at implementation time) reads
roughly:

> *You are an in-world decision-maker for a World of Warcraft NPC. Given the
> attached state digest, choose what the character should do next. Respond
> with a single JSON object matching the provided schema. Pick a goal that
> is plausible for the character's level, location, and current
> objectives. Prefer continuing existing quests over starting new ones when
> progress is partial. Be concise in `reasoning`.*

Exact wording is pinned in the implementation plan.

## 9. Error handling

| Failure | Detection | Action |
|---|---|---|
| llama-server unreachable | httplib transport error | JSONL `parsed_status="transport_error"`. Worker WARN throttled to 1/60 s. ClearInFlight. |
| HTTP non-2xx | `RawResponse::status != 200` | JSONL `parsed_status="http_error"`, body truncated to 4 KB. WARN throttled. |
| Read timeout | httplib timeout (per `RequestTimeoutMs`) | JSONL `parsed_status="timeout"`. WARN throttled. |
| Response not valid JSON OR fails schema | `ParseAndValidate` returns `ParseError` | JSONL `parsed_status="schema_error"`, `validator_error` populated. **Not throttled** (rare and interesting). |
| JSONL write fails | `ofstream` bad bit / exception | ERROR (not throttled). Drop the result; still push to result stack so summary INFO fires. |
| `BuildDigest` throws / hits nullptr | try/catch + nullptr guards | WARN. Don't enqueue this tick. No game-state impact. |

Three things Phase 1 explicitly does **not** do:

- **No retry** on transport/HTTP errors. Retries belong with the apply path
  in Phase 2.
- **No circuit breaker.** Same reason.
- **No backpressure beyond the per-bot in-flight check.** Queue depth is
  bounded by `N_bots`. Logged in JSONL via `queue_wait_ms`.

## 10. Testing strategy

### 10.1 Layer 1 — Unit tests (`tests/llmagent/`)

A new `llmagent_unit_tests` binary built from `tests/llmagent/CMakeLists.txt`
using **doctest** (single-header). Does not link against worldserver.

Test files and approximate counts:

- `test_digest_shape.cpp` (~8 cases): given a stubbed `BotState` POD, assert
  `BuildDigestJson` produces all §6 fields with correct types. Includes a
  realistic fixture (level-37 orc warrior in Hillsbrad mirroring the §6
  sketch).
- `test_goal_parser.cpp` (~12 cases): feed `Schemas/Goal::ParseAndValidate`
  known-good responses (one per variant), known-bad responses (wrong enum,
  missing required field, extra field, truncated JSON, empty). Assert
  correct classification.
- `test_http_client.cpp` (~5 cases): stand up an in-process `httplib::Server`
  on a free localhost port returning canned responses (and forced delays for
  timeouts). Verify `LlmHttpClient` returns the right `RawResponse` shape,
  honors `RequestTimeoutMs`, and surfaces error codes.

Run via `make llmagent_unit_tests && ./llmagent_unit_tests`.

### 10.2 Layer 2 — Threading-model test (same binary)

`test_manager_threading.cpp` stands up an in-process httplib mock server,
instantiates `LlmAgentManager` with 4 workers, enqueues 100 requests with
synthetic bot GUIDs at a steady cadence. Asserts:

- All 100 results come back within a deadline (no deadlock).
- In-flight tracking caps concurrent requests per bot at 1 (enqueue, second
  enqueue is rejected, observed via no second worker pickup).
- Result-drain is correctly per-bot (no cross-talk between GUIDs).
- Worker shutdown joins cleanly on `LlmAgentManager::Shutdown()`.

This is the test that validates the spike's stated goal.

### 10.3 Layer 3 — Manual smoke test on Heimdal

Documented procedure:

1. `git pull && build` on Heimdal.
2. Set `AiPlayerbot.LlmAgent.Enabled = 1` in `playerbots.conf`. Confirm
   `llama-server.service` is running and responding on `127.0.0.1:8080`.
3. Start worldserver. Spawn 20 bots in a major city, random levels, let
   them idle.
4. `tail -f logs/llm_agent_phase1.jsonl` for ~10 min.
5. Verify:
   - ~1 record per bot per ~5 min cadence.
   - Majority `parsed_status="ok"`.
   - `inference_ms` distribution matches §15.1 (p50 ~1.6 s, p95 ~2.7 s).
   - No worldserver-tick latency regression vs. baseline run.
6. Toggle `Enabled = 0`, restart, confirm zero new JSONL lines and zero
   `[LlmAgent]` INFO lines.

**Phase 1 done** = Layer 1 + Layer 2 green + Layer 3 passes once on a
≥30-min Heimdal run.

### 10.4 Explicitly not tested in Phase 1

- Decision quality of LLM responses (Phase 2 validator design).
- Long-duration stability (24-hour soak — Phase 2+).
- High-bot-count load (>20 bots).
- Memory sidecar paths (Phase 3).

## 11. Out of scope

Stays outside Phase 1 — lives in Phase 2 or later per parent design §12:

- `MemoryHttpClient`, memory sidecar, `memory_hints` population.
- `Tier2_Interactive`, `Tier3_ChatBrain`, `CloudRouter` (files don't exist).
- Validator that drives the apply path.
- Admin command. `Enabled` is server-wide; flip via config + restart.
- Persona cards.
- `HumanInSocialRangeTrigger`, `HumanChatTrigger`.
- Prometheus/StatsD metric export.
- Backpressure / circuit breaker beyond per-bot in-flight check.
- Multi-shard or remote-llama-server failover.

## 12. Module layout (final)

```
src/Bot/LlmAgent/
  LlmAgentManager.h/cpp
  LlmAgentConfig.h/cpp
  Client/
    LlmHttpClient.h/cpp
  Tiers/
    Tier0_StateDigest.h/cpp
  Schemas/
    Goal.h/cpp
  Strategy/
    LlmAgentStrategy.h/cpp
  Triggers/
    LlmReplanIdleTrigger.h/cpp
  Actions/
    LlmReplanIdleAction.h/cpp
  Vendor/
    httplib.h
    nlohmann_json.hpp
    doctest.h

tests/llmagent/
  CMakeLists.txt
  test_digest_shape.cpp
  test_goal_parser.cpp
  test_http_client.cpp
  test_manager_threading.cpp
```

Wiring point in existing code:

- `src/Bot/Engine/BuildSharedRpgContexts.cpp` — register `LlmAgentStrategy`,
  `LlmReplanIdleTrigger`, and `LlmReplanIdleAction` alongside the existing
  NewRpg components.

## 13. Open items deferred to Phase 2

- The system-prompt wording is v1; expect iteration once we have a JSONL
  corpus to inspect.
- The full §6 digest will likely need field tweaks (event-log window,
  inventory-highlights heuristic, social-state field set) — defer until
  smoke-test data informs the choices.
- Per-bot rate limiting may become necessary at higher bot counts; in
  Phase 1 the in-flight cap is sufficient.

## 14. Success criteria summary

1. `make llmagent_unit_tests && ./llmagent_unit_tests` reports all tests
   pass.
2. `LlmAgent.Enabled = 0` build is byte-identical at runtime to current
   `main` (no LlmAgent code executes).
3. `LlmAgent.Enabled = 1` + 20 bots + 30 min on Heimdal produces a JSONL
   file with the expected cadence and latency distribution, and no
   worldserver-tick regression.

When all three hold, Phase 1 is done and we can spec Phase 2 (validator +
apply path) on top of the corpus the smoke test produced.
