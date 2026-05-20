"""Small helpers shared across route modules."""
import base64
import math
import re
import uuid
from typing import Any

import numpy as np

from memory_sidecar.recall import ScoringWeights


def generate_memory_id() -> str:
    """Short base32 prefix of uuid4 (16 random bytes → 11 base32 chars)."""
    raw = uuid.uuid4().bytes
    b32 = base64.b32encode(raw).decode("ascii").rstrip("=").lower()
    return f"m_{b32[:11]}"


def generate_goal_id() -> str:
    """Distinct prefix from memories for grep-friendliness."""
    raw = uuid.uuid4().bytes
    b32 = base64.b32encode(raw).decode("ascii").rstrip("=").lower()
    return f"g_{b32[:11]}"


def upsert_entity(conn, bot_id: str, name: str, type_hint: str | None = None) -> int:
    """Insert or fetch an entity row; optionally set type if NULL.

    type_hint is honored only when (a) the entity is new OR (b) the
    existing row has type IS NULL. Existing non-null types are not
    overwritten by callers passing different hints.
    """
    lower = name.lower()
    cur = conn.execute(
        "SELECT id, type FROM entities WHERE bot_id=? AND name_lower=?",
        (bot_id, lower),
    )
    row = cur.fetchone()
    if row:
        eid, existing_type = row
        if type_hint is not None and existing_type is None:
            conn.execute(
                "UPDATE entities SET type=? WHERE id=?", (type_hint, eid)
            )
        return eid
    cur = conn.execute(
        "INSERT INTO entities (bot_id, name_lower, display_name, type) "
        "VALUES (?, ?, ?, ?)",
        (bot_id, lower, name, type_hint),
    )
    return cur.lastrowid


def embedding_to_blob(vec: np.ndarray) -> bytes:
    return vec.astype(np.float32).tobytes()


def evict_if_over_cap(conn, bot_id: str, cap: int, now: int, w: ScoringWeights) -> int:
    """Evict lowest-scoring memories until rowcount <= cap.

    Scoring uses created_ts (NOT last_recalled_ts) — same shift the
    recall path will make in v0.2.
    """
    cur = conn.execute(
        "SELECT id, salience, created_ts FROM memories WHERE bot_id=?",
        (bot_id,),
    )
    rows = cur.fetchall()
    if len(rows) <= cap:
        return 0

    def score(row: Any) -> float:
        age = max(0, now - row[2])
        return row[1] * math.exp(-age / w.tau_seconds)

    rows_sorted = sorted(rows, key=score)
    n_evict = len(rows) - cap
    to_evict = [r[0] for r in rows_sorted[:n_evict]]
    for mid in to_evict:
        conn.execute("DELETE FROM memories WHERE id=?", (mid,))
        conn.execute("DELETE FROM vec_memories WHERE memory_id=?", (mid,))
        conn.execute("DELETE FROM memory_entities WHERE memory_id=?", (mid,))
    return n_evict


# ---- FTS5 query construction (v0.2.1) ----

_STOPWORDS = frozenset({
    'a', 'an', 'the', 'is', 'are', 'was', 'were', 'be', 'been', 'being',
    'i', 'my', 'me', 'we', 'our', 'you', 'your', 'he', 'she', 'they', 'them',
    'it', 'its', 'and', 'or', 'but', 'in', 'on', 'at', 'to', 'for', 'of', 'with',
    'this', 'that', 'by', 'from', 'as', 'not', 'no', 'so', 'if', 'do', 'did',
    'have', 'had', 'has', 'will', 'would', 'can', 'could', 'should', 'shall',
    'into', 'up', 'out', 'off', 'over', 'about', 'then', 'than', 'when', 'while',
    'after', 'before', 'there', 'here', 'what', 'which', 'who', 'how',
    'today', 'yesterday', 'now', 'just', 'very', 'also', 'too', 'more', 'some',
    'all', 'any', 'each', 'both', 'few', 'other', 'same', 'such', 'only',
})


def build_fts5_query(natural_query: str, max_terms: int = 6) -> str | None:
    """Convert a natural-language query into an FTS5 MATCH expression.

    Strategy:
      1. Lowercase + tokenize on word boundaries (alpha + apostrophe).
      2. Drop tokens that are stopwords OR shorter than 3 chars.
      3. Dedupe preserving first-occurrence order.
      4. Cap at max_terms (default 6).
      5. OR-join the survivors.

    Returns None if no significant terms remain (caller should skip BM25).

    Example:
        >>> build_fts5_query("what did Alice and I plan about the mount")
        "alice OR plan OR mount"
    """
    # Tokenize on alphabetic runs only. Apostrophes break FTS5 MATCH
    # syntax ("brun's" → unquoted ' is a parse error). The porter+
    # unicode61 tokenizer in our FTS5 also drops apostrophes when
    # indexing, so this matches what's in the index.
    tokens = re.findall(r"[a-zA-Z]+", natural_query.lower())
    sig: list[str] = []
    seen: set[str] = set()
    for t in tokens:
        if len(t) < 3 or t in _STOPWORDS or t in seen:
            continue
        seen.add(t)
        sig.append(t)
        if len(sig) >= max_terms:
            break
    if not sig:
        return None
    return ' OR '.join(sig)
