"""FastAPI app for the memory sidecar."""
import base64
import os
import time
import uuid
from contextlib import asynccontextmanager
from typing import Any, Optional

import numpy as np
from fastapi import FastAPI, HTTPException
from pydantic import BaseModel, Field

from memory_sidecar.db import open_db, run_migrations
from memory_sidecar.embed import EmbeddingClient
from memory_sidecar.recall import Memory, ScoringWeights, score_memory


# ---- Request / response models ----

class RememberRel(BaseModel):
    src: str
    rel: str
    dst: str


class RememberRequest(BaseModel):
    bot_id: str
    text: str
    entities: list[str] = Field(default_factory=list)
    salience: float
    relations: list[RememberRel] = Field(default_factory=list)


class RememberResponse(BaseModel):
    memory_id: str
    evicted: int


class ForgetRequest(BaseModel):
    bot_id: str
    memory_id: str


class ForgetResponse(BaseModel):
    forgotten: bool


class RecallRequest(BaseModel):
    bot_id: str
    query: str
    top_k: int = 5


class RecalledMemory(BaseModel):
    memory_id: str
    text: str
    score: float
    ts: int


class RecallResponse(BaseModel):
    memories: list[RecalledMemory]


# ---- App factory ----

def create_app(embedder: Optional[Any] = None) -> FastAPI:
    db_path = os.environ.get("MEM_DB_PATH", "/var/memory/db.sqlite")
    embed_endpoint = os.environ.get("MEM_EMBED_ENDPOINT", "http://127.0.0.1:8081")
    cap_per_bot = int(os.environ.get("MEM_CAP_PER_BOT", "2000"))

    state: dict[str, Any] = {
        "db_path": db_path,
        "cap_per_bot": cap_per_bot,
        "embedder": embedder,
        "embed_endpoint": embed_endpoint,
        "weights": ScoringWeights(
            w_rel=float(os.environ.get("MEM_W_REL", "0.5")),
            w_rec=float(os.environ.get("MEM_W_REC", "0.2")),
            w_imp=float(os.environ.get("MEM_W_IMP", "0.3")),
            tau_seconds=int(os.environ.get("MEM_TAU_SECONDS", "604800")),
        ),
    }

    @asynccontextmanager
    async def lifespan(app: FastAPI):
        conn = open_db(state["db_path"])
        run_migrations(conn)
        state["conn"] = conn
        if state["embedder"] is None:
            state["embedder"] = EmbeddingClient(state["embed_endpoint"], dim=384)
        yield
        conn.close()
        if hasattr(state["embedder"], "aclose"):
            await state["embedder"].aclose()

    app = FastAPI(lifespan=lifespan, title="memory-sidecar")
    app.state.mem = state
    _register_routes(app, state)
    return app


def _generate_memory_id() -> str:
    """Short base32 prefix of uuid7 (16 random bytes → 11 base32 chars)."""
    raw = uuid.uuid4().bytes
    b32 = base64.b32encode(raw).decode("ascii").rstrip("=").lower()
    return f"m_{b32[:11]}"


def _upsert_entity(conn, bot_id: str, name: str) -> int:
    lower = name.lower()
    cur = conn.execute(
        "SELECT id FROM entities WHERE bot_id=? AND name_lower=?", (bot_id, lower)
    )
    row = cur.fetchone()
    if row:
        return row[0]
    cur = conn.execute(
        "INSERT INTO entities (bot_id, name_lower, display_name, type) "
        "VALUES (?, ?, ?, NULL)",
        (bot_id, lower, name),
    )
    return cur.lastrowid


def _embedding_to_blob(vec: np.ndarray) -> bytes:
    return vec.astype(np.float32).tobytes()


def _evict_if_over_cap(conn, bot_id: str, cap: int, now: int, w: ScoringWeights) -> int:
    cur = conn.execute(
        "SELECT id, salience, last_recalled_ts FROM memories WHERE bot_id=?",
        (bot_id,),
    )
    rows = cur.fetchall()
    if len(rows) <= cap:
        return 0

    import math
    def score(row):
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


