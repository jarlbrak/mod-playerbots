"""CSV + Markdown writers for Phase 0.5 results."""

import csv
from dataclasses import dataclass, asdict, fields
from pathlib import Path
from typing import Any

from tabulate import tabulate


@dataclass
class ResultRow:
    timestamp: str
    model: str
    grammar: str
    slots: int
    fixture: str
    phase: str  # "steady" | "burst"
    request_body_bytes: int
    response_status: int
    prompt_eval_count: int
    eval_count: int
    prompt_eval_duration_ns: int
    eval_duration_ns: int
    wall_clock_ms: int
    grammar_valid: bool
    parse_error: str | None
    raw_response: str


@dataclass
class CellSummary:
    model: str
    grammar: str
    slots: int
    phase: str
    n_requests: int
    p50_wall_ms: float
    p95_wall_ms: float
    p99_wall_ms: float
    decode_toks_per_sec: float
    prefill_toks_per_sec: float
    adherence: float
    vram_idle_mb: int
    vram_loaded_mb: int
    gpu_edge_c_loaded: float
    gpu_junction_c_loaded: float


def write_results_csv(path: Path, rows: list[ResultRow]) -> None:
    if not rows:
        path.write_text("")
        return
    field_names = [f.name for f in fields(ResultRow)]
    with open(path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=field_names)
        writer.writeheader()
        for r in rows:
            d = asdict(r)
            # parse_error: write empty string instead of "None" when null
            if d["parse_error"] is None:
                d["parse_error"] = ""
            writer.writerow(d)


def write_summary_md(
    path: Path,
    cells: list[CellSummary],
    run_metadata: dict[str, Any],
) -> None:
    lines: list[str] = []
    lines.append("# Phase 0.5 — Hardware Validation Run Summary")
    lines.append("")
    for k, v in run_metadata.items():
        lines.append(f"- **{k}**: {v}")
    lines.append("")
    lines.append("## Per-cell aggregates")
    lines.append("")

    headers = [
        "model",
        "grammar",
        "slots",
        "phase",
        "N",
        "p50 ms",
        "p95 ms",
        "p99 ms",
        "decode tok/s",
        "prefill tok/s",
        "adherence",
        "VRAM idle MB",
        "VRAM loaded MB",
        "edge °C",
        "junction °C",
    ]
    rows = [
        [
            c.model,
            c.grammar,
            c.slots,
            c.phase,
            c.n_requests,
            f"{c.p50_wall_ms:.0f}",
            f"{c.p95_wall_ms:.0f}",
            f"{c.p99_wall_ms:.0f}",
            f"{c.decode_toks_per_sec:.1f}",
            f"{c.prefill_toks_per_sec:.0f}",
            f"{c.adherence * 100:.1f}%",
            c.vram_idle_mb,
            c.vram_loaded_mb,
            f"{c.gpu_edge_c_loaded:.1f}",
            f"{c.gpu_junction_c_loaded:.1f}",
        ]
        for c in cells
    ]
    lines.append(tabulate(rows, headers=headers, tablefmt="pipe"))
    lines.append("")

    path.write_text("\n".join(lines))
