"""HTTP routes for /goals/*."""
import time
from typing import Any, Literal

from fastapi import APIRouter, HTTPException
from pydantic import BaseModel, Field

from memory_sidecar.helpers import generate_goal_id, generate_memory_id, embedding_to_blob


VALID_STATUSES = {"pending", "active", "completed", "abandoned"}
TERMINAL_STATUSES = {"completed", "abandoned"}
VALID_TRANSITIONS = {
    "pending":   {"active", "abandoned"},
    "active":    {"completed", "abandoned"},
    "completed": set(),
    "abandoned": set(),
}


class GoalCreateRequest(BaseModel):
    bot_id:        str
    text:          str = Field(..., min_length=1)
    source:        str | None = None
    priority:      int = 0
    origin_memory: str | None = None


class GoalCreateResponse(BaseModel):
    goal_id: str
    status:  str


class GoalRow(BaseModel):
    id:            str
    bot_id:        str
    text:          str
    status:        str
    source:        str | None
    priority:      int
    origin_memory: str | None
    created_ts:    int
    updated_ts:    int
    completed_ts:  int | None


class GoalUpdateRequest(BaseModel):
    bot_id:        str
    goal_id:       str
    text:          str | None = None
    status:        str | None = None
    priority:      int | None = None
    origin_memory: str | None = None


class GoalUpdateResponse(BaseModel):
    updated: bool


class GoalListResponse(BaseModel):
    items: list[GoalRow]
    total: int


class GoalCompleteRequest(BaseModel):
    bot_id:             str
    goal_id:            str
    outcome:            Literal["completed", "abandoned"]
    also_record_memory: bool = True


class GoalCompleteResponse(BaseModel):
    updated:   bool
    memory_id: str | None


def _row_to_goal(row) -> GoalRow:
    return GoalRow(
        id=row[0], bot_id=row[1], text=row[2], status=row[3], source=row[4],
        priority=row[5], origin_memory=row[6], created_ts=row[7],
        updated_ts=row[8], completed_ts=row[9],
    )


def _fetch_goal(conn, goal_id, bot_id):
    cur = conn.execute(
        "SELECT id, bot_id, text, status, source, priority, origin_memory, "
        "       created_ts, updated_ts, completed_ts "
        "FROM goals WHERE id=? AND bot_id=?",
        (goal_id, bot_id),
    )
    return cur.fetchone()


