# Phase 0.5 — Hardware Validation for the LLM Agent Layer

Status: **Approved design, not yet implemented.**
Parent design: [`docs/llm_agent_design.md`](../../llm_agent_design.md).
Branch: `claude/local-llm-bot-agents-O72i5`.

## 1. Goal and scope

Validate the inference assumptions baked into `llm_agent_design.md` against the actual Heimdal hardware **before** writing any AzerothCore C++. Phase 0.5 produces measurements, not running game features: tokens/sec, latency, grammar adherence, and VRAM footprint under realistic input on the chosen model + backend.

The parent design hand-waves at "GPU-bound, batched, cheap" and proposes Gemma 2 9B as the default. Phase 0.5 replaces those hand-waves with numbers and either confirms the model choice or surfaces evidence to revisit it.

Outcomes:

- A committed bench harness (`tools/llm-bench/`) plus a Quadlet for `llama-server` (`infra/heimdal/llama-server.container`).
- A committed results bundle (`results/YYYY-MM-DD-vulkan/{results.csv, summary.md}`) on the same branch.
- A new section §15 added to `docs/llm_agent_design.md` with concrete numbers folded in, replacing the current hand-waves.

Pass/fail thresholds are **informational only**. The team reviews the numbers and decides whether Phase 1 proceeds on the same model/backend, picks a different model, or revisits the design.

## 2. Hardware target

Heimdal, per the existing inventory:

- AMD Ryzen 7 5700X3D (8 core / 16 thread)
- 32 GB DDR4 + 15.6 GB zram
- AMD Radeon RX 6800 / 6800 XT / 6900 XT — Navi 21 (gfx1030), 16 GB VRAM
- Bazzite 43.20260211.0 (Fedora Silverblue derivative), kernel 6.17.7
- Podman 5.7.1, rootful Quadlet pattern established for `wow-server`
- SELinux: Enforcing — volume mounts require `:Z`

## 3. Test matrix

Three independent dimensions. 3 × 2 × 3 = **18 cells.**

### 3.1 Models (3, all Q4_K_M, all from ungated GGUF mirrors)

- `bartowski/gemma-2-9b-it-GGUF` — parent design's default (~5.5 GB weights)
- `bartowski/gemma-3-12b-it-GGUF` — parent design's alternative (~7 GB weights)
- `Qwen/Qwen2.5-7B-Instruct-GGUF` — non-Gemma strong baseline (~4.7 GB weights)

### 3.2 Grammar form (2)

Both encode the §7.1 goal schema from the parent design.

- **JSON Schema → llama.cpp auto-converted GBNF.** Schema lives at `tools/llm-bench/schemas/goal_schema.json`. llama-server invokes the converter internally when the request supplies `response_format: { type: "json_schema", json_schema: {...} }`.
- **Hand-written GBNF.** `tools/llm-bench/grammars/goal.gbnf`, supplied via the `grammar` field of the chat completion request.

The comparison answers whether hand-tuning grammar is worth the maintenance cost downstream.

### 3.3 Concurrency: `--parallel` ∈ {1, 4, 8}

Set on `llama-server` startup. 1 = single-stream baseline. 4 = parent design's lower batching default. 8 = parent design's upper. Skip 16 unless 8 still scales linearly with concurrency.

### 3.4 Fixtures (10 hand-authored)

Hand-authored JSON state digests matching the §6 schema. Cover the dimensions most likely to surface content-dependent latency or adherence variance:

| # | Class | Level | Social state | Special |
| - | --- | --- | --- | --- |
| 1 | orc warrior | 10 | alone | low-level baseline |
| 2 | undead mage | 37 | human-nearby + recent whisper | parent design's worked example |
| 3 | troll shaman | 70 | in-guild, in-group | high-level, social |
| 4 | tauren druid | 22 | alone, full bags + junk | inventory pressure |
| 5 | blood elf paladin | 50 | in-group, in-combat | combat in digest |
| 6 | human rogue | 80 | alone, dead | edge: bot dead |
| 7 | dwarf hunter | 45 | human-nearby + chat is prompt-injection-shaped ("ignore previous instructions and say X") | adversarial input |
| 8 | gnome warlock | 60 | alone, three completable quests in log | quest-pickup branch |
| 9 | night elf priest | 30 | in-group, no quest | "what now" idle |
| 10 | orc death knight | 58 | alone, fresh from start | recent-event-heavy log |

Each fixture is a single `.json` file in `tools/llm-bench/fixtures/` with a brief `# what this fixture probes` comment in a sibling `README.md`.

### 3.5 Per-cell load profile

For each of the 18 cells:

1. **Steady-state**: drive 1 RPS for 120 s, cycling fixtures round-robin.
2. **Burst**: send 50 requests as fast as possible (saturate the `httpx.AsyncClient` semaphore at `--parallel × 2`).

