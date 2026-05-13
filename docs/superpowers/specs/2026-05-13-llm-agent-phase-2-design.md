# LLM-Agent Phase 2 — Validator + Apply Path Design

**Status:** Approved (2026-05-13)
**Parent design:** [`docs/llm_agent_design.md`](../../llm_agent_design.md)
**Predecessors:** Phase 0.5 Vulkan characterization
([spec](2026-05-12-llm-agent-phase-0.5-hardware-validation-design.md),
[results](../../../results/2026-05-12-vulkan/summary.md)); Phase 0.5 ROCm
comparison ([spec](2026-05-12-llm-agent-phase-0.5-rocm-comparison-design.md),
[results](../../../results/2026-05-12-rocm/summary.md));
Phase 1 plumbing spike
([spec](2026-05-12-llm-agent-phase-1-plumbing-spike-design.md),
[results](../../../results/2026-05-12-llm-phase-1-smoke/summary.md)).

## 1. Goal

Take Phase 1's log-only plumbing and make T1 drive bot behavior. Validate
LLM-proposed `NewRpgInfo` transitions in C++, apply the valid ones,
fall back to the rule-based path for the rest. Gate which bots get the LLM
path via per-GUID sampling plus a "social opt-in" for any bot a real
player engages with. Enrich the T0 digest enough that the LLM has reason
to pick non-idle goals.

## 2. Phase 2 invariant

**`ApplyMode=apply`** is the only mode that changes bot behavior.
**`ApplyMode=log`** is byte-identical to Phase 1's runtime behavior.
**`ApplyMode=shadow`** is byte-identical to `log` from the bot's
perspective — only the JSONL records change.

**`AiPlayerbot.LlmAgent.Enabled=0`** returns the binary to baseline.
No LlmAgent code path executes.

This invariant lets us merge Phase 2 to a live server with `ApplyMode=log`,
ramp to `shadow` once we trust the data, then flip to `apply` once we've
observed the validator's accept/reject distribution.

## 3. Scope decisions

Settled during brainstorming (2026-05-13):

