#!/usr/bin/env -S uv run --project tools/llm-bench
"""Phase 0.5 hardware-validation bench entrypoint."""

import asyncio
import functools
import json
import sys
from dataclasses import dataclass
from datetime import date
from pathlib import Path

import click
import httpx

from llmbench.aggregate import (
    adherence_rate,
    decode_tokens_per_sec,
    percentile,
    prefill_tokens_per_sec,
)
from llmbench.driver import run_burst, run_steady_state
from llmbench.gpu import restore_power_cap, sample_gpu_temps, set_power_cap_watts
from llmbench.output import CellSummary, ResultRow, write_results_csv, write_summary_md
from llmbench.request import build_request
from llmbench.server import stop_server, wait_for_health, write_env_and_restart
from llmbench.vram import sample_vram


# --- matrix definition -------------------------------------------------------

@dataclass(frozen=True)
class ModelSpec:
    label: str  # short label used in results
    gguf_filename: str  # filename in /var/lib/llama-models/


MODELS = [
    ModelSpec("gemma-2-9b", "gemma-2-9b-it-Q4_K_M.gguf"),
    ModelSpec("gemma-3-12b", "gemma-3-12b-it-Q4_K_M.gguf"),
    ModelSpec("qwen-2.5-7b", "qwen2.5-7b-instruct-q4_k_m-00001-of-00002.gguf"),
]

PARALLEL_SLOTS = [1, 4, 8]
GRAMMAR_MODES = ["json_schema", "gbnf"]
STEADY_RPS = 1.0
STEADY_DURATION_S = 60.0  # was 120 s; halved after thermal trip in first run attempt
BURST_REQUESTS = 50
REQUEST_TIMEOUT_S = 30.0
HEIMDAL_AMD_CARD_DEVICE = "/sys/class/drm/card1/device"  # confirmed in Task 9 step 5
HEIMDAL_AMD_HWMON_DIR = "/sys/class/hwmon/hwmon2"  # amdgpu, for temp + power_cap

# Thermal mitigations after first-attempt thermal trip:
# 237 W is the lowest power_cap_min the AMD firmware permits on this card
# (264 W default; firmware refuses lower). Combined with fans pegged at PWM
# max via gpu-fan-max.service, this is the maximum heat reduction we can
# software-enforce.
GPU_POWER_CAP_WATTS = 237
INTER_CELL_COOLING_S = 30.0  # idle pause between (model x slots) cells


# --- helpers -----------------------------------------------------------------


def load_fixtures(fixtures_dir: Path) -> list[tuple[str, dict]]:
    pairs = []
    for path in sorted(fixtures_dir.glob("*.json")):
        with open(path) as f:
            pairs.append((path.stem, json.load(f)))
    return pairs


def load_schema(path: Path) -> dict:
    with open(path) as f:
        return json.load(f)


def load_gbnf(path: Path) -> str:
    return path.read_text()


def aggregate_cell(
    rows: list[ResultRow],
    model: str,
    grammar: str,
    slots: int,
    phase: str,
    vram_idle_mb: int,
    vram_loaded_mb: int,
    gpu_edge_c_loaded: float,
    gpu_junction_c_loaded: float,
) -> CellSummary:
    cell_rows = [r for r in rows if r.model == model and r.grammar == grammar and r.slots == slots and r.phase == phase]
    if not cell_rows:
        return CellSummary(
            model=model, grammar=grammar, slots=slots, phase=phase,
            n_requests=0, p50_wall_ms=0, p95_wall_ms=0, p99_wall_ms=0,
            decode_toks_per_sec=0, prefill_toks_per_sec=0, adherence=0,
            vram_idle_mb=vram_idle_mb, vram_loaded_mb=vram_loaded_mb,
            gpu_edge_c_loaded=gpu_edge_c_loaded,
            gpu_junction_c_loaded=gpu_junction_c_loaded,
        )
    wall = [r.wall_clock_ms for r in cell_rows]
    decode_rates = [
        decode_tokens_per_sec(r.eval_count, r.eval_duration_ns)
        for r in cell_rows
        if r.eval_duration_ns > 0
    ]
    prefill_rates = [
        prefill_tokens_per_sec(r.prompt_eval_count, r.prompt_eval_duration_ns)
        for r in cell_rows
        if r.prompt_eval_duration_ns > 0
    ]
    return CellSummary(
        model=model,
        grammar=grammar,
        slots=slots,
        phase=phase,
        n_requests=len(cell_rows),
        p50_wall_ms=percentile(wall, 50),
        p95_wall_ms=percentile(wall, 95),
        p99_wall_ms=percentile(wall, 99),
        decode_toks_per_sec=(sum(decode_rates) / len(decode_rates)) if decode_rates else 0,
        prefill_toks_per_sec=(sum(prefill_rates) / len(prefill_rates)) if prefill_rates else 0,
        adherence=adherence_rate([r.grammar_valid for r in cell_rows]),
        vram_idle_mb=vram_idle_mb,
        vram_loaded_mb=vram_loaded_mb,
        gpu_edge_c_loaded=gpu_edge_c_loaded,
        gpu_junction_c_loaded=gpu_junction_c_loaded,
    )


