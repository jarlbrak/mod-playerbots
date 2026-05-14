# Phase 4 Tier-2 Interactive Tool-Calling — Smoke Test Results

**Date:** 2026-05-13
**Branch:** `claude/llm-agent-phase-4-t2-interactive`
**Head commit:** `6629ab3f` (fix: trim T2 prompt to fit 1024-token per-slot context)
**Predecessor:** Phase 3 ([results](../2026-05-13-llm-phase-3-smoke/summary.md))

## TL;DR

Phase 4 ships a complete Tier-2 (T2) interactive tool-calling system. The trigger path, buffer wiring, worker dispatch, and JSONL recording all work correctly. Two bugs were found and fixed during this smoke run:

1. **Shared cooldown block (fixed `32eb73b9`):** T2 trigger was using the same `BotCooldownMap` as T1. After T1 replans, it sets a 60-minute cooldown that blocked T2 from ever firing. Fixed by adding a separate `t2_cooldowns_` member.

2. **Context-window overflow (fixed `6629ab3f`):** The llama-server runs `--ctx-size 8192 --parallel 8`, giving 1024 tokens per slot. T2 prompts totaled 1244-1387 tokens. Fixed by introducing `kT2ToolsJsonSchema` (4-tool compact subset, ~361 tokens) and stripping non-social fields from the T2 digest. Resulting prompts are ~680-800 tokens.

**T2 end-to-end confirmation status:** T2 trigger and dispatch confirmed working (19 records in JSONL). Full T2 LLM success requires deploying build `6629ab3f` (needs interactive sudo on Heimdal). A direct API test against the compact schema succeeded with 794 prompt tokens and returned a valid `memory.remember` tool call.

T1 continues working: 1 476 ok out of 1 482 total (99.6%).

## Run setup

- **Host:** Heimdal (Bazzite, AMD RX 6900 XT)
- **llama-server:** Vulkan, Qwen 2.5 7B Q4_K_M, `--parallel 8 --ctx-size 8192` (1024 tokens/slot)
- **Config:** `SamplePct=100, SocialOptIn=1, Tier2.Enabled=1, WhisperWindowSeconds=120`
- **Worldserver:** ~1 000 random-account bots, 0 connected human players

## Headline numbers

### T1 — Goal Replanning

| Metric | Value |
|---|---|
| Records total | 1 482 |
| `parsed_status == "ok"` | 1 476 / 1 482 (99.6 %) |
| Goal distribution | 1 476 x `idle` |
| Inference p50 | 5 188 ms |
| Inference p95 | 9 513 ms |
| Queue wait mean | 338 458 ms (~5.6 min) |

### T2 — Interactive Tool-Calling

| Metric | Value |
|---|---|
| Records total | 19 |
| `parsed_status == "ok"` | 0 (blocked by context overflow) |
| `parsed_status == "http_error"` | 19 (400: exceed_context_size_error) |
| n_prompt_tokens observed | 1 244 – 1 387 |
| n_ctx (slot limit) | 1 024 |
| Unique bots triggered | 19 |

**Direct API test (post-fix, compact schema):**

| Metric | Value |
|---|---|
| Prompt tokens | 794 |
| Response | 1 tool call: `memory.remember` |
| Status | 200 OK |

Sample records: [`sample_records.jsonl`](sample_records.jsonl).

## Bugs found and fixed

### Bug 1: T2 blocked by T1 60-minute cooldown (fixed `32eb73b9`)

`LlmInteractTrigger::IsActive()` called `mgr.Cooldowns().Eligible(guid)` — the same map used by T1's replan action, which sets a 60-minute cooldown after replanning. Any T2 injection was blocked.

**Fix:** Added `BotCooldownMap t2_cooldowns_` + `T2Cooldowns()` accessor. T2 trigger/action now use the separate map.

**Verification:** Batch whisper injection immediately produced 19 T2 requests — trigger fires independently of T1.

### Bug 2: T2 prompt exceeds per-slot context (fixed `6629ab3f`)

llama-server splits `--ctx-size 8192` across `--parallel 8` slots: 1024 tokens each. T2 prompts with full tool catalog + digest totaled 1244-1387 tokens.

**Fix:**
- `kT2ToolsJsonSchema`: compact 4-tool subset (~361 tokens vs 702 for full set)
- `BuildT2RequestBody` strips `quest_log`, `location.position`, `social.recent_whispers`
- `max_tokens` reduced from 512 to 256

**Verification:** Direct API call with compact schema returned 200 OK, 794 prompt tokens, `memory.remember` tool call.

## Confirmed working components

| Component | Status | Evidence |
|---|---|---|
| `InteractionEventBuffer` PushWhisper | OK | Whispers appear in T2 digest `interaction_context` |
| `InteractionEventBuffer` ExpireOlderThan | OK | WhisperWindowSeconds=120 enforced |
| `LlmInteractTrigger` | OK | Fires when pending interactions exist |
| `LlmInteractAction` enqueue | OK | 19 T2 requests dispatched to worker queue |
| T2 JSONL recording | OK | All 19 records have `"tier": 2`, correct interaction_context |
| Admin command inject_whisper | OK | `.playerbots t2 inject_whisper <bot> <text>` works |
| Separate t2_cooldowns_ | OK | T2 fires independently of T1 60-min cooldown |
| kT2ToolsJsonSchema | OK | Direct API test: 794 tokens, tool call returned |
| Tier-aware DrainResults | OK | T1/T2 results correctly segregated by tier |
| JSONL tier field | OK | Was null before fix; now correct `1` or `2` |

## What needs rebuild + restart to confirm

| Item | Status |
|---|---|
| T2 ok records in JSONL | Pending deploy of 6629ab3f |
| Tool call application (set_goal, accept_party_invite, etc.) | Code complete, not yet in production |

Build of `6629ab3f` in progress at `/tmp/worldserver-build-phase4b.log` on Heimdal.
Deploy requires: `sudo podman tag` + `systemctl restart wow-worldserver` (needs interactive sudo).

## Operator state at hand-off

- `AiPlayerbot.LlmAgent.Enabled = 1` on Heimdal — **turn off** before walking away
- Build in progress (rootless podman, ~34% complete when this was written)
- To confirm T2 end-to-end: deploy 6629ab3f, inject fresh whispers, watch JSONL for `"tier": 2, "parsed_status": "ok"`

## Next

Full T2 success confirmation + deploy. After that: Phase 5 (or production hardening — increase llama-server per-slot context to 2048+, e.g. `--ctx-size 16384 --parallel 8`).
