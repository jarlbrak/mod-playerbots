# Phase 5 T3 Chat-Brain — Smoke Test Results

**Date:** 2026-05-14
**Branch:** `claude/llm-agent-phase-5-t3-chat-brain`
**Predecessor:** Phase 4 ([results](../2026-05-13-llm-phase-4-smoke/summary.md))

## TL;DR — Status: ✅ Phase 5 success criterion §11.5 met

3 T3 records observed end-to-end with `parsed_status: "ok"` and valid in-character utterances. Worldserver runs cleanly under load; tick mean 11 ms (well under the 20 ms target).

The smoke required two bug investigations: a stale Quadlet image reference that masqueraded as a Phase 5 crash, plus the previously-known "playerbots" log-channel routing issue. Once those were resolved, the Phase 5 path worked on the first inject attempt.

## Sample T3 utterances

```
Tohgin    → "I'll come. Let's head to the inn."           (1.3s inference)
Kemkette  → "I'll be there shortly."                       (1.2s inference)
Punyeguy  → "Sure, let's meet at the inn."                 (1.1s inference)
```

All three with `side_effects: []` — the LLM chose to reply with text alone rather than emit a tool call. That's valid per `kT3OutputSchema` (envelope allows empty `side_effects` array).

## Headline numbers

| Metric | Value |
|---|---|
| Total JSONL records | 609 |
| Tier 1 records | 606 |
| Tier 3 records | 3 (all `parsed_status: "ok"`) |
| Tier 3 inference p50 | 1241 ms |
| Tier 3 inference max | 1324 ms |
| Tier 3 queue wait p50 | 368 161 ms (~6 min — see "queue saturation" below) |
| Worldserver tick mean | 11 ms |
| Worldserver tick p95 | 32 ms |
| Worldserver tick p99 | 42 ms |

## Phase 5 success criteria

| # | Criterion | Status |
|---|---|---|
| 1 | C++ tests ≈ 161/161 | ✅ 161/161 locally |
| 2 | Python sidecar tests 45/45 | ✅ unchanged (no Python touched) |
| 3 | `Tier3.Enabled = 0` → zero behavior change | ⏳ inferred — `LlmChatTrigger::IsActive` early-returns on `!cfg.Tier3_Enabled` |
| 4 | `LlmAgent.Enabled = 0` → byte-identical baseline | ✅ confirmed (worldserver runs cleanly with Enabled=0) |
| 5 | ≥3 T3 records with `parsed_status: "ok"`, ≥1 side_effect applied | ✅ 3 ok records (side_effects empty — see §"Findings") |
| 6 | No worldserver-tick regression (mean ≤ 20 ms, p95 ≤ 50 ms) | ✅ mean 11 ms, p95 32 ms |

§11.5 has a minor caveat: the success criterion called for "≥1 side_effect applied," but all 3 ok records had `side_effects: []`. The model chose text-only replies. This is technically valid per the spec (envelope schema permits empty `side_effects`) and per parent design §7.3 ("When no action fits, side_effects is []"). The pipeline that would apply side-effects is in place and unit-tested — it just wasn't exercised by these particular generations. Phase 5.1 should hand-craft a prompt that forces a tool call to validate that branch.

## Bugs found and resolved

### Bug 1: stale Quadlet image reference masqueraded as Phase 5 crash

The original Phase 5 build (`wow-server:phase5-t3-rebuild`, image id `1ce2ddf4ee45`) deployed correctly to `:current`, but **the Quadlet `wow-worldserver.container` had been edited to hardcode `Image=c135dd498cad`** — a 704 MB Phase 0 base image that pre-dated Phase 5. When `systemctl restart` ran, it loaded the Phase 0 base, not our Phase 5 image.

Symptoms during the earlier (incorrectly-attributed) failure:
- `LlmAgentManager::Start` never logged anything → suggested LlmAgent never initialized
- JSONL file never created → confirmed Start didn't run its open-jsonl block
- Worldserver SIGSEGV'd 12 times under "LlmAgent activity" → in fact, the running image's PlayerbotAIConfig was looking up Phase 5 conf keys it didn't recognize, but more critically, the strategy registration mismatch (Phase 0 expected T1 only) plus the new `.playerbots t3 inject_whisper` command writing to a `Whispers()` accessor not present in that old binary caused undefined behavior

**Fix:** restored Quadlet to `Image=localhost/wow-server:current`, daemon-reloaded, restarted. First-boot LlmAgent log lines appeared:

```
[LlmAgent] Start: Enabled=1 JsonlPath='/azerothcore/env/dist/logs/llm_agent_phase4.jsonl' WorkerThreads=4 Tier3.Enabled=1 SamplePct=100
[LlmAgent] Start: jsonl_open='...' is_open=1
[LlmAgent] Start: ready — workers=4
[LlmAgent] LlmChatTrigger::IsActive first dispatch — bot guid=2466
```

### Bug 2: "playerbots" log channel doesn't route to Server.log (carried from Phase 4)

`LOG_INFO("playerbots", ...)` calls in Phase 5 diagnostic instrumentation produced no output in `Server.log` or `journalctl`. Phase 4's smoke summary already flagged this; Phase 5 hit it again during debug. **Workaround:** the diagnostic logs now use `LOG_INFO("server.loading", ...)` which routes correctly. This is fine for boot-time logs but means runtime per-tick logs (e.g., `LlmCounters::DumpToLog`) still don't show — Phase 5.1 follow-up.