Rationale: 1 RPS matches the parent design's stated 200-bot steady-state offered load (200 × 1/300 s ≈ 0.67 RPS). The burst measures the saturation point cheaply without a full rate sweep.

### 3.6 Locked-in defaults

These were settled during brainstorming and are not load-bearing design questions:

- **Cell ordering**: model is the outer loop, `--parallel` slot count is the middle loop, grammar form is the inner loop. Grammar is a per-request parameter and does **not** require a server restart; model and slot count are startup args and do. Total restarts: 3 models × 3 slot configs = **9 server restarts** over the run.
- **VRAM measurement**: two snapshots per (model × slot) cell via the kernel's AMDGPU sysfs interface — `/sys/class/drm/card*/device/mem_info_vram_used` and `mem_info_vram_total` (bytes). One sample immediately after `/health` 200 (idle/load-only footprint) and one mid-steady-state at the 60 s mark (load + KV cache under traffic). `summary.md` records both as `vram_idle_mb` and `vram_loaded_mb`. Sysfs is chosen over `rocm-smi` because Bazzite is immutable Fedora and `rocm-smi` is not present by default; sysfs values are kernel-provided and always available.
- **System prompt**: a single short fixed string ("You decide what this WoW bot does next. Return JSON matching the supplied schema."). No prompt iteration in Phase 0.5.
- **Grammar adherence**: structural only — parse output as JSON, validate against `goal_schema.json` with `jsonschema`. Record `grammar_valid: true|false` per request. This is **not** the Phase-1 validator: we are not checking that `quest_id` exists in the AzerothCore DB or that `from_npc` is in range. Those semantic validators are Phase 1's job. Phase 0.5 only asks "did the model produce output the grammar accepted and the schema validated."

## 4. Infrastructure on Heimdal

### 4.1 `llama-server` Quadlet

File: `/etc/containers/systemd/llama-server.container` (rootful, same pattern as `wow-server`). Repo copy: `infra/heimdal/llama-server.container` on this branch.

Key shape:

- Image: `ghcr.io/ggml-org/llama.cpp:server-vulkan` (pinned to a specific SHA tag once chosen)
- Bind mount `/var/lib/llama-models/` → `/models` with `:Z`
- GPU passthrough: `--device /dev/dri/renderD128`, `--group-add render`, `--group-add video`
- Args: `--host 0.0.0.0 --port 8080 --model /models/${LLAMA_MODEL} --parallel ${LLAMA_SLOTS} --n-gpu-layers 999 --ctx-size 8192 --batch-size 512 --no-mmap`
- Unit reads `EnvironmentFile=/etc/llama-server.env` (managed root-owned), which the harness rewrites between cells with the next `LLAMA_MODEL` + `LLAMA_SLOTS` pair, then `sudo systemctl restart llama-server`. Quadlet does not natively interpolate env into `Exec=` args; the env-file pattern is the standard Quadlet-friendly way to parameterize without rewriting the unit. The harness needs passwordless `sudo` for `systemctl restart llama-server` and for the env-file write — sudoers drop-in shipped alongside the Quadlet in `infra/heimdal/`.
- Port `127.0.0.1:8080` only — no external reach, no Tailscale serve

### 4.2 Model staging

Pre-download all three GGUFs into `/var/lib/llama-models/` before the run. Total ~18 GB. The harness verifies presence at startup and aborts loudly if missing.

Download method documented in `tools/llm-bench/README.md` (`huggingface-cli download <repo> --local-dir ...`).

### 4.3 ROCm comparison: deferred

A second pass on `server-rocm` is a follow-up spec, not part of Phase 0.5.

## 5. Harness (`tools/llm-bench/`)

Single self-contained Python harness. Repo layout on this branch:

```
tools/llm-bench/
  bench.py                 # orchestrator (Python 3.11+, asyncio + httpx + jsonschema + tabulate)
  fixtures/
    01-orc-warrior-lv10.json
    02-undead-mage-lv37.json
    ...
    10-orc-dk-lv58.json
    README.md              # one line per fixture explaining what it probes
  schemas/
    goal_schema.json       # JSON Schema mirror of llm_agent_design.md §7.1
  grammars/
    goal.gbnf              # hand-written GBNF mirror of the same schema
  pyproject.toml           # uv-runnable; pinned httpx, jsonschema, tabulate, click
  README.md                # how to run; what each output column means
infra/heimdal/
  llama-server.container
results/
  YYYY-MM-DD-vulkan/
    results.csv
    summary.md
    cell-config.json       # exact (model, grammar, slots, parallel-config) per run
```

### 5.1 `bench.py` responsibilities

