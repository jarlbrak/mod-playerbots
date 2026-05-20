"""Pure-function unit tests for RRF + MMR."""
import numpy as np
import pytest

from memory_sidecar.search import rrf_fuse, mmr_select


def test_rrf_fuse_combines_ranked_lists():
    # Three rankers, each producing a ranked list of doc IDs.
    a = ["d1", "d2", "d3"]
    b = ["d2", "d3", "d1"]
    c = ["d3", "d1", "d2"]
    scores = rrf_fuse([a, b, c], k=60)
    # d1, d2, d3 each appear in all three lists at different positions.
    # All should be present.
    assert set(scores.keys()) == {"d1", "d2", "d3"}
    # Doc d2 appears at positions 1, 0, 2 → sum of 1/(k+1)+1/(k+0)+1/(k+2)
    expected_d2 = 1/(60+1) + 1/(60+0) + 1/(60+2)
    assert abs(scores["d2"] - expected_d2) < 1e-9


def test_rrf_fuse_handles_missing_docs():
    a = ["d1", "d2"]
    b = ["d3"]
    scores = rrf_fuse([a, b], k=60)
    assert "d1" in scores
    assert "d3" in scores
    # d1 only in a at rank 0; score = 1/60.
    assert abs(scores["d1"] - 1/60) < 1e-9


def test_mmr_select_returns_top_k():
    # 4 candidates, 2 of which are near-duplicates.
    rng = np.random.default_rng(42)
    q = rng.random(8, dtype=np.float32)
    # Two near-duplicates + two unique.
    e1 = rng.random(8, dtype=np.float32)
    e2 = e1 + 0.001 * rng.random(8, dtype=np.float32)  # near-duplicate of e1
    e3 = rng.random(8, dtype=np.float32)
    e4 = rng.random(8, dtype=np.float32)
    candidates = [
        ("d1", e1), ("d2", e2), ("d3", e3), ("d4", e4),
    ]
    selected = mmr_select(candidates, q, top_k=3, lam=0.5)
    ids = [s[0] for s in selected]
    assert len(ids) == 3
    # If lam=0.5, the second pick should NOT be d2 (near-dup of d1).
    if ids[0] == "d1":
        assert ids[1] != "d2"


def test_mmr_select_pure_relevance_when_lam_is_1():
    rng = np.random.default_rng(0)
    q = np.array([1, 0, 0, 0, 0, 0, 0, 0], dtype=np.float32)
    e1 = np.array([1, 0, 0, 0, 0, 0, 0, 0], dtype=np.float32)  # perfect match
    e2 = np.array([0.9, 0.1, 0, 0, 0, 0, 0, 0], dtype=np.float32)
    e3 = np.array([0, 1, 0, 0, 0, 0, 0, 0], dtype=np.float32)
    candidates = [("d1", e1), ("d2", e2), ("d3", e3)]
    selected = mmr_select(candidates, q, top_k=3, lam=1.0)
    # lam=1.0 → pure relevance ordering: d1, d2, d3
    assert [s[0] for s in selected] == ["d1", "d2", "d3"]
