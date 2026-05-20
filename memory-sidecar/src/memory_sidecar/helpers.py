"""Small helpers shared across route modules."""
import base64
import math
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