def build_router(state: dict[str, Any]) -> APIRouter:
    router = APIRouter()

    @router.post("/goals/create", response_model=GoalCreateResponse)
    async def create(req: GoalCreateRequest):
        conn = state["conn"]
        gid = generate_goal_id()
        now = int(time.time())
        conn.execute(
            "INSERT INTO goals (id, bot_id, text, status, source, priority, "
            "origin_memory, created_ts, updated_ts) "
            "VALUES (?, ?, ?, 'pending', ?, ?, ?, ?, ?)",
            (gid, req.bot_id, req.text, req.source, req.priority,
             req.origin_memory, now, now),
        )
        conn.commit()
        return GoalCreateResponse(goal_id=gid, status="pending")

    @router.get("/goals/list", response_model=GoalListResponse)
    async def list_goals(
        bot_id: str,
        status: str | None = None,
        limit: int = 50,
        offset: int = 0,
    ):
        conn = state["conn"]
        limit = max(1, min(500, limit))
        offset = max(0, offset)
        where = ["bot_id=?"]; params: list[Any] = [bot_id]
        if status is not None:
            statuses = [s.strip() for s in status.split(",") if s.strip()]
            if not all(s in VALID_STATUSES for s in statuses):
                raise HTTPException(status_code=400, detail="invalid status filter")
            placeholders = ",".join("?" * len(statuses))
            where.append(f"status IN ({placeholders})")
            params.extend(statuses)
        where_sql = " AND ".join(where)
        cur = conn.execute(f"SELECT COUNT(*) FROM goals WHERE {where_sql}", tuple(params))
        total = cur.fetchone()[0]
        cur = conn.execute(
            f"SELECT id, bot_id, text, status, source, priority, origin_memory, "
            f"       created_ts, updated_ts, completed_ts "
            f"FROM goals WHERE {where_sql} "
            f"ORDER BY priority DESC, created_ts DESC LIMIT ? OFFSET ?",
            tuple(params) + (limit, offset),
        )
        items = [_row_to_goal(r) for r in cur.fetchall()]
        return GoalListResponse(items=items, total=total)

    @router.get("/goals/{goal_id}", response_model=GoalRow)
    async def read(goal_id: str, bot_id: str):
        conn = state["conn"]
        row = _fetch_goal(conn, goal_id, bot_id)
        if not row:
            raise HTTPException(status_code=404, detail="goal not found")
        return _row_to_goal(row)

    @router.put("/goals/update", response_model=GoalUpdateResponse)
    async def update(req: GoalUpdateRequest):
        conn = state["conn"]
        row = _fetch_goal(conn, req.goal_id, req.bot_id)
        if not row:
            raise HTTPException(status_code=404, detail="goal not found")
        current_status = row[3]
        sets: list[str] = []
        params: list[Any] = []
        if req.text is not None:
            sets.append("text=?"); params.append(req.text)
        if req.priority is not None:
            sets.append("priority=?"); params.append(req.priority)
        if req.origin_memory is not None:
            sets.append("origin_memory=?"); params.append(req.origin_memory)
        if req.status is not None:
            if req.status not in VALID_STATUSES:
                raise HTTPException(status_code=400, detail=f"invalid status: {req.status}")
            if req.status != current_status:
                allowed = VALID_TRANSITIONS.get(current_status, set())
                if req.status not in allowed:
                    raise HTTPException(
                        status_code=400,
                        detail=f"invalid transition {current_status} → {req.status}",
                    )
            sets.append("status=?"); params.append(req.status)
            if req.status in TERMINAL_STATUSES:
                sets.append("completed_ts=?"); params.append(int(time.time()))
        if not sets:
            raise HTTPException(status_code=400, detail="no updatable field provided")
        sets.append("updated_ts=?"); params.append(int(time.time()))
        params.append(req.goal_id)
        conn.execute(f"UPDATE goals SET {', '.join(sets)} WHERE id=?", params)
        conn.commit()
        return GoalUpdateResponse(updated=True)

    @router.post("/goals/complete", response_model=GoalCompleteResponse)
    async def complete(req: GoalCompleteRequest):
        conn = state["conn"]
        row = _fetch_goal(conn, req.goal_id, req.bot_id)
        if not row:
            raise HTTPException(status_code=404, detail="goal not found")
        current_status = row[3]
        if current_status in TERMINAL_STATUSES:
            raise HTTPException(
                status_code=400,
                detail=f"goal is already terminal ({current_status})",
            )
        allowed = VALID_TRANSITIONS.get(current_status, set())
        if req.outcome not in allowed:
            raise HTTPException(
                status_code=400,
                detail=f"invalid transition {current_status} → {req.outcome}",
            )
        now = int(time.time())
        conn.execute(
            "UPDATE goals SET status=?, completed_ts=?, updated_ts=? WHERE id=?",
            (req.outcome, now, now, req.goal_id),
        )

        memory_id = None
        if req.also_record_memory:
            embedder = state["embedder"]
            text = f"Goal {req.outcome}: {row[2]}"  # row[2] = goal.text
            emb = await embedder.embed(text)
            memory_id = generate_memory_id()
            conn.execute(
                "INSERT OR IGNORE INTO bots (bot_id, created_ts) VALUES (?, ?)",
                (req.bot_id, now),
            )
            conn.execute(
                "INSERT INTO memories (id, bot_id, text, salience, created_ts, "
                "last_recalled_ts, embedding, memory_type, source) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, 'goal_link', ?)",
                (memory_id, req.bot_id, text, 0.6, now, now,
                 embedding_to_blob(emb), f"goals:{req.goal_id}"),
            )
            conn.execute(
                "INSERT INTO vec_memories (memory_id, bot_id, embedding) "
                "VALUES (?, ?, ?)",
                (memory_id, req.bot_id, embedding_to_blob(emb)),
            )

        conn.commit()
        return GoalCompleteResponse(updated=True, memory_id=memory_id)

    return router
