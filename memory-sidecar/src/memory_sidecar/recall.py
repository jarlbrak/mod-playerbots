"""Pure functions for the recall scoring formula (parent design §8.3)."""
import math
from dataclasses import dataclass
from typing import Optional

import numpy as np


TAU_SECONDS_DEFAULT = 604800  # 7 days


@dataclass
class Memory:
    id: str
    bot_id: str
    text: str
    salience: float
    created_ts: int
    last_recalled_ts: int
    embedding: Optional[np.ndarray]


@dataclass
class ScoringWeights:
    w_rel: float = 0.5
    w_rec: float = 0.2
    w_imp: float = 0.3
    tau_seconds: int = TAU_SECONDS_DEFAULT


def cosine(a: np.ndarray, b: np.ndarray) -> float:
    na = float(np.linalg.norm(a))
    nb = float(np.linalg.norm(b))
    if na == 0.0 or nb == 0.0:
        return 0.0
    return float(np.dot(a, b) / (na * nb))


def score_memory(
    m: Memory, query_emb: np.ndarray, now: int, w: ScoringWeights
) -> float:
    """Weighted sum of relevance + recency + importance."""
    if m.embedding is None:
        relevance = 0.0
    else:
        relevance = cosine(m.embedding, query_emb)
    age = max(0, now - m.last_recalled_ts)
    recency = math.exp(-age / w.tau_seconds)
    importance = max(0.0, min(1.0, m.salience))
    return w.w_rel * relevance + w.w_rec * recency + w.w_imp * importance
