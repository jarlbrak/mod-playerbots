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


class RecallAboutRequest(BaseModel):
    bot_id: str
    entity: str
    max_hops: int = 2
    top_k: int = 3


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
        cur = conn.execute(
            f"SELECT DISTINCT m.id, m.text, m.salience, m.created_ts, "
            f"       m.last_recalled_ts, m.embedding "
            f"FROM memories m "
            f"JOIN memory_entities me ON me.memory_id = m.id "
            f"WHERE m.bot_id = ? AND me.entity_id IN ({placeholders})",
            (req.bot_id,) + tuple(visited),
        )
        rows = cur.fetchall()
        if not rows:
            return RecallAboutResponse(hints=[])

        q_emb = await embedder.embed(f"recent events near {req.entity}")
        now = int(time.time())
        scored = []
        for r in rows:
            mid, text, sal, ct, lr, emb_blob = r
            emb = np.frombuffer(emb_blob, dtype=np.float32) if emb_blob else None
            m = Memory(
                id=mid, bot_id=req.bot_id, text=text, salience=sal,
                created_ts=ct, last_recalled_ts=lr, embedding=emb,
            )
            scored.append((score_memory(m, q_emb, now, state["weights"]), mid, text))
        scored.sort(reverse=True, key=lambda t: t[0])
        top = scored[: req.top_k]
        for _, mid, _ in top:
            conn.execute(
                "UPDATE memories SET last_recalled_ts=? WHERE id=?", (now, mid)
            )
        conn.commit()
        return RecallAboutResponse(hints=[text for _, _, text in top])

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
