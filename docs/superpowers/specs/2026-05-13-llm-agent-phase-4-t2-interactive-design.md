# LLM-Agent Phase 4 — T2 Interactive Tool-Calling Design

**Status:** Approved (2026-05-13)
**Parent design:** [`docs/llm_agent_design.md`](../../llm_agent_design.md) §7.2 + §11 + §12
**Predecessors:** Phase 0.5
([Vulkan spec](2026-05-12-llm-agent-phase-0.5-hardware-validation-design.md),
[ROCm spec](2026-05-12-llm-agent-phase-0.5-rocm-comparison-design.md));
Phase 1 plumbing
([spec](2026-05-12-llm-agent-phase-1-plumbing-spike-design.md),
[results](../../../results/2026-05-12-llm-phase-1-smoke/summary.md));
Phase 2 validator + apply
([spec](2026-05-13-llm-agent-phase-2-design.md),
[results](../../../results/2026-05-13-llm-phase-2-smoke/summary.md));
Phase 3 memory sidecar
([spec](2026-05-13-llm-agent-phase-3-memory-sidecar-design.md),
[results](../../../results/2026-05-13-llm-phase-3-smoke/summary.md)).

## 1. Goal

Let the LLM call tools — accept party invites, accept/turn-in quests,
change goals, vendor junk, write structured memories — when a real
human player engages with a bot via whisper, invite, or group join.
Phase 3's smoke run found that memory enrichment alone doesn't move
the LLM's decision posture past 100 % `idle` in a closed bot ecosystem
with zero connected humans. Phase 4 adds the live-human signal that
the LLM needs to make non-idle, action-oriented decisions.

## 2. Phase 4 invariant

T2 never modifies bot state when a sidecar (LLM or memory) is
unreachable, when no tool calls validate, or when the bot isn't
socially opted in. **Disabling `LlmAgent.Tier2.Enabled = 0` reduces
Phase 4 to a no-op overlay on top of Phases 1-3.** T1's Idle replan
path, Phase 2's apply mechanics, and Phase 3's memory recalls all
continue to function exactly as before. `LlmAgent.Enabled = 0` still
returns the binary to byte-identical baseline.

This invariant is what makes Phase 4 safe to merge to a live realm:
the new code is purely additive; turning it off doesn't change the
underlying behavior.

## 3. Scope decisions

Settled during brainstorming (2026-05-13):

