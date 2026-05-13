import math
import time

import numpy as np

from memory_sidecar.recall import Memory, ScoringWeights, score_memory, TAU_SECONDS_DEFAULT


def _vec(seed: int) -> np.ndarray:
    rng = np.random.default_rng(seed)
    return rng.random(384, dtype=np.float32)


def _normalize(v: np.ndarray) -> np.ndarray:
    n = np.linalg.norm(v)
    return v / n if n > 0 else v


def test_score_higher_for_relevant_memory():
    q = _normalize(_vec(1))
    relevant = Memory(
        id="m_rel", bot_id="b1", text="x", salience=0.5,
        created_ts=int(time.time()), last_recalled_ts=int(time.time()),
        embedding=q.copy(),
    )
    unrelated = Memory(
        id="m_un", bot_id="b1", text="x", salience=0.5,
        created_ts=int(time.time()), last_recalled_ts=int(time.time()),
        embedding=_normalize(_vec(99)),
    )
    w = ScoringWeights()
    now = int(time.time())
    assert score_memory(relevant, q, now, w) > score_memory(unrelated, q, now, w)


def test_score_higher_for_recent_memory():
    q = _normalize(_vec(2))
    e = _normalize(_vec(2))
    now = int(time.time())
    recent = Memory(
        id="m_recent", bot_id="b1", text="x", salience=0.5,
        created_ts=now - 60, last_recalled_ts=now - 60, embedding=e,
    )
    stale = Memory(
        id="m_stale", bot_id="b1", text="x", salience=0.5,
        created_ts=now - 30 * 86400, last_recalled_ts=now - 30 * 86400, embedding=e,
    )
    w = ScoringWeights()
    assert score_memory(recent, q, now, w) > score_memory(stale, q, now, w)


def test_score_higher_for_more_salient_memory():
    q = _normalize(_vec(3))
    e = _normalize(_vec(3))
    now = int(time.time())
    high = Memory(
        id="m_hi", bot_id="b1", text="x", salience=0.9,
        created_ts=now, last_recalled_ts=now, embedding=e,
    )
    low = Memory(
        id="m_lo", bot_id="b1", text="x", salience=0.1,
        created_ts=now, last_recalled_ts=now, embedding=e,
    )
    w = ScoringWeights()
    assert score_memory(high, q, now, w) > score_memory(low, q, now, w)


def test_recency_decay_uses_tau():
    q = _normalize(_vec(4))
    e = _normalize(_vec(4))
    now = int(time.time())
    one_tau_old = Memory(
        id="m1", bot_id="b1", text="x", salience=0.0,
        created_ts=now - TAU_SECONDS_DEFAULT,
        last_recalled_ts=now - TAU_SECONDS_DEFAULT,
        embedding=e,
    )
    w = ScoringWeights(w_rel=0.0, w_rec=1.0, w_imp=0.0)
    s = score_memory(one_tau_old, q, now, w)
    assert abs(s - math.exp(-1)) < 0.01


def test_weights_sum_to_one_in_default():
    w = ScoringWeights()
    assert abs((w.w_rel + w.w_rec + w.w_imp) - 1.0) < 1e-9


def test_default_weights_match_spec():
    w = ScoringWeights()
    assert w.w_rel == 0.5
    assert w.w_rec == 0.2
    assert w.w_imp == 0.3


def test_score_is_zero_when_all_weights_zero():
    q = _normalize(_vec(5))
    e = _normalize(_vec(5))
    now = int(time.time())
    m = Memory(
        id="m", bot_id="b1", text="x", salience=1.0,
        created_ts=now, last_recalled_ts=now, embedding=e,
    )
    w = ScoringWeights(w_rel=0.0, w_rec=0.0, w_imp=0.0)
    assert score_memory(m, q, now, w) == 0.0


def test_cosine_handles_zero_vectors():
    q = _normalize(_vec(6))
    zero = np.zeros(384, dtype=np.float32)
    now = int(time.time())
    m = Memory(
        id="m", bot_id="b1", text="x", salience=0.5,
        created_ts=now, last_recalled_ts=now, embedding=zero,
    )
    w = ScoringWeights()
    s = score_memory(m, q, now, w)
    assert not math.isnan(s)
