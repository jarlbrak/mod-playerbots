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
STEADY_DURATION_S = 120.0
BURST_REQUESTS = 50
REQUEST_TIMEOUT_S = 30.0
HEIMDAL_AMD_CARD_DEVICE = "/sys/class/drm/card1/device"  # confirmed in Task 9 step 5


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
) -> CellSummary:
    cell_rows = [r for r in rows if r.model == model and r.grammar == grammar and r.slots == slots and r.phase == phase]
    if not cell_rows:
        # empty cell — still emit a row so the table is complete
        return CellSummary(
            model=model, grammar=grammar, slots=slots, phase=phase,
            n_requests=0, p50_wall_ms=0, p95_wall_ms=0, p99_wall_ms=0,
            decode_toks_per_sec=0, prefill_toks_per_sec=0, adherence=0,
            vram_idle_mb=vram_idle_mb, vram_loaded_mb=vram_loaded_mb,
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
    )


# --- main -------------------------------------------------------------------


async def run_matrix(
    *,
    endpoint: str,
    fixtures_dir: Path,
    schema_path: Path,
    gbnf_path: Path,
    out_dir: Path,
    dry_run: bool,
) -> None:
    fixtures = load_fixtures(fixtures_dir)
    schema = load_schema(schema_path)
    gbnf = load_gbnf(gbnf_path)

    all_rows: list[ResultRow] = []
    summaries: list[CellSummary] = []

    models_to_run = MODELS[:1] if dry_run else MODELS
    slots_to_run = [4] if dry_run else PARALLEL_SLOTS
    grammars_to_run = ["json_schema"] if dry_run else GRAMMAR_MODES
    steady_duration = 10.0 if dry_run else STEADY_DURATION_S
    burst_n = 5 if dry_run else BURST_REQUESTS

    async with httpx.AsyncClient(http2=True) as client:
        for model in models_to_run:
            for slots in slots_to_run:
                click.echo(f"[matrix] starting cell model={model.label} slots={slots}")
                write_env_and_restart(model.gguf_filename, slots)
                await wait_for_health(endpoint)
                vram_idle_mb, vram_total_mb = sample_vram(HEIMDAL_AMD_CARD_DEVICE)
                click.echo(f"[matrix]   /health 200; vram_idle={vram_idle_mb}MB / {vram_total_mb}MB")

                vram_loaded_mb_for_cell: int | None = None
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
                    # mid-steady VRAM sample scheduled by a timer
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
                    await asyncio.sleep(min(60.0, steady_duration / 2))
                    if vram_loaded_mb_for_cell is None:
                        used_mb, _total = sample_vram(HEIMDAL_AMD_CARD_DEVICE)
                        vram_loaded_mb_for_cell = used_mb
                        click.echo(f"[matrix]   vram_loaded={vram_loaded_mb_for_cell}MB")
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
                        )
                    )
                    summaries.append(
                        aggregate_cell(
                            all_rows, model.label, grammar, slots, "burst",
                            vram_idle_mb, vram_loaded_mb_for_cell or 0,
                        )
                    )

    out_dir.mkdir(parents=True, exist_ok=True)
    write_results_csv(out_dir / "results.csv", all_rows)
    write_summary_md(
        out_dir / "summary.md",
        summaries,
        run_metadata={
            "date": date.today().isoformat(),
            "backend": "vulkan",
            "endpoint": endpoint,
            "models": ",".join(m.label for m in models_to_run),
            "slots": ",".join(str(s) for s in slots_to_run),
            "grammars": ",".join(grammars_to_run),
            "steady_rps": STEADY_RPS,
            "steady_duration_s": steady_duration,
            "burst_requests": burst_n,
            "dry_run": dry_run,
        },
    )
    click.echo(f"[matrix] done — results in {out_dir}/")


@click.command()
@click.option("--endpoint", default="http://127.0.0.1:8080")
@click.option("--fixtures-dir", default="fixtures", type=click.Path(exists=True, path_type=Path))
@click.option("--schema-path", default="schemas/goal_schema.json", type=click.Path(exists=True, path_type=Path))
@click.option("--gbnf-path", default="grammars/goal.gbnf", type=click.Path(exists=True, path_type=Path))
@click.option("--out-dir", required=True, type=click.Path(path_type=Path))
@click.option("--dry-run", is_flag=True, help="Smoke test: 1 model, 4 slots, json_schema only, 10 s steady, 5 burst.")
@click.option("--stop-server-on-exit", is_flag=True, help="Stop llama-server after the run.")
def main(
    endpoint: str,
    fixtures_dir: Path,
    schema_path: Path,
    gbnf_path: Path,
    out_dir: Path,
    dry_run: bool,
    stop_server_on_exit: bool,
) -> None:
    """Run the Phase 0.5 characterization matrix."""
    try:
        asyncio.run(
            run_matrix(
                endpoint=endpoint,
                fixtures_dir=fixtures_dir,
                schema_path=schema_path,
                gbnf_path=gbnf_path,
                out_dir=out_dir,
                dry_run=dry_run,
            )
        )
    finally:
        if stop_server_on_exit:
            stop_server()


if __name__ == "__main__":
    sys.exit(main())
