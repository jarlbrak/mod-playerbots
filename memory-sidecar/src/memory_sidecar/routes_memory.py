"""HTTP routes for /memory/* (everything except personality + goals)."""
import time
from typing import Any

import numpy as np
from fastapi import APIRouter, HTTPException
from pydantic import BaseModel, Field

from memory_sidecar.helpers import (
    embedding_to_blob,
    evict_if_over_cap,
    generate_memory_id,
    upsert_entity,
)
from memory_sidecar.recall import Memory, score_memory
from memory_sidecar.search import mmr_select


# ---- Request / response models (moved verbatim from main.py) ----

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
    bot_id:      str
    query:       str
    top_k:       int = 5
    since_ts:    int | None = None
    until_ts:    int | None = None
    memory_type: str | None = None


class RecalledMemory(BaseModel):
    memory_id: str
    text: str
    score: float
    ts: int


class RecallResponse(BaseModel):
    memories: list[RecalledMemory]


class RecallAboutRequest(BaseModel):
    bot_id:      str
    entity:      str
    max_hops:    int = 2
    top_k:       int = 3
    since_ts:    int | None = None
    until_ts:    int | None = None
    memory_type: str | None = None


class RecallAboutResponse(BaseModel):
    hints: list[str]


class UpdateRequest(BaseModel):
    bot_id:      str
    memory_id:   str
    text:        str | None = None
    salience:    float | None = None
    memory_type: str | None = None
    source:      str | None = None


class UpdateResponse(BaseModel):
    updated:     bool
    re_embedded: bool


class MemoryRow(BaseModel):
    id:               str
    bot_id:           str
    text:             str
    salience:         float
    memory_type:      str
    source:           str | None
    created_ts:       int
    last_recalled_ts: int


class ListResponse(BaseModel):
    items: list[MemoryRow]
    total: int


