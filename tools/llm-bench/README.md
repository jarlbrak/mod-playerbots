# llm-bench — Phase 0.5 hardware validation

Characterizes `llama-server` inference on Heimdal for the LLM agent layer
(see `docs/superpowers/specs/2026-05-12-llm-agent-phase-0.5-hardware-validation-design.md`).

## What it does

Iterates over a 3-model × 2-grammar × 3-parallelism matrix. For each cell:

1. Rewrites `/etc/llama-server.env` and `sudo systemctl restart llama-server` (on model or slot change only).
2. Waits for `/health` 200.
3. For each grammar form: drives 120 s @ 1 RPS steady-state + 50-req burst.
4. Samples VRAM via `/sys/class/drm/card1/device/mem_info_vram_*` twice per (model, slots) cell.
5. Validates each response against `schemas/goal_schema.json` post-hoc.

Outputs `results.csv` (per-request) and `summary.md` (per-cell aggregates) into `--out-dir`.

## Prerequisites

This bench runs **on Heimdal**, not on your dev machine. Prep is done in Tasks 1-4 of
`docs/superpowers/plans/2026-05-12-llm-agent-phase-0.5-hardware-validation.md`:

- `/var/lib/llama-models/` contains the three GGUFs (Gemma 2 9B, Gemma 3 12B, Qwen 2.5 7B shard-split).
- `/etc/containers/systemd/llama-server.container` is installed.
- `/etc/llama-server.env` has initial values.
- `/etc/sudoers.d/llama-server` grants `brackin` passwordless access to the four required commands.
- `llama-server.service` is registered (`systemctl daemon-reload` has been run).

## Running

From inside `~/llm-bench-workspace/mod-playerbots/tools/llm-bench/` on Heimdal:

```bash
# Dry run (fast smoke test, ~1 min)
uv run bench.py --dry-run --out-dir ../../results/$(date +%Y-%m-%d)-vulkan-dry

# Full matrix (~60 min)
uv run bench.py --out-dir ../../results/$(date +%Y-%m-%d)-vulkan --stop-server-on-exit
```

## Output column meanings

`results.csv` columns:

| Column | Meaning |
| --- | --- |
| `timestamp` | UTC ISO timestamp of request send |
| `model` | model label (gemma-2-9b / gemma-3-12b / qwen-2.5-7b) |
| `grammar` | grammar mode (json_schema / gbnf) |
| `slots` | `--parallel` value llama-server was started with |
| `fixture` | fixture name (file stem, e.g. `02-undead-mage-lv37`) |
| `phase` | `steady` or `burst` |
| `request_body_bytes` | size of the JSON payload sent |
| `response_status` | HTTP response code (0 = client-side error) |
| `prompt_eval_count` | prefill tokens (input) |
| `eval_count` | decode tokens (output) |
| `prompt_eval_duration_ns` | prefill duration in ns (from llama.cpp `/timings`) |
| `eval_duration_ns` | decode duration in ns |
| `wall_clock_ms` | end-to-end client-observed latency in ms |
| `grammar_valid` | post-hoc JSON Schema validation result |
| `parse_error` | error string if `grammar_valid=false`, else empty |
| `raw_response` | model output content |

`summary.md` columns are per-cell aggregates (p50/p95/p99 wall ms, mean decode and prefill tok/s, adherence percentage, VRAM idle + loaded in MB).

## Limitations

- Phase 0.5 structural validation only — Phase 1's semantic validators (quest IDs exist in DB, NPCs in range, etc.) are out of scope.
- The Heimdal AMD GPU card device is hard-coded as `/sys/class/drm/card1/device` in `bench.py`. If `lspci`/`/sys/class/drm` enumeration differs after a kernel update, adjust `HEIMDAL_AMD_CARD_DEVICE`.
- Vulkan backend only — ROCm comparison is a follow-up spec.