| Decision | Choice | Rationale |
|---|---|---|
| Scope | **Full Phase 2** — validator + apply + cooldown + sampling + digest enrichment + telemetry | Phase 1 surfaced that the apply path alone isn't viable: queue math fails without cooldown, LLM picks 100% `idle` without digest enrichment. |
| Bot selection | **GUID-hash sampling + social opt-in** | "Sample by GUID hash" picked over admin-command and level-band. Stable per-bot across restarts. Social opt-in (any bot a real player whispers / invites) is additive on top — prefigures Phase 4's T2 (interactive tier) at low cost. |
| Runtime safety | **Three `ApplyMode` settings: `log`, `shadow`, `apply`** | Default `log`. `shadow` gives an A/B-able intermediate (record what would have been applied; bot keeps behaving rule-based). `apply` is production. |
| Validator strictness | **Hard reject on invalid** | No embedding snap-to-nearest (that's Phase 4+). Rejection → fallback to rule-based + 5 min cooldown. |
| Cooldown | **Respect the LLM's `ttl_minutes`** for accepted goals (capped at `MaxCooldownMinutes`, default 60). **`FallbackCooldownMs`** (default 5 min) for every other outcome. | Aligns with the design's "1 replan per ~5 min" cadence and prevents queue saturation. |
| Shadow-mode comparator | **Drop** the "what rule-based would have picked" comparator | YAGNI. Recording "would have applied X" is enough Phase 2 signal. Add the comparator in Phase 3 if shadow data is insufficient. |
| Opt-in persistence | **In-memory only**, cleared on Shutdown | Restart starts fresh. Matches the in-flight set's lifecycle. |

## 4. Architecture overview

Phase 2 keeps the Phase 1 subtree intact and adds four new responsibility
groups + enrichment of `Tier0_StateDigest`:

```
src/Bot/LlmAgent/
  [Phase 1 — unchanged]
    LlmAgentManager.h/cpp
    LlmAgentConfig.h/cpp
    Client/LlmHttpClient.h/cpp
    Schemas/Goal.h/cpp
    Strategy/LlmAgentStrategy.h/cpp
    Triggers/LlmReplanIdleTrigger.h/cpp
    Actions/LlmReplanIdleAction.h/cpp          (logic grows substantially)
    Context/{LlmAgentAction,Trigger,Strategy}Context.h
    Vendor/{httplib.h, nlohmann_json.hpp}

  [Phase 2 — new]
    Selector/
      BotSelector.h/cpp                          GUID-hash sampling + opt-in set
    Cooldown/
      BotCooldownMap.h/cpp                       per-bot cooldown expiry
    Validator/
      GoalValidator.h/cpp                        pure decision function
      ValidationContext.h/cpp                    POD + SnapshotForValidation
    Telemetry/
      LlmCounters.h/cpp                          atomic counters, DumpToLog
    EventBuffer/
      RecentEventBuffer.h/cpp                    ring buffer feeding event_log

  [Phase 2 — modified]
    Tiers/Tier0_StateDigest.h/cpp                event_log + nearby_humans + recent_whispers
    LlmAgentManager.h/cpp                        cooldown + telemetry hooks
    Actions/LlmReplanIdleAction.h/cpp            decision tree per ApplyMode

tests/llmagent/
  [Phase 1 — unchanged]
    test_config_load.cpp, test_goal_parser.cpp,
    test_http_client.cpp, test_digest_shape.cpp (extended for new fields),
    test_manager_threading.cpp (extended for shadow-mode + cooldown)

  [Phase 2 — new]
    test_validator.cpp
    test_selector.cpp
    test_cooldown.cpp
    test_counters.cpp
```

The `LlmReplanIdleAction::Execute()` shape grows:

```
LlmReplanIdleAction::Execute()
  1. Drain results.
     For each result:
       - If parsed_status == "ok":
           ctx = SnapshotForValidation(botAI)
           decision = ValidateGoalDecision(result.parsed_goal, ctx)
           record telemetry + JSONL fields
           if decision.accepted and ApplyMode==apply:
             ApplyGoalToRpgInfo(result.parsed_goal, botAI)
             cooldown.Set(guid, now + min(ttl_minutes, MaxCooldownMinutes))
             applied_this_tick = true
           elif decision.accepted and ApplyMode==shadow:
             record "would have applied X" in JSONL
             telemetry.IncShadowAccepted()
             cooldown.Set(guid, now + min(ttl_minutes, MaxCooldownMinutes))
           elif decision.accepted and ApplyMode==log:
             telemetry.IncLogAcceptedSkipped()
             cooldown.Set(guid, now + FallbackCooldownMs)
           else:  // validator rejected
             telemetry.IncFallbackUsed()
             cooldown.Set(guid, now + FallbackCooldownMs)
       - Else (transport/http/schema/timeout error):
           record telemetry
           cooldown.Set(guid, now + FallbackCooldownMs)

  2. Enqueue (only if eligible AND not applied this tick):
     if RPG_IDLE and selector.IsLlmBot(guid) and cooldown.Eligible(guid)
        and !IsInFlight(guid) and !applied_this_tick:
       digest = BuildDigest(botAI)   // now includes event_log + social
       body = build_openai_body(digest, cfg.SystemPrompt, cfg.Model)
       mgr.Enqueue(...)

  3. Return: applied_this_tick;  // true suppresses status_update for the tick
```

Two new event hooks for social opt-in, both in existing seams:

- **Whisper:** `PlayerbotAI::HandleCommand` is the existing chat-receive
  seam (parent design §3.3). When the sender is a real player and
  `cfg.SocialOptIn=1`, call `BotSelector::OptInBot(bot_guid)`.
- **Group invite:** The existing handler for inbound party-invite events.
  Same call.

## 5. Components

### 5.1 `Selector/BotSelector.h/cpp`

```cpp
class BotSelector {
  public:
    void Configure(uint32_t sample_pct, bool social_opt_in);
    bool IsLlmBot(uint64_t bot_guid) const;
    void OptInBot(uint64_t bot_guid);          // thread-safe, idempotent
    void Clear();                              // called from Shutdown

  private:
    uint32_t                          sample_pct_ = 0;
    bool                              social_enabled_ = true;
    mutable std::mutex                opt_in_mu_;
    std::unordered_set<uint64_t>      opt_in_;
};
```

`IsLlmBot` is `O(1)`:
```cpp
bool IsLlmBot(uint64_t guid) const {
    if (sample_pct_ == 0 && opt_in_.empty()) return false;  // fast path
    if (FNV1aHash(guid) % 100 < sample_pct_) return true;
    std::lock_guard<std::mutex> g(opt_in_mu_);
    return opt_in_.count(guid) > 0;
}
```

Uses FNV-1a hash so distribution is stable across runs and doesn't
depend on STL hash implementation.

Lives inside `LlmAgentManager` (single instance, configured once at Start).

### 5.2 `Cooldown/BotCooldownMap.h/cpp`

```cpp
class BotCooldownMap {
  public:
    bool Eligible(uint64_t bot_guid) const;
    void Set(uint64_t bot_guid, std::chrono::steady_clock::time_point until);
    void Clear();                              // Shutdown
  private:
    mutable std::mutex                              mu_;
    std::unordered_map<uint64_t,
        std::chrono::steady_clock::time_point>     cooldowns_;
};
```

`Eligible(guid)` returns true if no cooldown is set OR the cooldown's
expiry is in the past. `Set` overwrites. Uses `steady_clock` not
`system_clock` — survives clock adjustments and DST.

Lives inside `LlmAgentManager`.

### 5.3 `Validator/GoalValidator.h/cpp` + `Validator/ValidationContext.h/cpp`

The split between pure decision logic and worldserver-side snapshot
mirrors `Tier0_StateDigest`'s pattern (pure `BuildDigestJson` +
worldserver-side `SnapshotBot`). Lets us unit-test the validator with
no AzerothCore dependency.

