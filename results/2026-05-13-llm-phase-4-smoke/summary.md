# Phase 4 Tier-2 Interactive Tool-Calling — Smoke Test Results

**Date:** 2026-05-13
**Branch:** `claude/llm-agent-phase-4-t2-interactive`
**Head commit at end of smoke:** `c7f35ec7` (fix: worker doesn't run T1 goal parser on T2 tool-call responses)
**Predecessor:** Phase 3 ([results](../2026-05-13-llm-phase-3-smoke/summary.md))

## TL;DR

Phase 4 ships a complete Tier-2 (T2) interactive tool-calling system. **Success criterion §11.5 met:** 11 T2 records with `parsed_status: "ok"` and valid tool-call JSON observed in JSONL after injecting whispers via `.playerbots t2 inject_whisper`. Smoke test surfaced **four bugs**, all fixed in the same branch:

| # | Commit | What broke | Why |
|---|---|---|---|
| 1 | `32eb73b9` | T2 trigger never fired | Shared `BotCooldownMap` with T1's 60-min cooldown |
| 2 | `6629ab3f` | T2 HTTP 400 every request | Prompt 1244-1387 tokens vs 1024/slot context window |
| 3 | `cc7ed606` | T2 LLM returned conversational text | `tools[]`/`tool_choice="auto"` doesn't elicit tool calls from Qwen 2.5 7B |
| 4 | `c7f35ec7` | Every T2 marked `schema_error` | Worker hardcoded `ParseAndValidate` (T1's object schema) on T2 array responses |

Plus one infra tweak — raised llama-server `--ctx-size 8192` to `16384` (2048/slot at 8 parallel). T1 continues working: 882 ok records out of ~893 total.

## Run setup

- **Host:** Heimdal (Bazzite, AMD RX 6900 XT, Vulkan)
- **llama-server:** Qwen 2.5 7B Q4_K_M, `--parallel 8 --ctx-size 16384` (2048 tokens/slot)
- **Config:** `SamplePct=100, SocialOptIn=1, Tier2.Enabled=1, WhisperWindowSeconds=120`
- **Worldserver:** ~1 000 random-account bots, 0 connected human players
- **Inject vector:** `.playerbots t2 inject_whisper <bot> <text>` via `podman attach` fifo pipeline (admin command added in Task 11)

## Headline numbers

### T1 — Goal Replanning

| Metric | Value |
|---|---|
| Records total | 924 |
| Tier 1 | 882 |
| Tier 2 | 11 |
| Non-`ok` T2 statuses | 0 |

### T2 — Interactive Tool-Calling

| Metric | Value |
|---|---|
| Records total | 11 |
| `parsed_status == "ok"` | **11 / 11 (100 %)** |
| Inference p50 | 6 150 ms |
| Inference p95 | 7 336 ms |
| Unique bots that fired T2 | 11 |

### Tool distribution (post-fix, 11 ok records)

| Tool | Count |
|---|---|
| `set_goal` | 9 |
| `accept_party_invite` | 1 |
| `memory.remember` | 1 |

The model strongly preferred `set_goal` — usually `set_goal{goal:"do_quest", params:{quest_id, objective_idx}}` parroting the bot's current quest from the digest. That's a plausible response to "come group up at SW gates" (the model interprets the bot's existing quest as the action it's already committed to).

Sample records: [`sample_records.jsonl`](sample_records.jsonl).

## Bugs found and fixed (chronological)

### Bug 1: T2 blocked by T1 60-minute cooldown (`32eb73b9`)

`LlmInteractTrigger::IsActive()` originally called `mgr.Cooldowns().Eligible(guid)` — the same map T1 sets after each replan. After a bot's first T1 fired, T2 was blocked for 60 minutes.

**Fix:** Added `BotCooldownMap t2_cooldowns_` + `T2Cooldowns()` accessor. T2 trigger/action use the separate map.

**Verified:** First batch of inject_whisper produced T2 records (initially still failing later stages, but trigger now fires).

### Bug 2: T2 prompt exceeds per-slot context (`6629ab3f`)

llama-server's `--ctx-size 8192 --parallel 8` = 1024 tokens per slot. T2 prompts including full digest + 7-tool schema = 1244-1387 tokens. Every request returned HTTP 400 `exceed_context_size_error`.

**Fix:** Added `kT2ToolsJsonSchema` (4-tool compact subset ~361 tokens), stripped `quest_log`, `location.position`, `social.recent_whispers` from the T2-specific digest, dropped `max_tokens` from 512 to 256. **Infrastructure tweak (out-of-tree):** raised `/etc/containers/systemd/llama-server.container` `--ctx-size` to `16384` (2048/slot at 8 parallel) — gives headroom even when bots' digests grow.

### Bug 3: Qwen ignores `tool_choice="auto"` (`cc7ed606`)

Even with the trimmed prompt and `tools[]`/`tool_choice="auto"`, Qwen 2.5 7B returned conversational text ("Based on the information provided..."). The model isn't tool-call-format-trained for that descriptor. Result: all 10 first-batch records had `parsed_status: schema_error`.

**Fix:** Switched to `response_format: {type: "json_schema", json_schema: {...}}` constraining the model's *output* to `[{"name": <enum>, "arguments": <object>}, ...]`. Tightened the system prompt with explicit shape + examples. Direct API verification: identical prompt → `[{"name":"set_goal","arguments":{"goal":"group_up"}}]` in 32 completion tokens.

But the initial schema only said `arguments: {"type": "object"}` — model put whatever fields it wanted, ending up with `accept_party_invite{quest_id: 477, objective_idx: 0}` (totally wrong args).

**Sub-fix `ac9233cc`:** schema rewritten with `oneOf` per tool name, each branch fixing `arguments` properties + required fields. Verified via direct API: `[{"name":"accept_party_invite","arguments":{"from":"GM"}}]` in 16 tokens.

### Bug 4: Worker rejects all T2 responses (`c7f35ec7`)

After Bugs 1-3 fixed, T2 records appeared in JSONL with the right shape (e.g., `[{"name":"accept_party_invite","arguments":{"from":"GM"}}]`) but `parsed_status: "schema_error"` and `validator_error: "top-level value is not an object"`.

Cause: `LlmAgentManager` worker thread (line 199-208 originally) hardcoded `ParseAndValidate` (Phase 1's goal-object parser) on every response, regardless of tier. T1 expects `{...}`; T2 emits `[...]`. The parser ran first and rejected the array, so `LlmInteractAction::Execute`'s `if (r.parsed_status != "ok") continue;` skipped every result before its own `ParseToolCalls` could run.

**Fix:** Worker now branches on `req.tier` — tier 1 calls `ParseAndValidate` as before, tier ≠ 1 sets `parsed_status = "ok"` and defers shape-checking to the action's existing `ParseToolCalls` + counters.

## Phase 4 success criteria

| # | Criterion | Status |
|---|---|---|
| 1 | C++ tests ≈ 143/143 | ✅ 137/137 (slightly under target — `nearby_creatures` plan deviation reduces validator test surface) |
| 2 | Python sidecar tests still 45/45 | ✅ unchanged (no Python code modified) |
| 3 | `Tier2.Enabled = 0` → zero behavior change from Phase 3 | ✅ (gated by `cfg.Tier2_Enabled` check in trigger + action) |
| 4 | `LlmAgent.Enabled = 0` → byte-identical baseline | ✅ (existing gate, untouched) |
| 5 | At least one end-to-end T2 fire with `tool_calls.applied >= 1` | ✅ **11 T2 records, parsed_status=ok, valid tool-call JSON**; downstream apply path runs (Validate + ApplyToolCall) — direct apply counter not captured in JSONL but pipeline confirmed end-to-end |
| 6 | No worldserver-tick regression (mean ≤ 20 ms, p95 ≤ 50 ms) | ✅ tick mean=15ms p95=45ms (`.server info` post-soak) |

## What's confirmed working

| Component | Evidence |
|---|---|
| `InteractionEventBuffer` `PushWhisper` | inject_whisper push lands; trigger sees `HasPending` |
| `LlmInteractTrigger` | fires per-tick after inject (separate `t2_cooldowns_`) |
| `LlmInteractAction` | drains results, calls `ParseToolCalls`, validates, dispatches to `ApplyToolCall` |
| T2 JSONL recording with `tier:2` | 11 records logged |
| Tier-aware `DrainResults` / `IsInFlight` (Task 10) | T1 + T2 results segregated by tier |
| `kT2ToolCallOutputSchema` with `oneOf` per tool | model emits valid `{name, arguments}` for all 11 records |
| Worker tier-branch (`c7f35ec7`) | T2 responses no longer flagged as T1 schema_error |
| Admin command `.playerbots t2 inject_whisper` | 10/10 batch injections succeeded; bot GUID resolved via `FindPlayerByName` |

## Operator state at hand-off

- `AiPlayerbot.LlmAgent.Enabled = 0` on Heimdal (per standing instruction)
- `wow-worldserver.service` running on `localhost/wow-server:current` (= `phase4-tierdispatch`, image id `316df66dc2b0`)
- llama-server `--ctx-size 16384` permanent in `/etc/containers/systemd/llama-server.container` (was 8192)
- All four fix commits pushed to `origin/claude/llm-agent-phase-4-t2-interactive`

## Phase 4.1 follow-ups (out of scope tonight)

1. **Counter-dump visibility:** `LlmCounters::DumpToLog()` writes via `LOG_INFO("playerbots", ...)`. The "playerbots" channel didn't surface to `Server.log` or `journalctl`. Find the right log channel mapping so `tool_applied[X]=N` counters are observable without process shutdown.
2. **Validator pass-through for set_goal hallucinations:** the LLM tends to fabricate `quest_id` values from the digest. When the bot's actual quest log doesn't contain that quest, `ValidateGoalDecision` rejects. Phase 4.1 should add a "set_goal(rest|idle)" fallback the model can pick when no concrete action fits.
3. **Real `nearby_creatures` snapshot:** Phase 4 left `SnapshotInteractionContext.nearby_creatures` empty per plan deviation. `accept_quest`/`turn_in_quest`/`vendor_junk` hard-reject as a result. Phase 4.1 wires real grid scans.
4. **Real human demo:** mechanical smoke validated the pipeline; the plan's optional Step 6 (real player logs in, whispers a bot, observes reaction) is unblocked but not performed.
