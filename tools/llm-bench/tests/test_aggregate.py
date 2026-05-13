import math
from llmbench.aggregate import (
    percentile,
    decode_tokens_per_sec,
    prefill_tokens_per_sec,
    adherence_rate,
)


def test_percentile_basic():
    assert percentile([1, 2, 3, 4, 5], 50) == 3
    assert percentile([1, 2, 3, 4, 5], 100) == 5


def test_percentile_p95_on_20_items():
    items = list(range(1, 21))  # 1..20
    # nearest-rank: ceil(0.95 * 20) = 19th item = 19
    assert percentile(items, 95) == 19


def test_percentile_empty_raises():
    import pytest

    with pytest.raises(ValueError):
        percentile([], 50)


def test_decode_tokens_per_sec_basic():
    # 200 decode tokens in 4 seconds = 50 tok/s
    assert decode_tokens_per_sec(eval_count=200, eval_duration_ns=4_000_000_000) == 50.0


def test_decode_tokens_per_sec_zero_duration():
    # avoid div by zero
    assert math.isnan(decode_tokens_per_sec(eval_count=200, eval_duration_ns=0))


def test_prefill_tokens_per_sec_basic():
    # 1500 prefill tokens in 0.5 seconds = 3000 tok/s
    assert prefill_tokens_per_sec(
        prompt_eval_count=1500, prompt_eval_duration_ns=500_000_000
    ) == 3000.0


def test_adherence_rate_all_true():
    assert adherence_rate([True, True, True]) == 1.0


def test_adherence_rate_mixed():
    assert adherence_rate([True, False, True, False]) == 0.5


def test_adherence_rate_empty():
    import pytest

    with pytest.raises(ValueError):
        adherence_rate([])
