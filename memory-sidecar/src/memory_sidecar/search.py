"""Pure-function retrieval primitives: RRF fusion + MMR reranking."""
from typing import Sequence

import numpy as np


def rrf_fuse(
    ranked_lists: Sequence[Sequence[str]], k: int = 60
) -> dict[str, float]:
    """Reciprocal Rank Fusion (Cormack, Clarke, Buettcher 2009).

    For each doc d and each ranker r, contribute 1 / (k + rank_r(d))
    where rank_r(d) is the 0-indexed position in r's ranked list.
    Docs missing from a list contribute 0 for that list.

    Returns a {doc_id: fused_score} dict (higher is better).
    """
    scores: dict[str, float] = {}
    for lst in ranked_lists:
        for rank, doc in enumerate(lst):
            scores[doc] = scores.get(doc, 0.0) + 1.0 / (k + rank)
    return scores


def _cosine(a: np.ndarray, b: np.ndarray) -> float:
    na = float(np.linalg.norm(a))
    nb = float(np.linalg.norm(b))
    if na == 0.0 or nb == 0.0:
        return 0.0
    return float(np.dot(a, b) / (na * nb))


def mmr_select(
    candidates: Sequence[tuple[str, np.ndarray]],
    query_emb: np.ndarray,
    top_k: int,
    lam: float = 0.7,
) -> list[tuple[str, np.ndarray]]:
    """Maximal Marginal Relevance (Carbonell & Goldstein 1998).

    Each iteration picks the candidate maximizing:
        lam * sim(c, query) - (1 - lam) * max sim(c, s) for s in selected.

    lam near 1 favors relevance; lam near 0 favors diversity.
    """
    if not candidates or top_k <= 0:
        return []
    remaining = list(candidates)
    selected: list[tuple[str, np.ndarray]] = []

    # Pre-compute relevance scores once.
    relevance = {doc: _cosine(emb, query_emb) for doc, emb in remaining}

    while len(selected) < top_k and remaining:
        best_doc: tuple[str, np.ndarray] | None = None
        best_score = float("-inf")
        for cand in remaining:
            cid, cemb = cand
            rel = relevance[cid]
            if not selected:
                penalty = 0.0
            else:
                penalty = max(_cosine(cemb, s_emb) for _, s_emb in selected)
            score = lam * rel - (1.0 - lam) * penalty
            if score > best_score:
                best_score = score
                best_doc = cand
        assert best_doc is not None
        selected.append(best_doc)
        remaining.remove(best_doc)

    return selected
