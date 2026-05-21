"""Unit tests for SSE frame encoders."""
import json

from memory_sidecar.sse_format import (
    format_sse_memory,
    format_sse_heartbeat,
    format_sse_error,
)


def test_format_sse_memory_uses_id_and_event_and_data():
    row = {
        "rowid": 4187234,
        "memory_id": "m_abc123defgh",
        "bot_id": "1003",
        "text": "received whisper from Tebrack: hi",
        "created_ts": 1716321547,
        "salience": 0.7,
    }
    out = format_sse_memory(row)
    assert "event: memory\n" in out
    assert "id: 4187234\n" in out
    assert "data: " in out
    payload_line = [ln for ln in out.split("\n") if ln.startswith("data: ")][0][6:]
    parsed = json.loads(payload_line)
    assert parsed["row_id"] == 4187234
    assert parsed["bot_id"] == "1003"
    assert parsed["text"].startswith("received whisper")
    assert out.endswith("\n\n")  # SSE event terminator


def test_format_sse_heartbeat_no_id():
    out = format_sse_heartbeat(now_ts=1716321552)
    assert "event: heartbeat\n" in out
    assert "id:" not in out
    assert "1716321552" in out
    assert out.endswith("\n\n")


def test_format_sse_error_carries_code_and_message():
    out = format_sse_error(code="bot_id_invalid", message="bot_id must be a string")
    assert "event: error\n" in out
    assert "bot_id_invalid" in out
    assert out.endswith("\n\n")


def test_format_sse_memory_renames_rowid_to_row_id_in_payload():
    """Payload uses row_id (semantic) for the client; SSE id header uses rowid (transport)."""
    row = {
        "rowid": 1,
        "memory_id": "m_xyztest123",
        "bot_id": "x",
        "text": "t",
        "created_ts": 1,
        "salience": 0.5,
    }
    out = format_sse_memory(row)
    data_line = [ln for ln in out.split("\n") if ln.startswith("data: ")][0][6:]
    parsed = json.loads(data_line)
    assert "row_id" in parsed
    assert "rowid" not in parsed  # avoid client confusion with SSE id
    assert parsed["memory_id"] == "m_xyztest123"