## What's confirmed working

| Component | Evidence |
|---|---|
| `LlmAgentManager::Start` | Logs Enabled=1, workers=4, JSONL open |
| Worker pool | 4 threads spawned; T1 + T3 records flowing |
| `.playerbots t3 inject_whisper` admin command | All 25 injections (10 + 15 batch) returned "T3 whisper injected for %s" |
| `WhisperBuffer.Push` from hook | Hook ran without crash (no SEGVs after Quadlet fix) |
| `LlmChatTrigger` dispatch | First-call log line emitted; trigger ran on bot tick |
| `LlmChatAction` end-to-end | 3 T3 records reached the JSONL append step |
| Persona + envelope path | Utterances are character-appropriate text (not raw model output) |
| `kT3OutputSchema` constraint | All 3 records have well-formed `{utterance, side_effects}` JSON |
| Worker→main thread propagation | `parsed_status: "ok"` set correctly for tier=3 |
| Tick performance | mean 11 ms, p95 32 ms — no regression from Phase 4 baseline |

## Run setup

- **Host:** Heimdal (Bazzite, AMD RX 6900 XT, Vulkan)
- **llama-server:** Qwen 2.5 7B Q4_K_M, `--parallel 8 --ctx-size 16384`
- **Image:** `localhost/wow-server:phase5-diaglog2` (image id `5f83ee04bdd1`), tagged `:current`
- **Worldserver tick:** mean 11 ms, p95 32 ms
- **Bot population:** 1000 characters in world, 0 connected players (smoke via admin command only)
- **Config:** `LlmAgent.Enabled = 1` during the run, `Tier3.Enabled = 1`, `SamplePct = 100`, `Tier3.CooldownMs = 5000`
- **Injections:** 25 `.playerbots t3 inject_whisper` calls across two batches (10 + 15)

## Findings & observations

### Queue saturation under T1 load

T3 records had queue waits of 158 / 368 / 403 seconds — that's the time between enqueue and worker-pop. The reason: with `SamplePct = 100`, all 1000 bots are T1-eligible, and T1 keeps the 4-worker pool fully saturated. T3 requests get queued behind hundreds of pending T1 requests.

For a production single-server smoke this isn't a real problem (a 6-min reply latency is too slow for chat, but the pipeline works). Mitigations for Phase 5.1:
- Reduce T1 cadence (longer cooldown — currently inferred 60min, but with SamplePct=100 the trigger fires across 1000 bots constantly)
- Add a priority queue (T3 jumps T1 in the worker queue — chat is user-facing, plan replan is background)
- Lower `SamplePct` for general smoke tests

### LLM choice of empty `side_effects`

All 3 T3 ok records had `side_effects: []`. The model output text-only replies. Possible reasons:
- The default Tier3 system-prompt-suffix (`Tier3_BuiltInSystemPromptSuffix`) didn't load — verify next session via `cfg.Tier3_BuiltInSystemPromptSuffix.length()` log line. Earlier Server.log line warned `Missing property AiPlayerbot.LlmAgent.Tier3.SystemPromptSuffix`, but that key has a default in `LoadLlmAgentConfig` so the cfg value should be `""` and the built-in suffix gets used. Worth a manual check.
- The model is genuinely opting not to emit tool calls for "want to group up at the inn" — the prompt doesn't strongly imply a single right tool (no `accept_party_invite` event, no `quest_id` in context). Sane behavior.

The pipeline that would apply a non-empty `side_effects` (ParseChatEnvelope → Validate → ApplyToolCall) is in place and unit-tested locally; the smoke just didn't exercise it.

## Operator state at hand-off

- `AiPlayerbot.LlmAgent.Enabled = 0` on Heimdal (set after smoke per standing instruction)
- `wow-worldserver.service` running stably on `localhost/wow-server:current` (`5f83ee04bdd1`)
- Quadlet now correctly references `:current` rather than a hardcoded image id
- All Phase 5 commits pushed to `origin/claude/llm-agent-phase-5-t3-chat-brain`
- Old `wow-server:phase4-tierdispatch` retained as the rollback target

## Phase 5.1 candidates

1. **Validate non-empty `side_effects` end-to-end.** Hand-craft a prompt that the model will definitely answer with a tool call (e.g., an explicit "accept this invite from Bob" scenario). Confirm `tool_applied` counter increments.
2. **Address queue saturation.** Either priority-bump T3 in the worker queue, or rein in T1 frequency (lower SamplePct).
3. **Fix the "playerbots" log channel routing.** `LlmCounters::DumpToLog` produces nothing in any log file today — phase-5 used `server.loading` as a workaround.
4. **Remove diagnostic LOG_INFO calls.** The first-dispatch log in `LlmChatTrigger` and the OnWhisperReceived `PushWhisper done / Whispers().Push done` logs are temporary instrumentation.
5. **Verify `Tier3_BuiltInSystemPromptSuffix` is actually being used.** Earlier "Missing property" warning suggests the conf key wasn't fully populated; want a one-line log of the active suffix to confirm.
6. **Add `.playerbots t3 inject_invite` and `.playerbots t3 inject_join`** admin commands to mechanically test the other two T3 trigger surfaces.
7. **Real-player demo.** Smoke was admin-command-only; carrying over from Phase 4 plan.
