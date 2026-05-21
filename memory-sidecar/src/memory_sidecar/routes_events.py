"""SSE endpoint for streaming new memory rows to subscribers.

Endpoint: GET /v1/events/stream
Query params:
  bot_id   : str (required, len <= 32)
  prefixes : str (required, comma-separated list, max 16 items, each <= 64 chars)
Headers:
  Authorization: Bearer <MEM_SSE_BEARER>  (checked only when MEM_SSE_BEARER env is set)
  Last-Event-ID: <int>                    (optional, present on reconnect)
  Accept: text/event-stream

Wire protocol:
  Row dict shape published by routes_memory.remember:
    {"rowid": int, "memory_id": str, "bot_id": str, "text": str,
     "created_ts": int, "salience": float}

  Replay query uses SQLite rowid (integer) as the cursor, not the string id.
  The idx_memories_bot index keeps per-bot replay fast.

Subscribe-then-replay ordering (critical):
  1. Register subscriber (queue starts buffering new writes).
  2. Replay missed rows from DB using rowid > last_event_id.
  3. Drain queue, de-duping against the replay high-water mark.
  This closes the race window where a row could be written between
  replay done and subscriber registered.
"""
from __future__ import annotations

import asyncio
import json
import logging
import os
import time
from typing import Optional

from fastapi import APIRouter, Header, HTTPException, Query, Request
from fastapi.responses import StreamingResponse

from memory_sidecar.sse_format import (
    format_sse_error,
    format_sse_heartbeat,
    format_sse_memory,
)

logger = logging.getLogger(__name__)
router = APIRouter()

MAX_PREFIXES = 16
MAX_PREFIX_LEN = 64
MAX_BOT_ID_LEN = 32
REPLAY_CHUNK_SIZE = 500
HEARTBEAT_INTERVAL_S = 15.0


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _parse_prefixes(raw: str) -> list[str]:
    parts = [p.strip().lower() for p in raw.split(",") if p.strip()]
    if not parts:
        raise HTTPException(status_code=400, detail="prefixes must be non-empty")
    if len(parts) > MAX_PREFIXES:
        raise HTTPException(status_code=400, detail=f"prefixes count exceeds {MAX_PREFIXES}")
    for p in parts:
        if len(p) > MAX_PREFIX_LEN:
            raise HTTPException(
                status_code=400, detail=f"prefix len exceeds {MAX_PREFIX_LEN}: {p!r}"
            )
    return parts


def _matches_any_prefix(text: Optional[str], prefixes: list[str]) -> bool:
    if not text:
        return False
    low = text.lower()
    return any(low.startswith(p) for p in prefixes)


def _check_bearer(request: Request, authorization: Optional[str]) -> None:
    """Check bearer token when MEM_SSE_BEARER is configured.

    When MEM_SSE_BEARER is not set (e.g. in tests without a token store),
    auth is disabled and any request passes. This mirrors the behavior of
    the HTTP routes which are open.
    """
    expected = os.environ.get("MEM_SSE_BEARER", "")
    if not expected:
        return  # auth disabled — no bearer configured
    if not authorization or not authorization.startswith("Bearer "):
        raise HTTPException(status_code=401, detail="missing bearer token")
    token = authorization.removeprefix("Bearer ").strip()
    if token != expected:
        raise HTTPException(status_code=401, detail="invalid bearer token")


def _fetch_rows_since(conn, bot_id: str, since_rowid: int, limit: int) -> list[dict]:
    """Fetch memory rows with rowid > since_rowid for this bot.

    Uses the idx_memories_bot index via bot_id filter, then applies rowid
    ordering. Returns dicts with keys matching the pubsub row dict shape.
    """
    rows = conn.execute(
        "SELECT rowid, id, bot_id, text, created_ts, salience "
        "FROM memories WHERE bot_id = ? AND rowid > ? ORDER BY rowid ASC LIMIT ?",
        (bot_id, since_rowid, limit),
    ).fetchall()
    out = []
    for r in rows:
        out.append({
            "rowid": r[0],
            "memory_id": r[1],
            "bot_id": r[2],
            "text": r[3],
            "created_ts": r[4],
            "salience": r[5] if r[5] is not None else 0.5,
        })
    return out


# ---------------------------------------------------------------------------
# SSE endpoint
# ---------------------------------------------------------------------------

@router.get("/v1/events/stream")
async def stream_events(
    request: Request,
    bot_id: Optional[str] = Query(None),
    prefixes: Optional[str] = Query(None),
    last_event_id: Optional[str] = Header(None, alias="Last-Event-ID"),
    authorization: Optional[str] = Header(None),
):
    # --- Auth (checked before validation so auth errors take precedence) ---
    _check_bearer(request, authorization)

    # --- Validation (returns 400, not 422, for missing/bad params) ---
    if not bot_id:
        raise HTTPException(status_code=400, detail="bot_id is required")
    if len(bot_id) > MAX_BOT_ID_LEN:
        raise HTTPException(status_code=400, detail="bot_id too long (max 32 chars)")
    if not prefixes:
        raise HTTPException(status_code=400, detail="prefixes is required")
    prefix_list = _parse_prefixes(prefixes)
    try:
        replay_cursor = int(last_event_id) if last_event_id else 0
    except (TypeError, ValueError):
        replay_cursor = 0

    # --- Subscribe FIRST (subscribe-then-replay ordering) ---
    pubsub = request.app.state.pubsub
    db_conn = request.app.state.mem["conn"]
    queue = pubsub.subscribe(bot_id)

    async def event_generator():
        replayed_max_id = replay_cursor
        try:
            # Phase 1: Replay missed rows in chunks until caught up.
            # Subscriber is already registered so any rows written now
            # land in the queue; they'll be de-duped in Phase 2.
            local_cursor = replay_cursor
            while True:
                rows = _fetch_rows_since(db_conn, bot_id, local_cursor, REPLAY_CHUNK_SIZE)
                if not rows:
                    break
                for r in rows:
                    if _matches_any_prefix(r["text"], prefix_list):
                        yield format_sse_memory(r)
                    local_cursor = r["rowid"]
                replayed_max_id = max(replayed_max_id, local_cursor)
                if len(rows) < REPLAY_CHUNK_SIZE:
                    break

            # Phase 2: Drain pubsub queue, de-duping against replay watermark.
            while True:
                try:
                    row = await asyncio.wait_for(queue.get(), timeout=HEARTBEAT_INTERVAL_S)
                except asyncio.TimeoutError:
                    yield format_sse_heartbeat(now_ts=int(time.time()))
                    continue
                if row["rowid"] <= replayed_max_id:
                    continue  # already emitted in replay
                if not _matches_any_prefix(row["text"], prefix_list):
                    continue
                yield format_sse_memory(row)

        except asyncio.CancelledError:
            raise
        except Exception as e:
            logger.warning("sse_stream_error bot_id=%s err=%s", bot_id, e)
            yield format_sse_error(code="internal", message=str(e))
        finally:
            pubsub.unsubscribe(bot_id, queue)

    return StreamingResponse(
        event_generator(),
        media_type="text/event-stream",
        headers={
            "Cache-Control": "no-cache",
            "X-Accel-Buffering": "no",
            "Connection": "keep-alive",
        },
    )
