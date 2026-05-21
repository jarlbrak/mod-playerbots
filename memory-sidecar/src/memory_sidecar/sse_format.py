"""SSE frame encoders. Each frame ends with a blank line (the event terminator).

Row dict shape expected by format_sse_memory:
    {
        "rowid":     int,    -- SQLite rowid (monotonic integer; used as SSE transport id)
        "memory_id": str,    -- string PK e.g. "m_abc123defgh"
        "bot_id":    str,
        "text":      str,
        "created_ts":int,    -- unix epoch seconds
        "salience":  float,
    }
"""
from __future__ import annotations

import json
from typing import Any


def format_sse_memory(row: dict[str, Any]) -> str:
    """Encode a memory row as an SSE 'memory' event frame."""
    payload = {
        "row_id": row["rowid"],        # integer for Last-Event-ID cursor
        "memory_id": row["memory_id"],  # string PK for client dedup
        "bot_id": row["bot_id"],
        "text": row["text"],
        "ts": row["created_ts"],
        "salience": row.get("salience", 0.5),
    }
    return (
        f"event: memory\n"
        f"id: {row['rowid']}\n"
        f"data: {json.dumps(payload, separators=(',', ':'))}\n"
        f"\n"
    )


def format_sse_heartbeat(now_ts: int) -> str:
    """Encode a heartbeat frame (no id — does not advance Last-Event-ID)."""
    return f"event: heartbeat\ndata: {json.dumps({'ts': now_ts})}\n\n"


def format_sse_error(*, code: str, message: str) -> str:
    """Encode an error frame."""
    return f"event: error\ndata: {json.dumps({'code': code, 'message': message})}\n\n"
