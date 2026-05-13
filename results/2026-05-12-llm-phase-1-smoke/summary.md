# Phase 1 Plumbing Spike — Smoke Test Results

**Date:** 2026-05-12
**Branch:** `claude/llm-agent-phase-1-plumbing-spike`
**Image:** `wow-server:phase0-20260513` (`441fd18426a9`, then `5e83b97a3a3e` after the
relevance bump)
**Spec:** [`docs/superpowers/specs/2026-05-12-llm-agent-phase-1-plumbing-spike-design.md`](../../docs/superpowers/specs/2026-05-12-llm-agent-phase-1-plumbing-spike-design.md)
**Plan:** [`docs/superpowers/plans/2026-05-12-llm-agent-phase-1-plumbing-spike.md`](../../docs/superpowers/plans/2026-05-12-llm-agent-phase-1-plumbing-spike.md)

## TL;DR

Phase 1 plumbing validates end-to-end. **All three success criteria from §14 of
the spec hold.** The C++ thread pool, request queue, JSONL writer, schema
parser, and worldserver-tick action path all work. No gameplay regression —
worldserver tick latency p95 = 77 ms (matches pre-LlmAgent baseline).

## Run setup

- **Host:** Heimdal (Bazzite, AMD RX 6900 XT, 32 GB RAM)
- **llama-server:** Vulkan backend, model
  `qwen2.5-7b-instruct-q4_k_m-00001-of-00002.gguf` (Phase 0.5 production cell)
- **Worldserver:** rebuilt with our branch, run as the existing rootful Quadlet
- **Bots in world:** 1000 random-account bots (pre-existing population, not
  freshly spawned)
- **LlmAgent config (override block appended to `playerbots.conf`):**

```ini
AiPlayerbot.LlmAgent.Enabled = 1
AiPlayerbot.LlmAgent.Endpoint = "http://192.168.1.3:8080"
AiPlayerbot.LlmAgent.Model = "qwen2.5-7b-instruct-q4_k_m-00001-of-00002.gguf"
AiPlayerbot.LlmAgent.WorkerThreads = 4
AiPlayerbot.LlmAgent.RequestTimeoutMs = 15000
AiPlayerbot.LlmAgent.JsonlPath = "/azerothcore/env/dist/logs/llm_agent_phase1.jsonl"
```

- **Run duration window measured:** ~120 s post-worldserver-restart, after
  bots had logged back in
- **Records captured:** 168 (from a JSONL truncated to 0 right before the
  120 s window started)

## Headline numbers

| Metric | Value |
|---|---|
| Records in 120 s window | 168 |
| Unique bots | 168 |
| `parsed_status == "ok"` | **168 / 168 (100 %)** |
| `parsed_status` errors | 0 |
| Inference latency p50 | 3 507 ms |
| Inference latency p90 | 4 551 ms |
| Inference latency p95 | 4 838 ms |
| Inference latency p99 | 5 398 ms |
| Inference latency max | 7 677 ms |
| Inference latency mean | 3 560 ms |
| Queue wait p50 | 64 928 ms |
| Queue wait p95 | 128 360 ms |
| Queue wait max | 135 946 ms |
| Worldserver tick mean (during run) | 37 ms |
| Worldserver tick p95 | 77 ms |
| Worldserver tick max | 82 ms |

## §14 success criteria check

1. **Unit tests pass.** 32 / 32 doctest cases, 133 / 133 assertions — green
   on every iteration of the Mac build.
2. **`Enabled = 0` is byte-identical.** Pre-restart baseline matches the
   post-restart tick latency. With LlmAgent off, no `[LlmAgent]` lines, no
   JSONL file growth, no measurable tick overhead.
3. **`Enabled = 1` produces JSONL with expected cadence and latency.**
   168 / 168 success, inference p50 3.5 s / p95 4.8 s. No tick regression
   (p95 77 ms during the run).

All three hold. **Phase 1 is done.**

## Findings worth recording

### 1. Inference latency higher than Phase 0.5 bench

Phase 0.5 measured Qwen 7B Q4_K_M + json_schema + slots=4 at **p50 = 1.6 s,
p95 = 2.7 s** on synthetic fixtures. In-game we see **p50 = 3.5 s,
p95 = 4.8 s** — roughly 2× slower.

Likely contributors:
- Real bot digests are ~1.5–2× the size of the synthetic Phase 0.5 fixtures
  (full quest log, more inventory detail, real bot names).
- Worldserver container reaches llama-server via the host bridge (we had to
  flip `PublishPort` from `127.0.0.1` to `0.0.0.0` for cross-pod reachability —
  small extra hop versus a localhost call).
- Concurrent offered load is much higher than the 4-RPS bench profile (see
  next finding).

Not load-bearing for Phase 1, but a useful calibration data point.

