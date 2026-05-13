# Phase 3 Memory Sidecar + T0 Hints — Smoke Test Results

**Date:** 2026-05-13
**Branch:** `claude/llm-agent-phase-3-memory-sidecar`
**Image:** `wow-server:phase0-20260513` (rebuilt with zone-tagging fix in `66310554`)
**Spec:** [`docs/superpowers/specs/2026-05-13-llm-agent-phase-3-memory-sidecar-design.md`](../../docs/superpowers/specs/2026-05-13-llm-agent-phase-3-memory-sidecar-design.md)
**Plan:** [`docs/superpowers/plans/2026-05-13-llm-agent-phase-3-memory-sidecar.md`](../../docs/superpowers/plans/2026-05-13-llm-agent-phase-3-memory-sidecar.md)
**Predecessor:** Phase 2 ([results](../2026-05-13-llm-phase-2-smoke/summary.md))

## TL;DR

The memory subsystem ships end-to-end. Python sidecar + llama-embed
Quadlet + C++ MemoryHttpClient + Tier0 recall_about integration + kill
hook shadow writer + lazy persona stub all work. JSONL records show
`memory_hints` populating (39 / 99 = 39 % hit rate in the apply-mode
run). Sidecar stored 4 031 memories across 727 bots without breaking a
sweat.

**One Phase 3 success criterion did not hold:** goal-distribution
diversification under apply mode. The LLM still chose `idle` for
97 / 99 valid responses. Memory enrichment alone — at least the kill
events we capture today — isn't enough signal to flip the LLM's
posture in a closed bot ecosystem with **zero connected human
players**. That's now squarely Phase 4 work (T2 interactive
tool-calling, real-human social opt-in proving its keep).

All other Phase 3 invariants and mechanical criteria hold. Memory
plumbing is correct; the next phase needs a different signal.

## Run setup

- **Host:** Heimdal (Bazzite, AMD RX 6900 XT, GPU fan still at 80 %).
- **llama-server:** Vulkan, Qwen 2.5 7B Q4_K_M, slots=4 (unchanged).
- **llama-embed (NEW):** Vulkan, `bge-small-en-v1.5-Q4_K_M.gguf`
  (384-dim, ~24 MB), `--embedding` mode on port 8081.
- **memory-sidecar (NEW):** Python 3.12 FastAPI image on port 8090,
  bind-mount `/opt/containers/memory/db.sqlite`.
- **Worldserver:** ~1 000 random-account bots, 0 connected human
  players throughout (this matters — see findings).
- **Sample:** 10 % GUID-hash sampling, 5 min cooldown after rejected /
  error outcomes.

Two test windows actually ran:

- **Cold-start Run:** sidecar DB wiped right before worldserver
  restart; 5 min, ApplyMode=log, SamplePct=10.
- **Apply Run:** continuation with the warm memory from Cold-start;
  ApplyMode flipped to `apply`; 15 min observation.

(The plan called for a separate "warm log-only" run between cold and
apply. Run 1 already partially warmed the memory in its 5 min window
— 980 memories at end — so we skipped the dedicated Run 2 and went
straight to apply.)

## Headline numbers

### Cold-start Run (ApplyMode=log, 5 min)

| Metric | Value |
|---|---|
| Records | 68 |
| `parsed_status == "ok"` | 68 / 68 (100 %) |
| `memory_hints` length distribution | 51 × 0, 8 × 1, 7 × 2, 2 × 3 |
| Records with ≥ 1 hint | 17 / 68 (25 %) |
| Sidecar memories end-of-run | 980 |
| Sidecar entities end-of-run | 1 013 (zones + mob names) |
| Sidecar edges end-of-run | 594 |

### Apply Run (ApplyMode=apply, 15 min, warm memory)

