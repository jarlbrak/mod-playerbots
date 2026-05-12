import csv
import json
from pathlib import Path

from llmbench.output import (
    ResultRow,
    CellSummary,
    write_results_csv,
    write_summary_md,
)


def make_row(**overrides):
    base = ResultRow(
        timestamp="2026-05-12T10:00:00Z",
        model="gemma-2-9b",
        grammar="json_schema",
        slots=4,
        fixture="01-orc-warrior-lv10",
        phase="steady",
        request_body_bytes=1200,
        response_status=200,
        prompt_eval_count=1500,
        eval_count=120,
        prompt_eval_duration_ns=400_000_000,
        eval_duration_ns=3_000_000_000,
        wall_clock_ms=3400,
        grammar_valid=True,
        parse_error=None,
        raw_response='{"goal":"rest","params":{},"ttl_minutes":15,"reasoning":"x"}',
    )
    return base._replace(**overrides) if hasattr(base, "_replace") else base.__class__(**{**base.__dict__, **overrides})


def test_write_results_csv_writes_all_columns(tmp_path):
    rows = [make_row(), make_row(fixture="02-undead-mage-lv37", grammar_valid=False, parse_error="bad enum")]
    out = tmp_path / "results.csv"
    write_results_csv(out, rows)

    with open(out) as f:
        reader = csv.DictReader(f)
        records = list(reader)

    assert len(records) == 2
    assert records[0]["model"] == "gemma-2-9b"
    assert records[0]["grammar_valid"] == "True"
    assert records[1]["grammar_valid"] == "False"
    assert records[1]["parse_error"] == "bad enum"


def test_write_summary_md_renders_cells(tmp_path):
    cells = [
        CellSummary(
            model="gemma-2-9b",
            grammar="json_schema",
            slots=4,
            phase="steady",
            n_requests=120,
            p50_wall_ms=1800.0,
            p95_wall_ms=2400.0,
            p99_wall_ms=3000.0,
            decode_toks_per_sec=42.5,
            prefill_toks_per_sec=2950.0,
            adherence=0.975,
            vram_idle_mb=6800,
            vram_loaded_mb=9900,
        )
    ]
    out = tmp_path / "summary.md"
    write_summary_md(out, cells, run_metadata={"date": "2026-05-12", "backend": "vulkan"})

    text = out.read_text()
    assert "gemma-2-9b" in text
    assert "json_schema" in text
    assert "97.5%" in text or "0.975" in text
    assert "9900" in text  # VRAM loaded
    assert "2026-05-12" in text
    assert "vulkan" in text