| Decision | Choice | Rationale |
|---|---|---|
| Tool catalog scope | **7 tools, no chat:** `accept_party_invite`, `leave_party`, `accept_quest`, `turn_in_quest`, `set_goal`, `vendor_junk`, `memory.remember` | Covers parent design §7.2 minus chat-routed tools (which belong to Phase 5's T3). Skips rarely-fired tools like `abandon_quest` and `train_skills`. |
| Trigger surface | **Any pending interaction event** (pending invite OR recent whisper from real player OR recent group join) | No proximity check. Simpler than the parent design's `HumanInSocialRangeTrigger`. The Phase 2 social opt-in already gates eligibility; Phase 4 only adds the per-event trigger. |
| Multi-tool responses | **Cap at 3 per response, applied sequentially** | LLM commonly emits causal sequences ("accept invite, then set goal to follow"). 3 is enough for the realistic interactions we expect. |
| Failure-mode for partial multi-tool | **No rollback** | Successfully accepting an invite is a useful outcome even if a downstream tool fails. Unwinding would create more weirdness than it solves. |
| `memory.remember` rejection | **Doesn't halt the sequence** | Memory is non-critical; other tools should still apply if the sidecar is down. |
| Entity resolution | **Name-based, resolved server-side** | LLM speaks in natural references ("RealPlayerBob", "Innkeeper"). Validators scan `nearby_humans[]` / `nearby_creature_guids[]` to resolve names → GUIDs. Aligns with the design's "soft-grounding" stance. |
| Personality cards | **Stay stubbed in Phase 4** | LLM-generated cards still deferred. Phase 4 ships only the interaction surface. |

## 4. Architecture overview

T2 lives as a sibling tier to T1 inside the existing `LlmAgent`
module. New components:

```
src/Bot/LlmAgent/
  [Phase 1-3 unchanged]
  Tiers/
    Tier2_Interactive.h/cpp       T2 digest + request builder
  Triggers/
    LlmInteractTrigger.h/cpp      fires on pending interaction events
  Actions/
    LlmInteractAction.h/cpp       drains + applies tool calls
  Tools/
    ToolCatalog.h/cpp             7-tool JSON Schema + ParseToolCalls
    ToolValidators.h/cpp          per-tool validation (pure)
    ToolExecutors.h/cpp           worldserver-side apply
  EventBuffer/
    InteractionEventBuffer.h/cpp  pending invite + recent whisper + group join
```

**Critical invariants:**

- T2 shares the existing `LlmAgentManager` worker pool. No new HTTP
  server, no new thread pool.
- T2 uses llama.cpp's OpenAI-compatible tool-calling
  (`tool_choice="auto"`, `tools[]` array). Response shape differs from
  T1 (parse `choices[0].message.tool_calls`).
- Tool execution is **worldserver-thread only**, same as Phase 2's
  apply path. Workers parse + validate; the action applies.
- **All tool calls validate before any apply.** If multiple come back
  and the first fails validation, none apply.
- T2 never modifies state when the bot isn't socially opted in.
- T1 and T2 are **mutually exclusive per tick** via relevance: T2 at
  16.0f beats T1 at 15.0f when both are active.

## 5. Components

### 5.1 `Tier2_Interactive.h/cpp`

Owns the T2-specific digest + request body construction.
`BuildT2Digest(PlayerbotAI*)` reuses
`Tier0_StateDigest::SnapshotBot` for the base digest, then enriches
with **pending interaction context**:

```cpp
struct InteractionPayload {
    std::vector<PendingInvite>     pending_invites;
    std::vector<UnhandledWhisper>  recent_whispers;    // last <WindowSeconds>
    std::vector<RecentGroupJoin>   recent_group_joins; // last <WindowSeconds>
};
```

The digest's user message is structured: base state JSON plus the
interaction payload. Request body uses `tools[]` instead of
`response_format`. `tool_choice = "auto"`.

### 5.2 `Triggers/LlmInteractTrigger.h/cpp`

```cpp
class LlmInteractTrigger : public Trigger {
  public:
    LlmInteractTrigger(PlayerbotAI* ai) : Trigger(ai, "llm interact") {}
    bool IsActive() override;
};
```

`IsActive()` returns true when **all**:
- `LlmAgent.Enabled && Tier2.Enabled`
- `BotSelector::IsLlmBot(guid)` (sample or opted-in)
- `Cooldowns().Eligible(guid)`
- `!IsInFlight(guid, tier=T2)` — separate in-flight tracking per tier
- `InteractionEventBuffer::HasPending(guid)` — pending invite OR
  unhandled whisper OR recent group join

Relevance 16.0f outranks T1's 15.0f. When pending interactions exist,
T2 wins the tick. When none exist, T1 fires normally.

### 5.3 `Actions/LlmInteractAction.h/cpp`

```cpp
class LlmInteractAction : public Action {
  public:
    LlmInteractAction(PlayerbotAI* ai) : Action(ai, "llm interact") {}
    bool Execute(Event event) override;
};
```

Same drain+enqueue pattern as Phase 2's `LlmReplanIdleAction`. Drain
runs each tool call through `ToolValidators::Validate(call)`, then
`ToolExecutors::Apply(call, botAI)` for accepted calls. Stops on
first validation failure (preserves causal sequencing). Returns true
when any tool was applied (suppresses sibling T1 action this tick).

### 5.4 `Tools/ToolCatalog.h/cpp`

`kToolsJsonSchema`: a JSON array containing 7 OpenAI function-call
schemas. Plus `ParseToolCalls(const std::string& raw_response) ->
std::variant<vector<ParsedToolCall>, ParseError>`. Mirrors Phase 1's
`Schemas/Goal.h/cpp` pattern.

```cpp
struct AcceptPartyInviteCall  { std::string from; };
struct LeavePartyCall         { };
struct AcceptQuestCall        { uint32_t quest_id; std::string from_npc_name; };
struct TurnInQuestCall        { uint32_t quest_id; std::string to_npc_name; };
struct SetGoalCall            { ParsedGoal goal; };   // reuses Phase 1's type
struct VendorJunkCall         { std::string vendor_npc_name; };
struct MemoryRememberCall {
    std::string text;
    std::vector<std::string> entities;
    double salience;
    std::vector<std::tuple<std::string, std::string, std::string>> relations;
};

using ParsedToolCall = std::variant<
    AcceptPartyInviteCall, LeavePartyCall, AcceptQuestCall,
    TurnInQuestCall, SetGoalCall, VendorJunkCall, MemoryRememberCall
>;
```

### 5.5 `Tools/ToolValidators.h/cpp`

Pure function per tool: `Validate<Tool>(const <Tool>Call&, const
InteractionContext&) -> ValidationResult`. Same shape as Phase 2's
`GoalValidator`. `InteractionContext` is the POD-only payload the
validators need (pending invites, quest log, nearby NPCs, nearby
humans, group membership, bot level, map_id).

Per-tool rules (full table in §6 below).

### 5.6 `Tools/ToolExecutors.h/cpp`

Worldserver-side apply functions. One `Apply<Tool>(...)` per tool.
Each wraps in try/catch with Idle recovery. Guarded by
`#ifndef LLMAGENT_UNIT_TESTS`. Calls existing AzerothCore primitives
(`bot->RemoveFromGroup()`, `bot->AddQuest(quest, npc)`, etc.).

### 5.7 `EventBuffer/InteractionEventBuffer.h/cpp`

Per-bot sliding window tracking three event types:

```cpp
struct PendingInvite     { std::string from_name; uint64_t from_guid; int64_t ts; };
struct UnhandledWhisper  { std::string from_name; uint64_t from_guid;
                           std::string text; int64_t ts; };
struct RecentGroupJoin   { std::string leader_name; uint64_t leader_guid; int64_t ts; };

class InteractionEventBuffer {
  public:
    void PushWhisper(uint64_t bot_guid, /*...*/);
    void PushInvite (uint64_t bot_guid, /*...*/);
    void PushJoin   (uint64_t bot_guid, /*...*/);
    bool HasPending(uint64_t bot_guid) const;
    InteractionPayload SnapshotFor(uint64_t bot_guid);
    void ExpireOlderThan(uint64_t bot_guid, int64_t window_seconds);
};
```

Hooks:
- `LlmAgentHooks::OnWhisperReceived` (existing Phase 2/3) — also
  pushes to `InteractionEventBuffer.PushWhisper`.
- `LlmAgentHooks::OnPartyInviteReceived` (NEW) — pushes
  `PushInvite`. Called from the AC party-invite event path.
- `LlmAgentHooks::OnGroupJoined` (NEW) — pushes `PushJoin`. Called
  when the bot's group membership changes.

### 5.8 `LlmAgentConfig` extensions

| Key | Default | Description |
|---|---|---|
| `AiPlayerbot.LlmAgent.Tier2.Enabled` | `1` | Sub-gate under master `LlmAgent.Enabled`. |
| `AiPlayerbot.LlmAgent.Tier2.MaxToolsPerResponse` | `3` | Hard cap on tool calls applied per response. |
| `AiPlayerbot.LlmAgent.Tier2.WhisperWindowSeconds` | `120` | Whispers older than this don't trigger T2. |
| `AiPlayerbot.LlmAgent.Tier2.SystemPrompt` | `(constexpr default)` | T2 system prompt (tool-calling-optimized). |

### 5.9 `LlmCounters` extensions

New atomic buckets (one per tool):

- `tool_call_received_<name>` — 7 buckets, ++ on each parsed tool call.
- `tool_call_applied_<name>` — 7 buckets, ++ on successful apply.
- `tool_call_rejected_<reason>` — keyed by reject_reason string (map,
  same shape as Phase 2's `rejected_by_reason`).
- `tool_call_threw_<name>` — 7 buckets, ++ on apply exception.
- `tool_call_no_action` — model returned 0 tool calls (correct
  "nothing to do" outcome).
- `tool_call_truncated` — model returned >MaxToolsPerResponse.
- `tool_call_schema_error` — `ParseToolCalls` returned ParseError.

## 6. Tool catalog: schemas + validator rules

Each tool follows OpenAI's function-call schema. Full
`kToolsJsonSchema` is one JSON array.

### 6.1 `accept_party_invite`

Params: `from: string` (inviter's character name).
**Validator:** `from` matches a name in `ctx.pending_invites[]`.
Rejects: `rejected_no_pending_invite`, `rejected_invite_from_unknown`.

### 6.2 `leave_party`

Params: none.
**Validator:** bot is currently in a group. Reject:
`rejected_not_in_group`.

### 6.3 `accept_quest`

Params: `quest_id: uint32`, `from_npc_name: string`.
**Validator:** `quest_id` exists in DB; quest is level-appropriate;
bot does NOT already have it; named NPC is the quest giver AND in
range (≤ 10 yards). Rejects: `rejected_unknown_quest`,
`rejected_level_inappropriate`, `rejected_quest_already_in_log`,
`rejected_npc_not_in_range`, `rejected_npc_not_quest_giver`.

### 6.4 `turn_in_quest`

Params: `quest_id`, `to_npc_name`.
**Validator:** quest is in log AND status = complete; named NPC is
the quest's turn-in target AND in range. Rejects:
`rejected_quest_not_in_log`, `rejected_quest_not_complete`,
`rejected_npc_not_in_range`, `rejected_npc_not_turn_in_target`.

### 6.5 `set_goal`

Params: same shape as T1's `BotGoal` schema.
**Validator:** reuses Phase 2's `GoalValidator::ValidateGoalDecision`.
Zero new validator code; this tool lets the LLM force a goal change
in response to a human signal instead of waiting for Idle.

### 6.6 `vendor_junk`

Params: `vendor_npc_name`.
**Validator:** NPC is a vendor; bot is in interaction range. Rejects:
`rejected_npc_not_vendor`, `rejected_npc_not_in_range`.

### 6.7 `memory.remember`

Params: `text` (≤ 500 chars), `entities[]`, `salience` (`[0, 1]`),
`relations[]`.
**Validator:** text length OK; salience in range. **No entity
existence check** — memory is the LLM's loose semantic store.
Rejects: `rejected_text_too_long`, `rejected_salience_out_of_range`.
Apply: calls `MemoryHttpClient::Remember` with the params.

## 7. Data flow

```
RealPlayerBob whispers Grimblade "come help me with Defias near Sentinel Hill"
  │
  ├─→ AC chat-receive → PlayerbotAI::HandleCommand
  │     └─→ LlmAgentHooks::OnWhisperReceived(grimblade, RealPlayerBob, "...")
  │           ├─→ RecentEventBuffer.event_log (Phase 2)
  │           ├─→ Selector.OptInBot(grimblade) (Phase 2)
  │           ├─→ MemoryClient.Remember (Phase 3)
  │           └─→ InteractionEventBuffer.PushWhisper (Phase 4 — NEW)
  │
  └─→ next tick that touches Grimblade
        ├─→ LlmInteractTrigger.IsActive() → true (pending whisper)
        └─→ LlmInteractAction::Execute()
              1. Drain T2 results (empty first time)
              2. digest = BuildT2Digest(grimblade)  // base + interaction payload
              3. body = T2 OpenAI request (tools[], tool_choice=auto)
              4. mgr.Enqueue(LlmRequest{tier=T2, body, digest})
              5. Return false

(worker thread, ≈ 50 ms)
  POST /v1/chat/completions → llama-server

(2-4 s later)
  llama-server returns:
    choices[0].message.tool_calls = [
      {name:"set_goal", arguments:"{...go_camp near Bob...}"}
    ]
  Worker:
    - ParseToolCalls → vector<ParsedToolCall>
    - Push LlmResult{tier=T2, tool_calls=[...]} to per-bot stack
    - JSONL line written

(next bot tick)
  LlmInteractAction::Execute()
    1. Drain returns the T2 result
    2. For each tool call (cap MaxToolsPerResponse, ordered):
         a. ctx = SnapshotInteractionContext(botAI)
         b. decision = ToolValidators.Validate(call, ctx)
         c. if accepted: ToolExecutors.Apply(call, botAI)
                         applied_any = true
                         counters.IncToolApplied(name)
         d. else:        counters.IncToolRejected(name, reason)
                         break  // stop on first reject
    3. Cooldowns.Set based on results
    4. Return applied_any (true → suppresses T1 status_update)

Grimblade transitions to go_camp via LLM-dispatched ChangeToGoCamp
toward Bob's vicinity. RealPlayerBob sees him head over.
```

**Three load-bearing properties:**

1. **The whisper hook is the T2 entry point.** Without
   `InteractionEventBuffer.Push`, T2 never fires.
2. **Multi-tool sequence is "stop on first failure, no rollback."**
   If call #1 succeeds and call #2 fails, the bot keeps call #1's
   effect. Correct semantics (accepting an invite is a useful outcome
   on its own).
3. **T2 and T1 are mutually exclusive per tick** via relevance 16 > 15.
   T1 fires on the next tick if no T2 interactions are pending.

## 8. Error handling

(See §5 above for the matrix. Summary: empty `tool_calls` is correct
"no-action," validator rejection halts the sequence,
`memory.remember` rejection is the one exception that doesn't halt,
and tool throws reset to Idle.)

## 9. Testing strategy

### 9.1 Layer 1 — C++ unit tests (~50 new cases)

- `test_tool_catalog.cpp` (~6): kToolsJsonSchema shape, parse cases.
- `test_tool_parser.cpp` (~8): happy + malformed + unknown tool +
  empty + truncated + missing args.
- `test_tool_validators.cpp` (~30): one happy + 2-3 rejects per tool.
- `test_interaction_buffer.cpp` (~6): push/expire/snapshot per event
  type; per-bot isolation; 120-s window enforcement.

Total: 93 (Phase 3) + ~50 = **~143 C++ unit cases.**

### 9.2 Layer 2 — Extend `test_manager_threading.cpp` (2 new)

- T2 request shape: stub server intercepts the POST body, verifies
  `tools[]` present and `tool_choice == "auto"`.
- T2 + T1 mutual exclusion: both triggers IsActive → only T2 fires.

### 9.3 Layer 3 — Heimdal smoke (real-human demo)

1. Worldserver built from this branch. `LlmAgent.Enabled=1`,
   `Tier2.Enabled=1`, `SamplePct=10`, `ApplyMode=apply`.
2. Sidecar + llama-embed up.
3. Real player logs in.
4. Pick a sampled bot. Whisper something action-oriented: *"come
   group up at SW gates"* or *"want to party?"*
5. Within ≈ 5 s a Tier=2 JSONL record appears with `tool_calls`
   populated.
6. Bot moves / accepts / vendors as appropriate.

**Phase 4 done** = Layers 1 + 2 green AND Layer 3 produces at least
one observed tool call applied successfully from a real-human-driven
trigger, counters show `tool_call_applied_* >= 1`, and the bot's
`NewRpgInfo` reflects the LLM-dispatched change.

### 9.4 Explicitly not tested in Phase 4

- LLM-generated personality cards.
- T3 chat brain (Phase 5).
- 24-hour soak under sustained T2 load.
- Cloud opt-in router (Phase 6).

## 10. Out of scope

- LLM-generated personality cards (Phase 4+).
- T3 chat brain / `say` / `whisper` tools (Phase 5).
- Cloud opt-in router (Phase 6).
- Embedding-snap entity grounding (Phase 4+).
- `train_skills`, `invite_to_party`, `abandon_quest` (deferred per
  scope choice).
- Multi-shard llama-server orchestration.
- Persisted T2 in-flight / cooldown state across restarts.
- Tool execution rollback on partial multi-tool failure (intentional
  non-feature per §5).

## 11. Success criteria summary

1. C++ tests: ≈ 143 / 143 (Phase 3's 93 + ~50 Phase 4).
2. Python sidecar tests still 45 / 45 (unchanged from Phase 3).
3. `Tier2.Enabled = 0`: zero behavior change from Phase 3.
4. `Enabled = 0`: byte-identical baseline.
5. **At least one observed end-to-end:** real human whispers /
   invites a sampled bot → T2 fires → tool validates + applies →
   bot acts. Logged in JSONL with `tool_calls.applied >= 1`.
6. No worldserver-tick regression: mean ≤ 20 ms, p95 ≤ 50 ms.

When all six hold, Phase 4 is done. Phase 5 (T3 chat brain) is the
natural next phase per parent design §12.

## 12. Phase 3 follow-ups closed by Phase 4

- ✅ Live human signal → digest. T2's `InteractionPayload` carries
  pending invites + recent whispers directly into the LLM's input.
- ✅ Tool-driven non-idle decisions. `set_goal` from T2 closes
  Phase 3's "100 % idle" finding when a human is present.
- ✅ LLM-driven memory writes via `memory.remember` tool.
- ⚠️ Quest accept / complete / turn-in writers: Phase 4 ships
  `accept_quest` and `turn_in_quest` as tools (LLM-initiated), but
  the C++ side does NOT yet auto-write a memory when a bot
  rule-based accepts/turns-in a quest. Phase 4.1 follow-up.

## 13. Open items deferred to Phase 5

- T3 chat brain: bots that actually speak back to humans, not just
  emit tool calls.
- LLM-generated personas (still stubbed).
- Tool-call audit admin command for in-game inspection.
- Multi-replica llama-server for high-T2-load scenarios.