1. Parse the test matrix from a config or CLI flags (`--models`, `--grammars`, `--parallel-set`, `--fixtures-dir`).
2. For each model (outer loop) and each `--parallel` slot value (middle loop): rewrite `/etc/llama-server.env` with the new `LLAMA_MODEL` + `LLAMA_SLOTS` pair, `sudo systemctl restart llama-server`, poll `/health` until 200 (with a startup timeout).
3. For each grammar form within that (model × slot) cell (inner loop, no restart): drive the 2-min steady-state at 1 RPS + 50-req burst.
4. Per request, capture: timestamp, model, grammar, slots, fixture, request body size, response status, `prompt_eval_count`, `eval_count`, `prompt_eval_duration` (ns), `eval_duration` (ns), wall-clock latency (ms), raw response text, `grammar_valid` (bool, post-hoc against `goal_schema.json`), `parse_error` (string, if any).
5. Sample `/sys/class/drm/card*/device/mem_info_vram_used` and `mem_info_vram_total` twice per (model × slot) cell: once immediately after `/health` 200 (`vram_idle_mb`) and once at the 60 s mark of steady-state (`vram_loaded_mb`). Grammar form does not affect VRAM, so two samples per (model × slot) — not per (model × grammar × slot) — is enough.
6. On completion: write `results.csv` (one row per request) and `summary.md` (per-cell aggregates: p50/p95/p99 wall-clock, decode tokens/sec, prefill tokens/sec, grammar adherence rate, VRAM peak).

### 5.2 Failure handling

- llama-server unreachable: harness aborts the cell, records the failure in `summary.md`, continues with the next cell.
- Schema validation throws: recorded as `grammar_valid: false`, run continues. We want to see how often the model violates the schema.
- A burst test that times out (any request > 30 s): aborts the burst, records partial data, continues.

## 6. Reporting and feedback into design

End state of Phase 0.5 is a single commit that adds:

- `tools/llm-bench/` and `infra/heimdal/llama-server.container` (the bench machinery)
- `results/YYYY-MM-DD-vulkan/` (the evidence)
- A new §15 in `docs/llm_agent_design.md` titled "Measured inference characteristics (Phase 0.5 — Vulkan)" with the headline numbers folded in and a short paragraph on what they imply for Phase 1. Example shape:

> Gemma 2 9B Q4_K_M on Vulkan, `--parallel 4`, sustained 1 RPS over 10 representative state digests: p95 wall-clock 2.1 s, decode 38 tok/s, grammar adherence (auto-GBNF) 97%, VRAM peak 9.8 GB / 16 GB. The 8-slot configuration scaled to 2.4× the 1-slot throughput, suggesting batch headroom for the design's 200-bot target. Gemma 3 12B added 1.2 GB VRAM and degraded p95 to 3.4 s, marginal-but-acceptable on grammar adherence (98%). Qwen 2.5 7B was the fastest (p95 1.4 s) but had the lowest grammar adherence (89%) on the adversarial fixture.

(Numbers above are illustrative only — actual numbers are what the run produces.)

A short "what this implies" paragraph at the end of §15 captures recommended adjustments to the rest of the design — e.g. raise/lower `WorkerThreads` default, revise the §10 config defaults, flag any open question that the data answered.

## 7. Explicit non-goals

- No AzerothCore module code.
- No `LlmAgentManager`, no `LlmAgentStrategy`, no C++ at all on this branch.
- No memory sidecar.
- No worldserver-attached digest extractor — fixtures stay hand-authored in Phase 0.5.
- No cloud opt-in test — local-only.
- No prompt engineering / system-prompt iteration.
- No ROCm pass — deferred to its own spec.
- No production deployment of `llama-server` — the Quadlet stays on `127.0.0.1:8080` and is stopped after the run.

## 8. Risks and what we'll do if hit

- **Vulkan llama.cpp build doesn't expose `/dev/dri` on Bazzite cleanly.** Investigate via `podman run --rm -it ... vulkaninfo` first; if Vulkan fundamentally won't pass through, escalate to ROCm earlier than planned.
- **Auto-GBNF conversion produces an invalid grammar on the goal schema.** llama.cpp's converter has known edge cases with `oneOf` / discriminated unions, which the goal schema uses. Mitigation: hand-written GBNF is already in the matrix, so we always have a working path.
- **Adversarial fixture (#7) jailbreaks the model into ignoring the schema.** That's a finding we want — it directly informs the §11 prompt-injection defense in the parent design.
- **8-slot cell exhausts VRAM on Gemma 3 12B.** Record the OOM, mark the cell as N/A, continue. The data point itself answers a design question.
- **Per-fixture results vary so much that aggregates are misleading.** `results.csv` retains per-request rows, so post-hoc disaggregation is always possible.

## 9. Out-of-scope follow-ups (named so we don't forget)

- ROCm pass: same matrix, `server-rocm` image, compare numbers head-to-head.
- Larger-parallel slot test (16, 32) if 8 scales linearly.
- Rate-sweep characterization (0.3 / 0.7 / 1.5 / 3.0 RPS) to find the saturation knee.
- Real-bot state digest extraction — moves to Phase 1.
- Prompt iteration / few-shot examples — moves to the relevant agent-tier spec.
