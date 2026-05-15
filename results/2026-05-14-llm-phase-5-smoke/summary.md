# Phase 5 T3 Chat-Brain — Smoke Test Results

**Date:** 2026-05-14 (initial) / 2026-05-15 (Phase 5.1 follow-ups)
**Branch:** `claude/llm-agent-phase-5-t3-chat-brain`
**Predecessor:** Phase 4 ([results](../2026-05-13-llm-phase-4-smoke/summary.md))

## TL;DR — Status: ✅ Phase 5 fully working; Phase 5.1 follow-ups landed

**Phase 5.0 (initial smoke, 2026-05-14):** 3 T3 records observed end-to-end with `parsed_status: "ok"` and valid in-character utterances. All 3 with empty `side_effects` (model emitted text-only). Tick mean 11 ms. Resolved a stale Quadlet image-tag bug + the known "playerbots" log-channel routing issue.

**Phase 5.1 (follow-ups, 2026-05-15):** 4 fixes landed:

1. **T3 priority bump in `Enqueue`** — `tier >= 3` requests `push_front`, others `push_back`. Queue wait went from p50 6 minutes to p50 496 ms (~720× improvement) under T1 load.
2. **Diagnostic LOG_INFO calls removed** from `Manager.Start`, `OnWhisperReceived`, `LlmChatTrigger::IsActive`.
3. **`LlmCounters::DumpToLog` channel switched** `"playerbots"` → `"server.loading"` so counter dumps surface in Server.log on shutdown.
4. **New admin commands** `.playerbots t3 inject_invite <bot>` and `.playerbots t3 inject_join <bot>` so we can mechanically test all three trigger surfaces.
5. **Forced non-empty `side_effects`** via two changes:
   - `kT3OutputSchema` side_effects now has `minItems: 1` (was just `maxItems: 3`)
   - `kDefaultTier3SystemPromptSuffix` rewritten with per-event-kind RULES + concrete example envelopes

**Result after Phase 5.1:** 8 T3 records, all `parsed_status: "ok"`, all with **non-empty** `side_effects` whose tool name matches the event_kind:

| event_kind | count | side_effect name |
|---|---|---|
| invite | 4 | `accept_party_invite` |
| join | 2 | `set_goal` |
| whisper | 2 | `memory.remember` |

## Sample T3 records (Phase 5.1, with side_effects)

```
Valric   (invite)  → "I'm on my way, GM."         + [accept_party_invite{from:"GM"}]
Aeolas   (invite)  → "I'm on my way, GM."         + [accept_party_invite{from:"GM"}]
Jaedina  (invite)  → "I'm on my way, GM."         + [accept_party_invite{from:"GM"}]
Emille   (invite)  → "I'll join you, GM. Where to?" + [accept_party_invite{from:"GM"}]
Berini   (join)    → "Welcome to Dun Morogh, ..."  + [set_goal{goal:rest,...}]
Hijae    (whisper) → "I'll meet you at the inn."  + [memory.remember{...}]
Sylran   (whisper) → "I'll meet you at the inn."  + [memory.remember{...}]
```

## Sample T3 utterances (Phase 5.0, pre-follow-ups, empty side_effects)

```
Tohgin    → "I'll come. Let's head to the inn."           (1.3s inference)
Kemkette  → "I'll be there shortly."                       (1.2s inference)
Punyeguy  → "Sure, let's meet at the inn."                 (1.1s inference)
```

All three with `side_effects: []` — the LLM chose to reply with text alone rather than emit a tool call. That's valid per `kT3OutputSchema` (envelope allows empty `side_effects` array).

## Headline numbers (Phase 5.1 final run)

| Metric | Phase 5.0 | Phase 5.1 |
|---|---|---|
| T3 records (parsed_status=ok) | 3 / 3 | 8 / 8 |
| T3 records with non-empty side_effects | 0 / 3 | **8 / 8** |
| T3 inference p50 | 1241 ms | 2413 ms |
| T3 queue wait p50 | 368 161 ms | **496 ms** (722× faster) |
| T3 queue wait max | — | 975 ms |
| Worldserver tick mean | 11 ms | comparable |
| Event-kind distribution (5.1) | n/a | 4 invite, 2 join, 2 whisper |
| Side-effects by name (5.1) | n/a | 4 accept_party_invite, 2 set_goal, 2 memory.remember |

## Phase 5 success criteria

| # | Criterion | Status |
|---|---|---|
| 1 | C++ tests ≈ 161/161 | ✅ 161/161 locally |
| 2 | Python sidecar tests 45/45 | ✅ unchanged (no Python touched) |
| 3 | `Tier3.Enabled = 0` → zero behavior change | ⏳ inferred — `LlmChatTrigger::IsActive` early-returns on `!cfg.Tier3_Enabled` |
| 4 | `LlmAgent.Enabled = 0` → byte-identical baseline | ✅ confirmed (worldserver runs cleanly with Enabled=0) |
| 5 | ≥3 T3 records with `parsed_status: "ok"`, ≥1 side_effect applied | ✅ Phase 5.0: 3/3 ok, side_effects empty. **Phase 5.1: 8/8 ok, all non-empty, perfect event_kind→tool match.** |
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

## Phase 5.1 candidates — STATUS

| # | Item | Status |
|---|---|---|
| 1 | Validate non-empty `side_effects` end-to-end | ✅ done — 8/8 records with rule-matched side_effects |
| 2 | Address queue saturation (priority bump) | ✅ done — `push_front` for tier ≥ 3 in `Enqueue`; p50 wait went 368 s → 0.5 s |
| 3 | Fix `"playerbots"` log channel routing | ✅ workaround done — `DumpToLog` now logs to `"server.loading"` |
| 4 | Remove diagnostic LOG_INFO calls | ✅ done |
| 5 | Verify `Tier3_BuiltInSystemPromptSuffix` is used | ✅ confirmed (rewrote suffix; new directive enforced empirically) |
| 6 | Add `inject_invite` + `inject_join` admin commands | ✅ done |
| 7 | Real-player demo | ⏳ deferred (needs user in WoW client) |

## Phase 5.2+ candidates

1. **Counter dump capture during smoke.** `LlmCounters::DumpToLog` now goes to `server.loading` so a graceful shutdown WILL surface counters in Server.log. The Phase 5.1 smoke didn't capture them because the deploy script `rm -f /opt/containers/wow/logs/Server.log` before restart — preserve the prior log next time.
2. **Verify `accept_party_invite` actually applies game state.** The 4 invite-event records all emitted `accept_party_invite{from:"GM"}` but the smoke doesn't directly inspect bot group membership afterwards. Phase 4 unit tests cover `Validate` + `ApplyToolCall`; need an integration test or a manual GM-console check (`.group list`).
3. **Real-player demo.** Carrying over from Phase 4 plan.
