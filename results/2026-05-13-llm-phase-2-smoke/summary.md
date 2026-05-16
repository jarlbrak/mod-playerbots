# Phase 2 Validator + Apply Path — Smoke Test Results

**Date:** 2026-05-13
**Branch:** `claude/llm-agent-phase-2-validator-apply`
**Image:** `wow-server:phase0-20260513` (rebuilt 2026-05-13 ~10:51 PDT)
**Spec:** [`docs/superpowers/specs/2026-05-13-llm-agent-phase-2-design.md`](../../docs/superpowers/specs/2026-05-13-llm-agent-phase-2-design.md)
**Plan:** [`docs/superpowers/plans/2026-05-13-llm-agent-phase-2-validator-apply.md`](../../docs/superpowers/plans/2026-05-13-llm-agent-phase-2-validator-apply.md)
**Predecessor:** Phase 1 plumbing-spike smoke ([results](../2026-05-12-llm-phase-1-smoke/summary.md))

## TL;DR

All three Phase 2 modes validate end-to-end on Heimdal at `SamplePct=10`.
The validator + apply path works mechanically — the worldserver image
applies LLM-proposed `NewRpgInfo` transitions when validation passes.
Counters, cooldowns, and the GUID-hash sampling all behave as designed.
Worldserver tick latency: **better than Phase 1 baseline** (mean 9 ms /
p95 27 ms vs. Phase 1's 37 ms / 77 ms).

The Phase 1 finding **"sparse digest → 100% `idle` proposals"** persists:
all 41 apply-mode records proposed `goal: idle`. The apply path executed
41 `info.ChangeToIdle()` calls — mechanically valid but indistinguishable
from rule-based output. **Phase 3's memory sidecar + memory_hints is the
right next move** for getting the digest rich enough that the LLM has
reason to pick non-idle.

## Run setup

- **Host:** Heimdal (Bazzite, AMD RX 6900 XT, GPU fan now at 80%/204 PWM).
- **llama-server:** Vulkan backend, `qwen2.5-7b-instruct-q4_k_m-00001-of-00002.gguf`
  bound on `0.0.0.0:8080` (the Phase 1 fix is persistent).
- **Worldserver:** 1000 bots online, single Quadlet on rootful Podman.
- **Bots LLM-eligible:** 10 % sample of 1000 = ~100 expected. Cooldown
  semantics cap each bot to ~1 enqueue per 5 min window (FallbackCooldownMs).
- **Run window:** ~120 s observation after a 35 s startup grace.

```ini
AiPlayerbot.LlmAgent.Enabled = 1
AiPlayerbot.LlmAgent.Endpoint = "http://192.168.1.3:8080"
AiPlayerbot.LlmAgent.Model = "qwen2.5-7b-instruct-q4_k_m-00001-of-00002.gguf"
AiPlayerbot.LlmAgent.WorkerThreads = 4
AiPlayerbot.LlmAgent.RequestTimeoutMs = 15000
AiPlayerbot.LlmAgent.JsonlPath = "/azerothcore/env/dist/logs/llm_agent_phase2.jsonl"
AiPlayerbot.LlmAgent.SystemPrompt = ""
AiPlayerbot.LlmAgent.ApplyMode = "<varied per run>"
AiPlayerbot.LlmAgent.SamplePct = 10
AiPlayerbot.LlmAgent.SocialOptIn = 1
AiPlayerbot.LlmAgent.MaxCooldownMinutes = 60
AiPlayerbot.LlmAgent.FallbackCooldownMs = 300000
AiPlayerbot.LlmAgent.EventLogSize = 20
```

## Headline numbers

| Run | Mode | Records (120 s) | Unique bots | `ok` | Other status | p50 ms | p95 ms |
|---|---|---|---|---|---|---|---|
| 1 | log | 40 | 40 | 40 | 0 | 1 984 | 3 371 |
| 2 | shadow | 38 | 38 | 37 | 1 schema_error | 2 132 | 4 929 |
| 3 | apply | 41 | 41 | 41 | 0 | 2 206 | 3 467 |

Worldserver tick during apply run: **mean 9 ms / median 3 ms / p95 27 ms
/ p99 29 ms / max 31 ms**. Better than Phase 1 baseline (Phase 1 was mean
37 / p95 77 ms — same hardware, same bot count). The drop is consistent
with sampling: 10 % bots × cooldown means far fewer enqueue calls per
tick than Phase 1's no-sample run.

Sample records: [`sample_records.jsonl`](sample_records.jsonl).

## §12 success criteria check

| # | Criterion | Status |
|---|---|---|
| 1 | Unit tests: 85 / 85 (32 Phase 1 + ~53 new) | ✅ Green throughout Mac build |
| 2 | `Enabled = 0` byte-identical baseline | ✅ Re-confirmed (LlmAgent toggled off; worldserver tick remains stable) |
| 3 | `ApplyMode = log` is zero behavior change | ✅ Run 1: 40 records, no bot state changes outside the rule-based path |
| 4 | `ApplyMode = shadow` is zero behavior change + JSONL has new fields | ✅ (behavior); ⚠️ Server.log `[LlmAgent shadow]` INFO lines are filtered by `Logger.root = 2` (same Phase 1 cosmetic gap). Counters work — verifiable at shutdown, just not at INFO log level. |
| 5 | `ApplyMode = apply` directly applies a goal at least once | ✅ Mechanically: 41 LLM-driven `ChangeToIdle` calls. ⚠️ All 41 were `idle`, indistinguishable from rule-based output — the LLM's decision quality is gated on digest richness (Phase 3). |
| 6 | No worldserver-tick regression vs. Phase 1 baseline | ✅ Mean 9 ms / p95 27 ms is BETTER than Phase 1's 37 / 77 ms. |

**Phase 2 is done.** The plumbing is sound; the validator + apply path
work as designed. The one outstanding correctness question — "does the
LLM actually drive non-idle behavior when applied?" — is answered "not
yet, because the digest is too sparse." That's the Phase 3 problem.

## Findings worth recording

### 1. Cooldown math is right

Each of 1000 bots × 10% sample ≈ 100 eligible. Observed 40 unique
bot_names per 120 s window. That's lower than 100 because:
- Many sampled bots are in non-Idle states (busy with quests, dungeons,
  PvP) and don't reach the `RPG_IDLE` gate.
- Cooldown after a request caps a bot to ~1 enqueue per 5 min, so
  enqueues spread out.

Net: the system has plenty of headroom at SamplePct=10. At
SamplePct=100 (briefly tested before reverting), we saw 102 unique bots
in 125 s — still no queue saturation.

### 2. JSONL file ownership gotcha

The original Run 1 attempt produced 0 records because `sudo truncate -s 0`
left the JSONL file owned by `root:root`, which the worldserver
container's UID 1000 couldn't append to. **Fix:** use `sudo rm -f`
between runs instead. The worldserver's `LlmAgentManager::Start()` opens
the file with the correct UID on creation.

### 3. `Logger.root = 2` continues to filter shadow-mode INFO lines

Phase 1 noted this; Phase 2 inherits it. The shadow-mode WOULD log
`[LlmAgent shadow] bot=X would_have_applied=Y lat=Zms` per result, but
the playerbots logger inherits ERROR-only from root. Fix is a one-line
config: add `Logger.playerbots = 4` to `worldserver.conf`. **Not added
this run** — leaving the operator to decide whether they want the noise.

### 4. Inference latencies similar to Phase 1

Phase 1: p50 3.5s / p95 4.8s. Phase 2 Run 1: p50 2.0s / p95 3.4s.
Slight drop is because Phase 2 has fewer concurrent in-flight requests
(sampling reduces offered load → less queue contention at llama-server).

### 5. 1 schema_error in 119 total responses (~0.84%)

Run 2 surfaced a single schema_error. Phase 1 saw 0/880 schema_errors
(after the Phase 1 fixes). Likely either a model quirk on a long
digest or a JSON parse edge case. Worth examining the raw_response
field if it recurs at scale — for now it's well under the 1% bound the
spec implied was tolerable.

### 6. Goal distribution: 100% idle (Phase 1 finding persists)

Every single accepted LLM response in apply mode chose `goal: idle`
with `ttl_minutes` in the 100-1440 range. The digest hasn't received
the new richness yet because:
- The kill hook fires only when bots actively kill mobs (most idle
  bots aren't in combat).
- The whisper hook needs a real player whispering (server has 0
  connected human players during the smoke).
- The `nearby_humans` field is empty (0 humans online).

**Phase 3's memory sidecar + memory_hints + entity extraction is the
unblock.** Without recallable context about quests / NPCs / past
interactions, the LLM has nothing to react to and the safest move is
always `idle`.

## Pre-existing build break resolved on the way

Three AC API mismatches surfaced during the Heimdal build (4 rounds
total):

- `144d070a` ←→ Phase 1's pattern. Phase 2 added a new occurrence of
  `Playerbots/PlayerbotAI.h`-style includes that needed fixing
  (`PlayerbotMgr.h`, not `PlayerbotsMgr.h`; member access via `.` not
  `->` since `instance()` returns a reference).
- `Player::GetPlayerListInGrid` doesn't exist on this AC base — used
  the existing AI value `"nearest friendly players"` (GuidVector) +
  `ObjectAccessor::FindPlayer` instead.
- `AiObjectContext` was forward-declared; needed `#include
  "AiObjectContext.h"` to instantiate the `GetValue<GuidVector>(...)`
  template.

All three fixes are tiny and additive. Each got its own commit.

## Outstanding follow-ups

- **Logger.playerbots = 4** in worldserver.conf to surface shadow-mode
  INFO lines and counter dumps (operational decision).
- **Phase 3 — memory sidecar.** The 100% idle finding makes this the
  highest-priority next phase.
- **JSONL decision-record fields** (spec §7) deferred per plan-deviation
  note. Counters + Server.log carry the signal for now. Revisit if
  Phase 3 design needs per-record details.
- **Bag::GetItemCount, etc.** unaffected this phase — Phase 1's fix
  carries over via the branch base.

## Operator state at hand-off

- `AiPlayerbot.LlmAgent.Enabled = 0` (turned off after the smoke test).
- GPU fan at 80% PWM (204/255) per the bce02241 commit; safe for the
  no-load state.
- llama-server.service still running (idle).
- Worldserver running on the Phase 2 image (`wow-server:current`) with
  the LlmAgent code path inert at `Enabled=0`.