```cpp
// ValidationContext.h — POD, cross-thread safe
struct QuestLogContextEntry {
    uint32_t  id;
    uint32_t  status;        // 0=incomplete, 1=complete, etc.
    int32_t   objective_count;
};

struct BotValidationContext {
    uint32_t  bot_level = 0;
    uint32_t  map_id = 0;
    double    map_min_x = -100000.0;
    double    map_max_x =  100000.0;
    double    map_min_y = -100000.0;
    double    map_max_y =  100000.0;
    std::vector<QuestLogContextEntry>   quest_log;
    std::vector<uint64_t>               nearby_creature_guids;
    std::unordered_set<uint32_t>        known_flight_node_ids;
    std::unordered_set<uint32_t>        valid_capture_point_spawn_ids;
};

// Worldserver-thread only, guarded by #ifndef LLMAGENT_UNIT_TESTS
BotValidationContext SnapshotForValidation(PlayerbotAI* botAI);
```

```cpp
// GoalValidator.h — pure, unit-testable
struct ValidationResult {
    bool        accepted = false;
    std::string reject_reason;       // e.g. "rejected_quest_not_in_log"
};

ValidationResult ValidateGoalDecision(
    const ParsedGoal& g,
    const BotValidationContext& ctx);
```

Per-variant rules:

| Variant | Rules |
|---|---|
| `Idle`, `WanderRandom`, `Rest` | Always accepted. |
| `DoQuest` | `quest_id` in `ctx.quest_log` AND status != 2 (complete-already-turned-in) AND `0 ≤ starting_objective_idx < quest.objective_count`. |
| `GoGrind`, `GoCamp` | `map_id == ctx.map_id` AND position is within map bounds. |
| `WanderNpc` | `npc_guid ∈ ctx.nearby_creature_guids` (in bot's grid). Permissive: if `nearby_creature_guids` is empty, skip the check (let the trigger fail at action time). |
| `TravelFlight` | `destination_node_id ∈ ctx.known_flight_node_ids`. (The flightmaster check is best-effort — bots discover taxi nodes lazily.) |
| `OutdoorPvp` | `capture_point_spawn_id ∈ ctx.valid_capture_point_spawn_ids`. |

Rejection reasons are constants (small, fixed set) so the telemetry's
`validator_rejected_<reason>` keys are bounded.

### 5.4 `Telemetry/LlmCounters.h/cpp`

```cpp
class LlmCounters {
  public:
    // All increment paths are lock-free (std::atomic).
    void IncEnqueued();
    void IncStatus(const std::string& parsed_status);   // ok / schema_error / ...
    void IncValidator(const ValidationResult&);          // accepted vs. each reject_reason
    void IncApplied();                                   // apply mode + accepted + applied
    void IncShadowAccepted();                            // shadow mode + accepted + recorded
    void IncLogAcceptedSkipped();                        // log mode + accepted + skipped
    void IncFallbackUsed();                              // any non-accepted outcome
    void IncAppliedThrew();                              // ChangeTo* threw at apply time

    void DumpToLog() const;                              // Shutdown
};
```

Counter set (small, fixed):
- `enqueued_total`
- `parsed_ok`, `parsed_schema_error`, `parsed_transport_error`,
  `parsed_http_error`, `parsed_timeout`
- `validator_accepted`, plus one bucket per reject_reason string
- `applied`, `shadow_accepted`, `log_accepted_skipped`, `fallback_used`,
  `applied_threw`

Each drained result hits exactly one of the four outcome counters
(`applied` / `shadow_accepted` / `log_accepted_skipped` / `fallback_used`)
plus one of the validator decision counters.

No Prometheus/StatsD in Phase 2 — just `DumpToLog()` at Shutdown.

### 5.5 `EventBuffer/RecentEventBuffer.h/cpp`

Per-bot ring buffer of up to `cfg.EventLogSize` strings (default 20).

```cpp
class RecentEventBuffer {
  public:
    void Push(uint64_t bot_guid, std::string event);
    std::vector<std::string> Snapshot(uint64_t bot_guid) const;
    void Clear(uint64_t bot_guid);
  private:
    mutable std::mutex mu_;
    std::unordered_map<uint64_t, std::deque<std::string>> buffers_;
    uint32_t cap_ = 20;
};
```

Lives inside `LlmAgentManager`. Hooks that call `Push` (added in Phase 2):

- `PlayerbotAI::HandleCommand` (incoming whisper) — push
  `"received whisper from {sender}: {text}"` (truncated to 80 chars).
- Combat kill event — push `"killed {creature_name} (+{xp} xp)"`.
- Loot event — push `"looted {item_name} x{count}"`.
- Quest progress — push `"quest progress: {quest_title} {progress}"`.
- Group join/leave — push `"joined group with {leader}"` / `"left group"`.

Each hook is one new line in the existing code paths — no architectural
changes to `PlayerbotAI`.

### 5.6 `Tier0_StateDigest` enrichment

`BotState` (renamed `LlmBotState` in Phase 1) gains:

- **`event_log`** — populated from `RecentEventBuffer::Snapshot(bot_guid)`.
- **`social.nearby_humans`** — scan players in bot's grid via existing
  AI helper, filter `!isPlayerbotAI(player)`, take top 5 by distance.
- **`social.recent_whispers`** — last 3 messages from real players in
  the last 60 s. Pulled from a small per-bot sliding window kept by the
  whisper hook (separate from `event_log` because we want the structured
  fields `from / text / age_s`).

The fields exist in `LlmBotState` from Phase 1 — they're just empty
arrays. Phase 2 wires up the data.

### 5.7 `LlmReplanIdleAction` — apply-mode decision tree

See §4's pseudocode. Key invariants:

1. Drain happens regardless of `ApplyMode` (so JSONL stays useful).
2. Validator runs only when `parsed_status == "ok"`.
3. `cooldown.Set` is called on **every** drained result (accepted,
   rejected, or error) — prevents tight re-enqueue loops.
4. `applied_this_tick = true` causes the action to return true, which
   suppresses `NewRpgStatusUpdateAction` for this tick (the bot
   transitions via LLM only, not both).
5. Apply mode is read from cached config (parsed once at startup).

## 6. Configuration keys

Six new entries on top of Phase 1's seven (total: 13). Append to
`conf/playerbots.conf.dist`:

| Key | Default | Description |
|---|---|---|
| `AiPlayerbot.LlmAgent.ApplyMode` | `"log"` | `log` / `shadow` / `apply`. Default keeps Phase 1 semantics. |
| `AiPlayerbot.LlmAgent.SamplePct` | `0` | 0-100. Hash-based bot selection. |
| `AiPlayerbot.LlmAgent.SocialOptIn` | `1` | Auto-opt-in any bot a real player whispers or party-invites. |
| `AiPlayerbot.LlmAgent.MaxCooldownMinutes` | `60` | Cap on the LLM's `ttl_minutes` for cooldown purposes. |
| `AiPlayerbot.LlmAgent.FallbackCooldownMs` | `300000` | Cooldown applied after rejected / error outcomes. |
| `AiPlayerbot.LlmAgent.EventLogSize` | `20` | Ring buffer length for the digest's `event_log`. |

Parsed once at `PlayerbotAIConfig::Initialize()`. No live-reload.

## 7. JSONL record additions

New fields on every record (extending Phase 1's shape):

```json
{
  ...existing Phase 1 fields...,
  "apply_mode": "shadow",
  "sample_hit": true,
  "selector_reason": "sample" | "social_opt_in",
  "validator_decision": "accepted" | "rejected_quest_not_in_log" | ...,
  "applied_transition": null,             // populated in apply mode on accept
  "would_have_applied": "do_quest:502"    // populated in shadow mode on accept
}
```

Phase 1's `parsed_goal` field continues to record the LLM's raw output.
The new fields record what we **did** with it.

## 8. Data flow

(See §4's pseudocode + the Phase 1 spec §7 for the cross-thread shape.
Same backbone: digest built on worldserver thread, HTTP on worker, drain
+ validate + apply on worldserver thread.)

## 9. Error handling (changes from Phase 1 only)

| New failure | Detection | Action |
|---|---|---|
| Validator rejection | `ValidateGoalDecision` returns `accepted=false` | JSONL `validator_decision = "rejected_<reason>"`. Counter incremented. Cooldown = FallbackCooldownMs. Fallback runs. No log emission (avoid flood at high reject rate). |
| `ChangeTo*` throws / asserts | try/catch around each apply call | LOG_ERROR with goal kind + params. `applied_threw` counter incremented. `botAI->rpgInfo.ChangeToIdle()` to recover. Cooldown = FallbackCooldownMs. **The bot does NOT stay in a corrupt state.** |
| State drift between snapshot and apply | The validator runs at apply time on fresh `Player*` state | Caught as ordinary rejection. |

Phase 1's failure modes (transport_error, http_error, schema_error,
timeout, JSONL write fail, digest exception) all stay as-is.

## 10. Testing strategy

### 10.1 Layer 1 — unit tests

New test files in `tests/llmagent/`:

- **`test_validator.cpp`** (~30 cases): one happy + 2-3 reject paths per
  variant. Quest not in log; quest already complete; objective_idx out of
  range; npc_guid not nearby; map_id mismatch; position out of bounds;
  flight node unknown; capture point invalid. Plus a smoke case for each
  of the always-accepted variants.
- **`test_selector.cpp`** (~8 cases):
  - SamplePct=0 hits nothing.
  - SamplePct=100 hits everything.
  - SamplePct=10 across 10000 synthetic GUIDs: chi-square test
    (`χ² < 3.84 at p=0.05`).
  - Opt-in add / contains / clear.
  - Threaded contention: 4 threads × 1000 OptInBot calls each, expect
    no crashes, expect all opt-ins visible.
- **`test_cooldown.cpp`** (~6 cases):
  - Eligibility before/after expiry.
  - Multiple bots independent.
  - `Set` overwrites.
  - `Clear` empties the map.
- **`test_counters.cpp`** (~5 cases):
  - 10 threads × 1000 increments each = exactly 10000.
  - `DumpToLog()` lines cover all counter names.

Extend **`test_digest_shape.cpp`** with cases for the new fields
(event_log array, social.nearby_humans shape, social.recent_whispers).

### 10.2 Layer 2 — extended manager threading test

`test_manager_threading.cpp` gains:

- **Shadow-mode JSONL records have the right shape.** Run with
  `ApplyMode=shadow`, enqueue, drain, verify `applied_transition == null`
  AND `would_have_applied != null` AND `validator_decision != null`.
- **Cooldown blocks re-enqueue.** Enqueue → drain (sets cooldown) →
  enqueue again immediately → observe second enqueue rejected by
  `cooldown.Eligible(...) == false`.

The "100 requests with 4 workers" core test stays — proves Phase 1's
plumbing didn't regress.

### 10.3 Layer 3 — manual smoke test on Heimdal

Three runs documented in the implementation plan:

1. **`ApplyMode=log`, `SamplePct=10`.** Expect ~10 % of Phase 1's record
   rate. Confirms sampling math + no regressions.
2. **`ApplyMode=shadow`, `SamplePct=10`.** JSONL gets the full
   `validator_decision` distribution. Lets us see whether the LLM's
   proposals would pass before we commit to applying.
3. **`ApplyMode=apply`, `SamplePct=10`.** Watch ONE sampled bot in-game
   for 15 min. Confirm it actually transitions to non-rule-based goals
   (e.g., LLM says `do_quest:502` and the bot heads toward quest 502).
   Compare its trajectory against a non-sampled control bot. No
   worldserver-tick regression.

**Phase 2 done** = unit tests green + extended manager threading test
green + all three Layer 3 runs pass + one bot directly observed picking
+ executing an LLM-driven goal.

### 10.4 Explicitly not tested in Phase 2

- 24-hour stability soak.
- Large-scale (>20 sampled bots) sustained load.
- Memory subsystem paths (Phase 3).
- T2 interactive tool-calling (Phase 4).
- Cloud opt-in (Phase 6).

## 11. Out of scope

- `MemoryHttpClient` / memory sidecar / `memory_hints` population
  (Phase 3).
- `Tier2_Interactive` / tool-calling / `HumanInSocialRangeTrigger`
  (Phase 4). The social opt-in is a *cheap proxy* for the interactive
  tier; the real one comes later.
- `Tier3_ChatBrain` (Phase 5).
- Cloud opt-in router (Phase 6).
- Embedding snap-to-nearest entity grounding (Phase 4+).
- Multi-replica llama-server / load balancing across endpoints.
- Live config reload.
- Prometheus / StatsD metric export.
- Per-player whisper rate limiting.
- DB-persistent opt-in set.
- Shadow-mode "what would rule-based have picked" comparator (deferred
  to Phase 3 pending shadow-data signal).

## 12. Success criteria summary

1. Unit tests: every Phase 1 case still passes + ~50 new cases all
   green (validator + selector + cooldown + counters + extended digest).
2. `Enabled=0`: byte-identical baseline (no LlmAgent code executes).
3. `ApplyMode=log`: zero behavior change versus Phase 1.
4. `ApplyMode=shadow`: zero behavior change AND JSONL has the new
   shadow-only fields populated correctly.
5. `ApplyMode=apply` with `SamplePct=10`: at least one bot directly
   observed transitioning to a non-rule-based goal via the LLM, and
   continuing to execute it via the existing engine.
6. No worldserver-tick regression (mean / p95 within Phase 1
   measurements: ~37 ms / ~77 ms).

When all six hold, Phase 2 is done. Phase 3 (memory sidecar + memory
hints) is the natural next phase per parent design §12.

## 13. Phase 1 follow-ups closed by Phase 2

- ✅ Per-bot cooldown (Phase 1 finding: queue saturation at 1000 bots).
- ✅ Digest enrichment (Phase 1 finding: 100 % `idle` proposals).
- ✅ Per-bot selective enablement (matches user preference for
  selective LLM activation).
- ✅ Validator + apply path (the §12 deliverable).
- ✅ Counters / telemetry (the §12 deliverable).

## 14. Phase 1 follow-ups still open after Phase 2

- `Logger.playerbots = 4` in `worldserver.conf` — operational item, not
  code. Document in the plan's Layer 3 procedure.
- Multi-replica llama-server orchestration — only needed if we want
  `SamplePct > 15`-ish of 1000 bots. Defer until production usage data
  justifies it.
- BIOS Smart Fan profile "Full Speed" on Heimdal — physical access
  required; user-side.