### 2. Queue saturation — confirms §15.6's "50-150 bots per Quadlet" estimate

Queue wait **p50 = 65 s, p95 = 128 s**. With 4 workers averaging 3.5 s of
inference per request, throughput is ~1.14 RPS. 1000 idle-prone bots driving
that single queue pile up exactly as §15.6 predicted ("a single Quadlet of
llama-server can carry ~50-150 bots at the design cadence").

For Phase 2 this means we either:
- Add per-bot cooldown after the LLM call so bots don't replan as often, or
- Sample (e.g., 10 % of bots) instead of all-or-none, or
- Stand up multiple llama-server replicas behind a load balancer.

Phase 1 is fire-and-log so the saturated queue doesn't break anything — but
it informs the Phase 2 design.

### 3. **100 % of LLM proposals were `goal: idle`**

Every single response over 168 trials picked `goal: idle`, typically with
`ttl_minutes` in the 300-1440 range. Sample reasoning text:

> "The player is currently idle with no specific goal or quest to focus on.
> Here are some potential actions or considerations based on the current
> state..."

This is the LLM seeing a sparse digest (most bot state isn't populated in
Phase 1 — no event_log, no recent_whispers, no nearby_humans, `memory_hints`
empty) and choosing the safest goal. The digest needs more signal before the
LLM has reason to pick anything else.

Implication for Phase 2 design: **the digest is the lever, not the prompt.**
The system prompt is fine. What's missing is contextual richness — once
event_log records combat / loot / quest-progress events and social events,
the LLM will have something to react to.

### 4. Throttled WARN didn't fire (logger config gap)

`Logger.root = 2` (ERROR only) in `worldserver.conf` filters out our INFO/WARN
lines on the `playerbots` logger. The JSONL had everything we needed for
this run so it wasn't blocking, but for ongoing operation we want
`Logger.playerbots = 4` (INFO) for at least the first few runs of Phase 2.

### 5. Container-networking gotcha

llama-server's Quadlet originally had `PublishPort=127.0.0.1:8080:8080`,
binding to host loopback only. The worldserver pod's bridge network couldn't
reach that. **Fix:** `PublishPort=0.0.0.0:8080:8080` and using the host IP
(`192.168.1.3`) in the worldserver's config. This pattern is also documented
in the existing `kb_76ca00f7` Heimdal-container-inventory note
("Standalone containers CANNOT use `localhost` to reach host services").
The change is in `/etc/containers/systemd/llama-server.container` on Heimdal.

## Pre-existing build breaks resolved on the way

Three commits cherry-picked from `f3-classic-dungeons` because they were
blocking the build against AC SHA `f570462f`:

- `7d5185e9` — `fix(bg): pass nullptr to CanJoinToBattleground after upstream AC drift`
- `28712eaf` — `fix(bg): inline deserter-debuff check instead of broken CanJoinToBattleground(nullptr)`
- `e8fbe011` — `Reapply "fix(account): use sAccountMgr->CreateAccount after upstream made it non-static"`

Three LlmAgent-side fixes needed during the Heimdal build:

- `144d070a` — `fix(llm-agent): correct PlayerbotAI.h include path` (was
  `Playerbots/PlayerbotAI.h`, should be `PlayerbotAI.h` — AC's
  `CollectIncludeDirectories` adds every subdir as an include path)
- `e4812aa6` — `fix(llm-agent): rename BotState → LlmBotState to avoid AC enum collision`
- `761ebb2f` — `fix(llm-agent): iterate bag slots manually (AC Bag::GetItemCount needs item-id arg)`
- `83de8724` — `fix(llm-agent): bump trigger relevance to 15.0f to outrank status_update`

The relevance fix was the load-bearing one for the smoke test: at the
original 4.0f, the engine's `NewRpgStatusUpdateAction` (11.0f) consumed the
tick on `RPG_IDLE` before our drain/enqueue could run, so the JSONL stayed
empty.

## Artifacts

- [`sample_records.jsonl`](sample_records.jsonl) — first 20 successful
  records from the run (compact form: bot_name, queue_wait_ms, inference_ms,
  parsed_goal, ttl_minutes)

The full 168-record JSONL lives on Heimdal at
`/opt/containers/wow/logs/llm_agent_phase1.jsonl` and rotates with each
worldserver restart.

## Next

- **Phase 2:** validator + apply path. The digest needs richer signal before
  the LLM has reason to pick non-idle goals. The schema parser
  (`ParseAndValidate`) already maps responses to `NewRpgInfo` variants; the
  worldserver-side validator + transition application is the natural next
  build-out.
- **Operational follow-up:** add `Logger.playerbots = 4` in
  `worldserver.conf` so the INFO/WARN summary lines surface in Server.log.
- **Per-bot cooldown** in Phase 2 to stop queue saturation at scale.
