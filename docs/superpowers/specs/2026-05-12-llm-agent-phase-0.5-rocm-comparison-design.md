# Phase 0.5 — ROCm Comparison Pass

Status: **Approved design, not yet implemented.**
Parent design: [`docs/llm_agent_design.md`](../../llm_agent_design.md) §15.
Vulkan baseline: [`results/2026-05-12-vulkan/`](../../../results/2026-05-12-vulkan/).
Branch: `claude/local-llm-bot-agents-O72i5`.

## 1. Goal and scope

The Phase 0.5 Vulkan run named Qwen 2.5 7B + `json_schema` + `--parallel 4` as the production config. This pass measures whether the same cell on ROCm is materially faster or slower. Single decision: does the production stack stay on Vulkan, or switch to ROCm?

Outcomes:

- A new `infra/heimdal/llama-server-rocm.container` Quadlet (and supporting sudoers entries).
- A new `--rocm` flag on `bench.py` that targets the ROCm Quadlet instead of the Vulkan one.
- A single committed results bundle at `results/YYYY-MM-DD-rocm/{results.csv, summary.md}`.
- A short §15.8 added to `docs/llm_agent_design.md` with the head-to-head numbers and the decision (stay on Vulkan or switch to ROCm).

No predefined pass/fail threshold — same as Phase 0.5. The team eyeballs the numbers post-hoc and decides.

## 2. Scope

**One cell only**: Qwen 2.5 7B Q4_K_M, `--parallel 4`, `json_schema` grammar, steady (60 s at 1 RPS) + burst (50 requests at concurrency 8). Fixtures: the same 10 hand-authored digests from `tools/llm-bench/fixtures/`.

Explicit non-scope (all deferred):

- No Gemma 2 9B or Gemma 3 12B cells.
- No `gbnf` grammar cells.
- No `--parallel 1` or `--parallel 8` cells.
- No new fixtures.
- No spec for "what to do if ROCm wins" — that's a follow-up.

The matrix is **1 cell** producing **2 summary rows** (steady, burst) and ~110 per-request rows.

## 3. ROCm container setup

### 3.1 Image

`ghcr.io/ggml-org/llama.cpp:server-rocm` (pinned to a specific SHA tag once selected). Image size ~8 GB; pulled once and cached on Heimdal.

### 3.2 Device passthrough

ROCm needs both `/dev/kfd` (compute device) and `/dev/dri/renderD128` (display/render). The container also needs `render`, `video`, and (on some setups) `kfd` group memberships. Approach mirrors the Vulkan Quadlet pattern:

- `AddDevice=/dev/kfd`
- `AddDevice=/dev/dri/renderD128`
- `GroupAdd=105` (render, host GID — verified in Phase 0.5 Task 4)
- `GroupAdd=39` (video, host GID — verified in Phase 0.5 Task 4)

If a `kfd` group exists on Heimdal at deploy time, add its numeric GID as a third `GroupAdd`. Otherwise the kfd device is typically world-accessible via render/video.

### 3.3 GPU-arch fallback

The RX 6900 XT is Navi 21 (gfx1030), officially supported by ROCm 6.x. As a defensive measure for older or stripped ROCm images, set `HSA_OVERRIDE_GFX_VERSION=10.3.0` in the env file. The env var is a no-op when not needed.

### 3.4 Quadlet shape

File: `/etc/containers/systemd/llama-server-rocm.container` (rootful, sibling of the existing Vulkan unit). Repo copy: `infra/heimdal/llama-server-rocm.container`.

Same env-file pattern as the Vulkan unit, but with a separate env file (`/etc/llama-server-rocm.env`) so the two services do not collide. `bench.py --rocm` rewrites that file rather than `/etc/llama-server.env`.

Same port (`127.0.0.1:8080`) — the two services are mutually exclusive at the port level. Harness ensures one or the other is running at any time.

## 4. Sudoers additions

Add three NOPASSWD entries to `infra/heimdal/sudoers-llama-server` covering the ROCm service:

```
brackin ALL=(root) NOPASSWD: /usr/bin/systemctl restart llama-server-rocm.service
brackin ALL=(root) NOPASSWD: /usr/bin/systemctl start llama-server-rocm.service
brackin ALL=(root) NOPASSWD: /usr/bin/systemctl stop llama-server-rocm.service
brackin ALL=(root) NOPASSWD: /usr/bin/install -m 0644 /tmp/llama-server-rocm.env.next /etc/llama-server-rocm.env
```

`power1_cap` sudo entry from the Vulkan rerun stays unchanged and applies to both runs.

## 5. Harness changes

### 5.1 `bench.py --rocm` flag

A single boolean flag selects the target backend. When `--rocm` is set:

