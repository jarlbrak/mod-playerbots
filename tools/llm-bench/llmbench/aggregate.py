"""Pure aggregation helpers for per-cell metrics."""

import math
from collections.abc import Sequence


def percentile(values: Sequence[float], p: float) -> float:
    """Nearest-rank percentile. p in [0, 100]."""
    if not values:
        raise ValueError("percentile of empty sequence")
    if not 0 <= p <= 100:
        raise ValueError(f"p must be in [0, 100], got {p}")
    sorted_vals = sorted(values)
    n = len(sorted_vals)
    rank = max(1, math.ceil(p / 100.0 * n))
    return sorted_vals[rank - 1]


def decode_tokens_per_sec(eval_count: int, eval_duration_ns: int) -> float:
    """Tokens generated per second of decode."""
    if eval_duration_ns <= 0:
        return float("nan")
    seconds = eval_duration_ns / 1_000_000_000
    return eval_count / seconds


def prefill_tokens_per_sec(
    prompt_eval_count: int, prompt_eval_duration_ns: int
) -> float:
    """Tokens prefilled per second."""
    if prompt_eval_duration_ns <= 0:
        return float("nan")
    seconds = prompt_eval_duration_ns / 1_000_000_000
    return prompt_eval_count / seconds


def adherence_rate(grammar_valid_flags: Sequence[bool]) -> float:
    """Fraction of requests with grammar_valid=True."""
    if not grammar_valid_flags:
        raise ValueError("adherence of empty sequence")
    return sum(1 for v in grammar_valid_flags if v) / len(grammar_valid_flags)