def build_router(state: dict[str, Any]) -> APIRouter:
    router = APIRouter()

    @router.post("/memory/remember", response_model=RememberResponse)
    async def remember(req: RememberRequest):
        salience = max(0.0, min(1.0, req.salience))
        conn = state["conn"]
        embedder = state["embedder"]
        embedding = await embedder.embed(req.text)
        memory_id = generate_memory_id()
        now = int(time.time())

        conn.execute(
            "INSERT OR IGNORE INTO bots (bot_id, created_ts) VALUES (?, ?)",
            (req.bot_id, now),
        )

        entity_ids: list[int] = [
            upsert_entity(conn, req.bot_id, name) for name in req.entities
        ]

        conn.execute(
            "INSERT INTO memories (id, bot_id, text, salience, created_ts, "
            "last_recalled_ts, embedding) VALUES (?, ?, ?, ?, ?, ?, ?)",
            (memory_id, req.bot_id, req.text, salience, now, now,
             embedding_to_blob(embedding)),
        )
        conn.execute(
            "INSERT INTO vec_memories (memory_id, bot_id, embedding) VALUES (?, ?, ?)",
            (memory_id, req.bot_id, embedding_to_blob(embedding)),
        )
        for eid in entity_ids:
            conn.execute(
                "INSERT OR IGNORE INTO memory_entities (memory_id, entity_id) "
                "VALUES (?, ?)",
                (memory_id, eid),
            )

        for r in req.relations:
            src_id = upsert_entity(conn, req.bot_id, r.src)
            dst_id = upsert_entity(conn, req.bot_id, r.dst)
            conn.execute(
                "INSERT OR REPLACE INTO edges "
                "(src_entity_id, rel, dst_entity_id, weight, last_seen_ts) "
                "VALUES (?, ?, ?, 1.0, ?)",
                (src_id, r.rel, dst_id, now),
            )

        evicted = evict_if_over_cap(
            conn, req.bot_id, state["cap_per_bot"], now, state["weights"]
        )
        conn.commit()
        return RememberResponse(memory_id=memory_id, evicted=evicted)

    @router.post("/memory/forget", response_model=ForgetResponse)
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

    @router.post("/memory/recall", response_model=RecallResponse)
    async def recall(req: RecallRequest):
        conn = state["conn"]
        embedder = state["embedder"]
        q_emb = await embedder.embed(req.query)

        # KNN against vec_memories (over-fetch for MMR + filters)
        over_fetch = max(req.top_k * 4, 20)
        extra_where = _extra_where_clause(req)
        extra_params = _extra_params(req)
        cur = conn.execute(
            "SELECT v.memory_id, m.text, m.salience, m.created_ts, "
            "       m.last_recalled_ts, m.embedding "
            "FROM vec_memories v "
            "JOIN memories m ON m.id = v.memory_id "
            f"WHERE v.bot_id = ? {extra_where} "
            "ORDER BY vec_distance_cosine(v.embedding, ?) ASC "
            "LIMIT ?",
            (req.bot_id,) + extra_params + (embedding_to_blob(q_emb), over_fetch),
        )
        rows = cur.fetchall()
        if not rows:
            return RecallResponse(memories=[])

        now = int(time.time())
        candidates: list[tuple[str, np.ndarray]] = []
        meta: dict[str, tuple[str, float, int]] = {}  # memory_id → (text, score, created_ts)
        for row in rows:
            mid, text, sal, ct, lr, emb_blob = row
            emb = np.frombuffer(emb_blob, dtype=np.float32) if emb_blob else None
            m = Memory(
                id=mid, bot_id=req.bot_id, text=text, salience=sal,
                created_ts=ct, last_recalled_ts=lr, embedding=emb,
            )
            s = score_memory(m, q_emb, now, state["weights"],
                             recency_basis=state["recency_basis"])
            if emb is not None:
                candidates.append((mid, emb))
            meta[mid] = (text, s, ct)

        # MMR diversity reranking
        selected = mmr_select(candidates, q_emb, top_k=req.top_k,
                              lam=state["mmr_lambda"])

        for mid, _ in selected:
            conn.execute(
                "UPDATE memories SET last_recalled_ts=? WHERE id=?", (now, mid)
            )
        conn.commit()

        return RecallResponse(
            memories=[
                RecalledMemory(memory_id=mid, text=meta[mid][0],
                               score=float(meta[mid][1]), ts=meta[mid][2])
                for mid, _ in selected
            ]
        )

    @router.post("/memory/recall_about", response_model=RecallAboutResponse)
    async def recall_about(req: RecallAboutRequest):
        conn = state["conn"]
        embedder = state["embedder"]
        entity_lower = req.entity.lower()

        cur = conn.execute(
            "SELECT id FROM entities WHERE bot_id=? AND name_lower=?",
            (req.bot_id, entity_lower),
        )
        row = cur.fetchone()
        if not row:
            return RecallAboutResponse(hints=[])
        seed_id = row[0]

        visited = {seed_id}
        frontier = {seed_id}
        for _ in range(max(0, req.max_hops)):
            if not frontier:
                break
            placeholders = ",".join("?" * len(frontier))
            cur = conn.execute(
                f"SELECT src_entity_id, dst_entity_id FROM edges "
                f"WHERE src_entity_id IN ({placeholders}) "
                f"   OR dst_entity_id IN ({placeholders})",
                tuple(frontier) + tuple(frontier),
            )
            next_frontier: set[int] = set()
            for s, d in cur.fetchall():
                for n in (s, d):
                    if n not in visited:
                        next_frontier.add(n)
                        visited.add(n)
            frontier = next_frontier

        placeholders = ",".join("?" * len(visited))
        extra_where = _extra_where_clause(req)
        extra_params = _extra_params(req)
        cur = conn.execute(
            f"SELECT DISTINCT m.id, m.text, m.salience, m.created_ts, "
            f"       m.last_recalled_ts, m.embedding "
            f"FROM memories m "
            f"JOIN memory_entities me ON me.memory_id = m.id "
            f"WHERE m.bot_id = ? AND me.entity_id IN ({placeholders}) {extra_where}",
            (req.bot_id,) + tuple(visited) + extra_params,
        )
        rows = cur.fetchall()
        if not rows:
            return RecallAboutResponse(hints=[])

        q_emb = await embedder.embed(f"recent events near {req.entity}")
        now = int(time.time())
        candidates: list[tuple[str, np.ndarray]] = []
        meta: dict[str, tuple[str, float, int]] = {}
        for r in rows:
            mid, text, sal, ct, lr, emb_blob = r
            emb = np.frombuffer(emb_blob, dtype=np.float32) if emb_blob else None
            m = Memory(
                id=mid, bot_id=req.bot_id, text=text, salience=sal,
                created_ts=ct, last_recalled_ts=lr, embedding=emb,
            )
            s = score_memory(m, q_emb, now, state["weights"],
                             recency_basis=state["recency_basis"])
            if emb is not None:
                candidates.append((mid, emb))
            meta[mid] = (text, s, ct)

        # MMR diversity reranking
        selected = mmr_select(candidates, q_emb, top_k=req.top_k,
                              lam=state["mmr_lambda"])

        for mid, _ in selected:
            conn.execute(
                "UPDATE memories SET last_recalled_ts=? WHERE id=?", (now, mid)
            )
        conn.commit()
        return RecallAboutResponse(hints=[meta[mid][0] for mid, _ in selected])

    def _extra_where_clause(req) -> str:
        """Build optional WHERE clause fragments for since/until/memory_type."""
        parts: list[str] = []
        if getattr(req, "since_ts", None) is not None:
            parts.append("AND m.created_ts >= ?")
        if getattr(req, "until_ts", None) is not None:
            parts.append("AND m.created_ts <= ?")
        if getattr(req, "memory_type", None) is not None:
            parts.append("AND m.memory_type = ?")
        return " ".join(parts)

    def _extra_params(req) -> tuple[Any, ...]:
        out: list[Any] = []
        if getattr(req, "since_ts", None) is not None:
            out.append(req.since_ts)
        if getattr(req, "until_ts", None) is not None:
            out.append(req.until_ts)
        if getattr(req, "memory_type", None) is not None:
            out.append(req.memory_type)
        return tuple(out)

    @router.get("/memory/list", response_model=ListResponse)
    async def list_memories(
        bot_id: str,
        memory_type: str | None = None,
        source: str | None = None,
        since_ts: int | None = None,
        until_ts: int | None = None,
        limit: int = 50,
        offset: int = 0,
    ):
        limit = max(1, min(500, limit))
        offset = max(0, offset)
        conn = state["conn"]
        where = ["bot_id=?"]
        params: list[Any] = [bot_id]
        if memory_type is not None:
            where.append("memory_type=?"); params.append(memory_type)
        if source is not None:
            where.append("source=?"); params.append(source)
        if since_ts is not None:
            where.append("created_ts>=?"); params.append(since_ts)
        if until_ts is not None:
            where.append("created_ts<=?"); params.append(until_ts)
        where_sql = " AND ".join(where)

        cur = conn.execute(
            f"SELECT COUNT(*) FROM memories WHERE {where_sql}", tuple(params)
        )
        total = cur.fetchone()[0]

        cur = conn.execute(
            f"SELECT id, bot_id, text, salience, memory_type, source, "
            f"       created_ts, last_recalled_ts "
            f"FROM memories WHERE {where_sql} "
            f"ORDER BY created_ts DESC LIMIT ? OFFSET ?",
            tuple(params) + (limit, offset),
        )
        items = [
            MemoryRow(
                id=r[0], bot_id=r[1], text=r[2], salience=r[3],
                memory_type=r[4], source=r[5], created_ts=r[6],
                last_recalled_ts=r[7],
            )
            for r in cur.fetchall()
        ]
        return ListResponse(items=items, total=total)

    @router.get("/memory/{memory_id}", response_model=MemoryRow)
    async def get_memory(memory_id: str, bot_id: str):
        conn = state["conn"]
        cur = conn.execute(
            "SELECT id, bot_id, text, salience, memory_type, source, "
            "       created_ts, last_recalled_ts "
            "FROM memories WHERE id=? AND bot_id=?",
            (memory_id, bot_id),
        )
        row = cur.fetchone()
        if not row:
            raise HTTPException(status_code=404, detail="memory not found")
        return MemoryRow(
            id=row[0], bot_id=row[1], text=row[2], salience=row[3],
            memory_type=row[4], source=row[5], created_ts=row[6],
            last_recalled_ts=row[7],
        )

    @router.put("/memory/update", response_model=UpdateResponse)
    async def update(req: UpdateRequest):
        if req.text is None and req.salience is None \
                and req.memory_type is None and req.source is None:
            raise HTTPException(status_code=400, detail="no updatable field provided")

        conn = state["conn"]
        cur = conn.execute(
            "SELECT id FROM memories WHERE id=? AND bot_id=?",
            (req.memory_id, req.bot_id),
        )
        if cur.fetchone() is None:
            raise HTTPException(status_code=404, detail="memory not found for this bot")

        sets: list[str] = []
        params: list[Any] = []
        re_embedded = False

        if req.text is not None:
            embedder = state["embedder"]
            new_emb = await embedder.embed(req.text)
            sets.append("text=?")
            params.append(req.text)
            re_embedded = True
            # vec_memories is a virtual table; trigger pattern doesn't fire on
            # it (no AFTER UPDATE trigger for vec0). Manage manually.
            conn.execute("DELETE FROM vec_memories WHERE memory_id=?", (req.memory_id,))
            conn.execute(
                "INSERT INTO vec_memories (memory_id, bot_id, embedding) VALUES (?, ?, ?)",
                (req.memory_id, req.bot_id, embedding_to_blob(new_emb)),
            )
            sets.append("embedding=?")
            params.append(embedding_to_blob(new_emb))

        if req.salience is not None:
            sets.append("salience=?")
            params.append(max(0.0, min(1.0, req.salience)))

        if req.memory_type is not None:
            sets.append("memory_type=?")
            params.append(req.memory_type)

        if req.source is not None:
            sets.append("source=?")
            params.append(req.source)

        now = int(time.time())
        sets.append("last_recalled_ts=?")
        params.append(now)

        params.append(req.memory_id)
        conn.execute(
            f"UPDATE memories SET {', '.join(sets)} WHERE id=?",
            params,
        )
        conn.commit()

        return UpdateResponse(updated=True, re_embedded=re_embedded)

    return router