- **Pre-flight**: `sudo -n systemctl stop llama-server.service` (the Vulkan service) before any ROCm restart, since both bind `127.0.0.1:8080`. Best-effort (no-op if already stopped). Symmetrically, `--rocm`-off mode pre-flights a stop on `llama-server-rocm.service`. Both stops are idempotent.
- `write_env_and_restart()` writes `/tmp/llama-server-rocm.env.next` and `sudo install`s it to `/etc/llama-server-rocm.env`.
- Restart target is `llama-server-rocm.service`.
- `wait_for_health()` polls the same endpoint (port 8080).
- `--stop-server-on-exit` stops the ROCm service (not Vulkan).
- Run metadata records `backend: rocm`.

Implementation: `server.py` gains a `backend` parameter on `write_env_and_restart` and `stop_server` (default `"vulkan"` preserves existing behavior). The new pre-flight stop is part of `write_env_and_restart`, so callers don't need to manage it. No module split.

### 5.2 Single-cell mode

Add a `--single-cell` flag that scopes the matrix to one (model, slots, grammar) combination. Default off. Used for this ROCm pass.

Implementation: `MODELS`, `PARALLEL_SLOTS`, `GRAMMAR_MODES` get filtered when `--single-cell` is set. The filter is hard-coded to the production config (Qwen 2.5 7B, slots=4, json_schema) to avoid an explosion of new CLI options. If a different single cell is ever wanted, that's a separate spec.

### 5.3 No changes to fixtures, schema, GBNF, output writers, or matrix loop

The orchestrator loop already iterates over the filtered sets — single-cell just means "one element each." Output paths still come from `--out-dir`.

## 6. Run procedure

1. Deploy `llama-server-rocm.container`, sudoers update, and a fresh env-file to Heimdal.
2. Pull the ROCm image once on Heimdal (`podman pull` — ~8 GB, takes a minute or two).
3. Smoke-test: `sudo systemctl start llama-server-rocm.service`; poll `/health` 200; send one trivial completion to confirm the GPU is actually used (check `journalctl -u llama-server-rocm.service` for "llama_init_from_model" + Vulkan/ROCm device line).
4. If startup fails with "no GPU found," set `HSA_OVERRIDE_GFX_VERSION=10.3.0` in the env file and retry. If still fails, escalate — the brainstorm assumed RX 6900 XT is supported; if not, the spec needs revision.
5. Run `bench.py --rocm --single-cell --out-dir results/YYYY-MM-DD-rocm`. Wall-clock ~5 min.
6. Pull results to Mac, commit, and write §15.8 in the design doc with head-to-head numbers.

## 7. Reporting and feedback into design

End state: a single commit that adds:

- `infra/heimdal/llama-server-rocm.container` + updated sudoers.
- Modified `bench.py` (`--rocm`, `--single-cell`) and `server.py` (backend parameter).
- `results/YYYY-MM-DD-rocm/{results.csv, summary.md}` (the head-to-head evidence).
- A new §15.8 in `docs/llm_agent_design.md` titled "ROCm vs Vulkan — head-to-head" with a short narrative + decision.

If ROCm clearly wins: update §15.6 to recommend ROCm for production. If Vulkan wins or they tie: explicitly close the open follow-up from §15.7 with a "tested, Vulkan stays" note.

## 8. Explicit non-goals

- No AzerothCore module code (still Phase 0.5 territory).
- No new harness modules.
- No multi-cell ROCm matrix (deferred to a future spec only if A is inconclusive — Option B in the brainstorm).
- No comparison against a third backend (Vulkan + Mesa SubGroups, Vulkan + Coopmat, OpenCL, etc.).
- No re-running the full Vulkan matrix for re-validation. The 2026-05-12 Vulkan numbers are the baseline.

## 9. Risks and what we'll do if hit

- **ROCm image won't start with default device passthrough.** First retry path: `HSA_OVERRIDE_GFX_VERSION=10.3.0`. Second: investigate `/dev/kfd` permissions and any missing `kfd` group. Third: escalate, since the brainstorm's premise was "RX 6900 XT is supported."
- **ROCm runs but is 50%+ slower than Vulkan.** Surprising; likely indicates the gfx1030 kernel isn't actually being used (falling back to host CPU?). Check `journalctl` for the loaded device line. Record and document.
- **VRAM footprint changes meaningfully.** ROCm has different KV-cache allocation patterns; the same model at slots=4 may show 10-20% different `vram_loaded_mb`. Record; not a blocker.
- **Image pull fails or is slow.** ~8 GB image; if Heimdal's internet is slow, this is the longest part of the run. Pull happens once.
- **ROCm container hangs at startup.** Has been a historical pain point. Mitigation: `TimeoutStartSec=300` in the Quadlet `[Service]` block (5 min instead of the Vulkan 120 s).

## 10. Out-of-scope follow-ups (named so we don't forget)

- If ROCm wins on the single cell: rerun the full matrix on ROCm for parity with Vulkan results (Option B / C from the brainstorm).
- Investigate Vulkan vs ROCm KV-cache behavior differences (if VRAM patterns diverge).
- Power efficiency comparison (W/tok, not just tok/s) — relevant if 24/7 production cost matters.
- ROCm-specific tuning knobs (`--threads-batch`, `--n-cpu-moe`) — out of scope for the single-cell pass.