def flush_incremental(out_dir: Path, all_rows: list[ResultRow], summaries: list[CellSummary], run_metadata: dict) -> None:
    """Write current state of results.csv and summary.md. Called after each cell."""
    out_dir.mkdir(parents=True, exist_ok=True)
    write_results_csv(out_dir / "results.csv", all_rows)
    write_summary_md(out_dir / "summary.md", summaries, run_metadata)


# --- main -------------------------------------------------------------------


async def run_matrix(
    *,
    endpoint: str,
    fixtures_dir: Path,
    schema_path: Path,
    gbnf_path: Path,
    out_dir: Path,
    dry_run: bool,
    single_cell: bool = False,
    backend: str = "vulkan",
) -> None:
    fixtures = load_fixtures(fixtures_dir)
    schema = load_schema(schema_path)
    gbnf = load_gbnf(gbnf_path)

    all_rows: list[ResultRow] = []
    summaries: list[CellSummary] = []

    if single_cell:
        # Production cell: Qwen 2.5 7B + slots=4 + json_schema. The Phase 0.5
        # winner; used for the ROCm comparison pass.
        models_to_run = [m for m in MODELS if m.label == "qwen-2.5-7b"]
        slots_to_run = [4]
        grammars_to_run = ["json_schema"]
        steady_duration = STEADY_DURATION_S
        burst_n = BURST_REQUESTS
        cooling_s = 0.0  # only one cell — no need for a cooling pause
    elif dry_run:
        models_to_run = MODELS[:1]
        slots_to_run = [4]
        grammars_to_run = ["json_schema"]
        steady_duration = 10.0
        burst_n = 5
        cooling_s = 5.0
    else:
        models_to_run = MODELS
        slots_to_run = PARALLEL_SLOTS
        grammars_to_run = GRAMMAR_MODES
        steady_duration = STEADY_DURATION_S
        burst_n = BURST_REQUESTS
        cooling_s = INTER_CELL_COOLING_S

    run_metadata = {
        "date": date.today().isoformat(),
        "backend": backend,
        "endpoint": endpoint,
        "models": ",".join(m.label for m in models_to_run),
        "slots": ",".join(str(s) for s in slots_to_run),
        "grammars": ",".join(grammars_to_run),
        "steady_rps": STEADY_RPS,
        "steady_duration_s": steady_duration,
        "burst_requests": burst_n,
        "gpu_power_cap_watts": GPU_POWER_CAP_WATTS,
        "inter_cell_cooling_s": cooling_s,
        "dry_run": dry_run,
        "single_cell": single_cell,
    }

    # Apply GPU power cap before any GPU work; restore on exit.
    click.echo(f"[matrix] setting GPU power cap to {GPU_POWER_CAP_WATTS} W")
    set_power_cap_watts(GPU_POWER_CAP_WATTS, hwmon_dir=HEIMDAL_AMD_HWMON_DIR)

    try:
        async with httpx.AsyncClient(http2=True) as client:
            cell_idx = 0
            total_cells = len(models_to_run) * len(slots_to_run)
            for model in models_to_run:
                for slots in slots_to_run:
                    cell_idx += 1
                    click.echo(f"[matrix] starting cell {cell_idx}/{total_cells} model={model.label} slots={slots}")
                    write_env_and_restart(model.gguf_filename, slots, backend=backend)
                    await wait_for_health(endpoint)
                    vram_idle_mb, vram_total_mb = sample_vram(HEIMDAL_AMD_CARD_DEVICE)
                    idle_temps = sample_gpu_temps(HEIMDAL_AMD_HWMON_DIR)
                    click.echo(
                        f"[matrix]   /health 200; vram_idle={vram_idle_mb}MB / {vram_total_mb}MB; "
                        f"edge={idle_temps['edge_c']:.1f}C junction={idle_temps['junction_c']:.1f}C"
                    )

                    vram_loaded_mb_for_cell: int | None = None
                    loaded_temps: dict[str, float] = {"edge_c": float("nan"), "junction_c": float("nan"), "mem_c": float("nan")}

                    for grammar in grammars_to_run:
                        click.echo(f"[matrix]   grammar={grammar} steady...")
                        builder = functools.partial(
                            build_request,
                            grammar_mode=grammar,  # type: ignore[arg-type]
                            schema=schema if grammar == "json_schema" else None,
                            gbnf=gbnf if grammar == "gbnf" else None,
                            max_tokens=256,
                            temperature=0.3,
                        )
                        steady_task = asyncio.create_task(
                            run_steady_state(
                                client=client,
                                endpoint=endpoint,
                                model=model.label,
                                grammar=grammar,
                                slots=slots,
                                phase_label="steady",
                                fixtures=fixtures,
                                request_builder=lambda fx, b=builder: b(fixture=fx),
                                schema=schema,
                                target_rps=STEADY_RPS,
                                duration_s=steady_duration,
                                timeout_s=REQUEST_TIMEOUT_S,
                            )
                        )
                        await asyncio.sleep(min(steady_duration / 2, 60.0))
                        if vram_loaded_mb_for_cell is None:
                            used_mb, _total = sample_vram(HEIMDAL_AMD_CARD_DEVICE)
                            vram_loaded_mb_for_cell = used_mb
                            loaded_temps = sample_gpu_temps(HEIMDAL_AMD_HWMON_DIR)
                            click.echo(
                                f"[matrix]   mid-steady: vram_loaded={vram_loaded_mb_for_cell}MB; "
                                f"edge={loaded_temps['edge_c']:.1f}C junction={loaded_temps['junction_c']:.1f}C mem={loaded_temps['mem_c']:.1f}C"
                            )
                        steady_rows = await steady_task
                        all_rows.extend(steady_rows)

                        click.echo(f"[matrix]   grammar={grammar} burst...")
                        burst_rows = await run_burst(
                            client=client,
                            endpoint=endpoint,
                            model=model.label,
                            grammar=grammar,
                            slots=slots,
                            fixtures=fixtures,
                            request_builder=lambda fx, b=builder: b(fixture=fx),
                            schema=schema,
                            n_requests=burst_n,
                            concurrency=slots * 2,
                            timeout_s=REQUEST_TIMEOUT_S,
                        )
                        all_rows.extend(burst_rows)

                        summaries.append(
                            aggregate_cell(
                                all_rows, model.label, grammar, slots, "steady",
                                vram_idle_mb, vram_loaded_mb_for_cell or 0,
                                loaded_temps["edge_c"], loaded_temps["junction_c"],
                            )
                        )
                        summaries.append(
                            aggregate_cell(
                                all_rows, model.label, grammar, slots, "burst",
                                vram_idle_mb, vram_loaded_mb_for_cell or 0,
                                loaded_temps["edge_c"], loaded_temps["junction_c"],
                            )
                        )

                    # Incremental flush after every (model x slots) cell — survives crashes.
                    flush_incremental(out_dir, all_rows, summaries, run_metadata)
                    click.echo(f"[matrix]   cell {cell_idx} flushed to {out_dir}/")

                    # Inter-cell cooling pause — let the GPU shed heat before the next cell.
                    if cell_idx < total_cells:
                        post_temps = sample_gpu_temps(HEIMDAL_AMD_HWMON_DIR)
                        click.echo(
                            f"[matrix]   cooling pause {cooling_s:.0f}s; "
                            f"edge={post_temps['edge_c']:.1f}C junction={post_temps['junction_c']:.1f}C"
                        )
                        await asyncio.sleep(cooling_s)

        flush_incremental(out_dir, all_rows, summaries, run_metadata)
        click.echo(f"[matrix] done — results in {out_dir}/")
    finally:
        click.echo("[matrix] restoring GPU power cap to hardware default")
        try:
            restore_power_cap(hwmon_dir=HEIMDAL_AMD_HWMON_DIR)
        except Exception as e:
            click.echo(f"[matrix] WARNING: could not restore power cap: {e}", err=True)