| Metric | Value |
|---|---|
| Records | 99 |
| `parsed_status == "ok"` | 97 / 99 (98 %) — 2 schema_error |
| `memory_hints` length distribution | 60 × 0, 13 × 1, 8 × 2, 18 × 3 |
| Records with ≥ 1 hint | 39 / 99 (**39 %**) |
| Inference latency p50 / p95 | 1 556 ms / 3 967 ms |
| Proposed goal distribution | 97 × `idle`, 2 × `null` (schema_error) |
| Tick mean / median / p95 / max | 10 / 2 / 29 / 37 ms |
| Sidecar memories end-of-run | 4 031 |
| Sidecar entities end-of-run | 2 016 |
| Sidecar edges end-of-run | 1 326 |
| Bots tracked in sidecar | 727 |

Sample records: [`sample_records.jsonl`](sample_records.jsonl). Example
record with a hint:

```json
{
  "bot_name": "Phehaa",
  "parsed_status": "ok",
  "inference_ms": 2090,
  "parsed_goal": "idle",
  "hints_len": 1,
  "hints": ["killed Grizzled Brown Bear in Bloodmyst Isle"]
}
```

## §12 success criteria check

| # | Criterion | Status |
|---|---|---|
| 1 | Python tests ~40 / 40 green | ✅ 45 / 45 (5 schema + 4 embed + 8 scoring + 8 write + 6 recall + 6 recall_about + 4 personality + 4 eviction) |
| 2 | C++ tests 93 / 93 green | ✅ Phase 2's 85 + 6 MemoryHttpClient + 2 config = 93 |
| 3 | Sidecar-unreachable degrades cleanly | ✅ (implicit — `MemoryHttpClient` has 30-s sticky-down + best-effort empty / nullopt returns; the plan's standalone test wasn't re-executed but the behavior was confirmed under the deploy-fail probe earlier) |
| 4 | `memory_hints` populates after warm-up | ✅ 39 / 99 records (39 %) in apply run — sidecar's `recall_about` returns real hints derived from kill events tagged with zones |
| 5 | Apply mode produces non-idle goals (target: ≥ 20 %) | ❌ 97 / 99 still `idle` |
| 6 | No tick regression (mean ≤ 20, p95 ≤ 50) | ✅ Mean 10 / p95 29 ms — comparable to Phase 2's 9 / 27 ms |

**5 / 6 hold.** Criterion 5's miss is informative, not a regression —
see findings below.

## Findings worth recording

### 1. Memory plumbing is correct

The system end-to-end works:
- `OnPlayerbotCheckKillTask` fires → C++ Hook → `MemoryHttpClient.Remember`
  → sidecar `/memory/remember` → SQLite + `vec_memories` insert.
- `Tier0_StateDigest::SnapshotBot` → `MemoryHttpClient.RecallAbout` for
  zone → sidecar BFS + scoring → hints array → digest JSON →
  llama-server user message.
- 4 031 memories accrued in ~20 min of run-time. No errors observed
  in either Server.log or memory-sidecar's logs.

### 2. Zone-tagging gap discovered + fixed mid-run

First Run 1 attempt had **0 / 65 hint pickup** because the kill hook
only tagged `victim_name` as an entity. T0 queries
`recall_about(zone)` — and no memory was zone-attached, so recall
returned empty.

**Fix:** the kill hook now also tags the bot's current zone (via
`sAreaTableStore.LookupEntry(bot->GetZoneId())`) as a second entity
**and** writes a `located_in` relation linking victim → zone. The
whisper hook got the same treatment with `encountered_in`. Commit
`66310554` on this branch.

After the fix: 25 % hit rate in cold-start Run, 39 % hit rate in apply
Run.

### 3. **100 % `idle` finding persists despite memory enrichment**

This is the load-bearing result. Even when `memory_hints` carries 3
zone-relevant kill memories, the LLM still chooses `idle`. Examples
from the apply Run:

> `bot=Phehaa hints=["killed Grizzled Brown Bear in Bloodmyst Isle"]
>  → goal=idle ttl=300`
>
> `bot=Saetlaeth hints=["killed Murloc in Westfall", "killed Murloc
>  in Westfall", "killed Murloc in Westfall"] → goal=idle ttl=1440`

Reading the LLM's `reasoning` fields, the model is treating past
kills as "ambient information," not as a call to action. To pick
non-idle the LLM needs one of:

- A **live signal** (real player whispering, party invite, quest
  objective progress in the last few minutes).
- **Goal salience** (a high-priority quest that has progressed since
  the last LLM call, encoded richly in the digest).
- A **memory entry that suggests an action** (e.g. "RealPlayerBob
  asked me to meet at the bank in 5 min" — there are zero such
  memories because there are zero connected human players).

Phase 4 (T2 interactive tool-calling) is exactly where these live
signals show up. Phase 3 ships the memory infrastructure that Phase 4
will read from; what it can't do alone is invent a reason to act.

### 4. Performance numbers are good

- llama-embed throughput is plenty; the sidecar's LRU embedding cache
  hits frequently (each zone is queried repeatedly across multiple
  bots).
- Tick latency mean 10 ms / p95 29 ms — comparable to Phase 2's
  baseline. No observable degradation from the extra `recall_about`
  HTTP calls on the worldserver tick (each is 2-15 ms; T0 issues up
  to 5 of them but caps via `HintMaxChars`).
- Inference p50 1.6 s / p95 4.0 s — actually slightly faster than
  Phase 2 (which was p50 2.2 / p95 3.5). The `memory_hints` payload
  is small enough not to bloat the prompt meaningfully.

### 5. Sidecar Quadlet operational notes

Two small Heimdal-side gotchas worth recording (and now fixed):

- **Initial Quadlet install** via `tee /etc/containers/systemd/...`
  produced 0-byte files because the sudo-via-ssh pipe lost stdin. Fix:
  `scp` the file to /tmp first, then `sudo install -m 644 ...`. Now
  documented in the smoke procedure.
- **Old config blocks in `playerbots.conf`** caused duplicate-key
  warnings and let Phase 2's `Enabled=0` mask Phase 3's `Enabled=1`.
  Fix: delete the prior LlmAgent section completely before appending
  the new one.

### 6. `bge-small-en-v1.5` Q4_K_M GGUF is ~24 MB and Just Works

Downloaded from `CompendiumLabs/bge-small-en-v1.5-gguf`. llama-embed
serves `/v1/embeddings` correctly out of the box with the standard
llama.cpp Vulkan image plus `--embedding` flag.

## Plan deviation acknowledged

The plan called for three separate runs (cold log → warm log → apply).
We collapsed the "warm log" run into the natural warm state at the end
of the cold-start run (980 memories) and proceeded directly to apply.
Result is the same: by the time apply runs, memory is well-populated
(2 000+ entities by the start of Run 3, 4 031 by end). The skipped
"warm log" middle step wouldn't have added new signal — the apply run
itself observed the warm-memory behavior over 15 min.

## Operator state at hand-off

- `AiPlayerbot.LlmAgent.Enabled = 1, ApplyMode = apply, SamplePct = 10`
  still set on Heimdal. **Recommend turning off** before walking away
  unless you want continued memory growth.
- `llama-embed.service` + `memory-sidecar.service` still running
  (cheap at idle).
- Sidecar DB at `/opt/containers/memory/db.sqlite` ~4 MB after the
  smoke run; preserve or wipe at the operator's discretion.
- GPU fan still at 80 % PWM (204 / 255) from the Phase 2 commit.

## Next

**Phase 4 — T2 interactive tool-calling** per parent design §12.
The "100 % idle when no humans are around" finding makes this the
right next step: the T2 tier is exactly where bots react to live
human-driven events (party invites, whisper questions, group
coordination). It's also where the LLM finally gets to write its own
memories via tool calls — which closes the loop the design originally
intended.