def _register_routes(app: FastAPI, state: dict[str, Any]) -> None:

    @app.get("/health")
    async def health():
        return {"ok": True}

    @app.post("/memory/remember", response_model=RememberResponse)
    async def remember(req: RememberRequest):
        salience = max(0.0, min(1.0, req.salience))
        conn = state["conn"]
        embedder = state["embedder"]
        embedding = await embedder.embed(req.text)
        memory_id = _generate_memory_id()
        now = int(time.time())

        conn.execute(
            "INSERT OR IGNORE INTO bots (bot_id, created_ts) VALUES (?, ?)",
            (req.bot_id, now),
        )

        entity_ids: list[int] = [
            _upsert_entity(conn, req.bot_id, name) for name in req.entities
        ]

        conn.execute(
            "INSERT INTO memories (id, bot_id, text, salience, created_ts, "
            "last_recalled_ts, embedding) VALUES (?, ?, ?, ?, ?, ?, ?)",
            (memory_id, req.bot_id, req.text, salience, now, now,
             _embedding_to_blob(embedding)),
        )
        conn.execute(
            "INSERT INTO vec_memories (memory_id, bot_id, embedding) VALUES (?, ?, ?)",
            (memory_id, req.bot_id, _embedding_to_blob(embedding)),
        )
        for eid in entity_ids:
            conn.execute(
                "INSERT OR IGNORE INTO memory_entities (memory_id, entity_id) "
                "VALUES (?, ?)",
                (memory_id, eid),
            )

        for r in req.relations:
            src_id = _upsert_entity(conn, req.bot_id, r.src)
            dst_id = _upsert_entity(conn, req.bot_id, r.dst)
            conn.execute(
                "INSERT OR REPLACE INTO edges "
                "(src_entity_id, rel, dst_entity_id, weight, last_seen_ts) "
                "VALUES (?, ?, ?, 1.0, ?)",
                (src_id, r.rel, dst_id, now),
            )

        evicted = _evict_if_over_cap(
            conn, req.bot_id, state["cap_per_bot"], now, state["weights"]
        )
        conn.commit()
        return RememberResponse(memory_id=memory_id, evicted=evicted)

    @app.post("/memory/forget", response_model=ForgetResponse)
    async def forget(req: ForgetRequest):
        conn = state["conn"]
        cur = conn.execute(
            "DELETE FROM memories WHERE id=? AND bot_id=?", (req.memory_id, req.bot_id)
        )
        deleted = cur.rowcount > 0
        if deleted:
            conn.execute("DELETE FROM vec_memories WHERE memory_id=?", (req.memory_id,))
            conn.execute(
                "DELETE FROM memory_entities WHERE memory_id=?", (req.memory_id,)
            )
            conn.commit()
        return ForgetResponse(forgotten=deleted)

    @app.post("/memory/recall", response_model=RecallResponse)
    async def recall(req: RecallRequest):
        conn = state["conn"]
        embedder = state["embedder"]
        cur = conn.execute(
            "SELECT id, text, salience, created_ts, last_recalled_ts, embedding "
            "FROM memories WHERE bot_id=?",
            (req.bot_id,),
        )
        rows = cur.fetchall()
        if not rows:
            return RecallResponse(memories=[])

        q_emb = await embedder.embed(req.query)
        now = int(time.time())

        scored: list[tuple[float, str, str, int]] = []
        for row in rows:
            mid, text, salience, created_ts, last_recalled_ts, emb_blob = row
            emb = np.frombuffer(emb_blob, dtype=np.float32) if emb_blob else None
            m = Memory(
                id=mid, bot_id=req.bot_id, text=text, salience=salience,
                created_ts=created_ts, last_recalled_ts=last_recalled_ts,
                embedding=emb,
            )
            s = score_memory(m, q_emb, now, state["weights"])
            scored.append((s, mid, text, created_ts))

        scored.sort(reverse=True, key=lambda t: t[0])
        top = scored[: req.top_k]
        for s, mid, _, _ in top:
            conn.execute(
                "UPDATE memories SET last_recalled_ts=? WHERE id=?", (now, mid)
            )
        conn.commit()

        return RecallResponse(
            memories=[
                RecalledMemory(memory_id=mid, text=text, score=float(s), ts=ts)
                for s, mid, text, ts in top
            ]
        )