@click.command()
@click.option("--endpoint", default="http://127.0.0.1:8080")
@click.option("--fixtures-dir", default="fixtures", type=click.Path(exists=True, path_type=Path))
@click.option("--schema-path", default="schemas/goal_schema.json", type=click.Path(exists=True, path_type=Path))
@click.option("--gbnf-path", default="grammars/goal.gbnf", type=click.Path(exists=True, path_type=Path))
@click.option("--out-dir", required=True, type=click.Path(path_type=Path))
@click.option("--dry-run", is_flag=True, help="Smoke test: 1 model, 4 slots, json_schema only, 10 s steady, 5 burst, 5 s cooling.")
@click.option("--single-cell", is_flag=True, help="Run only the production cell (Qwen 2.5 7B + slots=4 + json_schema). Mutually exclusive with --dry-run.")
@click.option("--rocm", is_flag=True, help="Target the ROCm Quadlet (llama-server-rocm.service) instead of Vulkan.")
@click.option("--stop-server-on-exit", is_flag=True, help="Stop llama-server (the active backend) after the run.")
def main(
    endpoint: str,
    fixtures_dir: Path,
    schema_path: Path,
    gbnf_path: Path,
    out_dir: Path,
    dry_run: bool,
    single_cell: bool,
    rocm: bool,
    stop_server_on_exit: bool,
) -> None:
    """Run the Phase 0.5 characterization matrix."""
    if dry_run and single_cell:
        raise click.UsageError("--dry-run and --single-cell are mutually exclusive")
    backend = "rocm" if rocm else "vulkan"
    try:
        asyncio.run(
            run_matrix(
                endpoint=endpoint,
                fixtures_dir=fixtures_dir,
                schema_path=schema_path,
                gbnf_path=gbnf_path,
                out_dir=out_dir,
                dry_run=dry_run,
                single_cell=single_cell,
                backend=backend,
            )
        )
    finally:
        if stop_server_on_exit:
            stop_server(backend=backend)


if __name__ == "__main__":
    sys.exit(main())
