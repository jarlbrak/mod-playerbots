"""Tests for /goals/* endpoints — create/read/update/list/complete."""
from pathlib import Path

import pytest
from fastapi.testclient import TestClient

from memory_sidecar.main import create_app


@pytest.fixture
def client(tmp_path, monkeypatch, stub_embedder):
    monkeypatch.setenv("MEM_DB_PATH", str(tmp_path / "test.sqlite"))
    monkeypatch.setenv(
        "MEM_MIGRATIONS_DIR",
        str(Path(__file__).resolve().parent.parent / "migrations"),
    )
    with TestClient(create_app(embedder=stub_embedder)) as c:
        yield c


def _create(c, bot_id="b1", text="grind to 15", **kw):
    body = {"bot_id": bot_id, "text": text, **kw}
    r = c.post("/goals/create", json=body)
    assert r.status_code == 200, r.text
    return r.json()["goal_id"]


def test_create_returns_pending(client):
    gid = _create(client, text="grind to level 15")
    r = client.get(f"/goals/{gid}?bot_id=b1")
    assert r.status_code == 200
    body = r.json()
    assert body["id"] == gid
    assert body["status"] == "pending"
    assert body["text"] == "grind to level 15"


def test_read_not_found(client):
    r = client.get("/goals/g_nonexistent?bot_id=b1")
    assert r.status_code == 404


def test_update_pending_to_active(client):
    gid = _create(client)
    r = client.put("/goals/update", json={"bot_id": "b1", "goal_id": gid, "status": "active"})
    assert r.status_code == 200
    assert r.json()["updated"] is True
    body = client.get(f"/goals/{gid}?bot_id=b1").json()
    assert body["status"] == "active"


def test_update_active_to_completed(client):
    gid = _create(client)
    client.put("/goals/update", json={"bot_id": "b1", "goal_id": gid, "status": "active"})
    r = client.put("/goals/update", json={"bot_id": "b1", "goal_id": gid, "status": "completed"})
    assert r.status_code == 200
    body = client.get(f"/goals/{gid}?bot_id=b1").json()
    assert body["status"] == "completed"
    assert body["completed_ts"] is not None


def test_invalid_transition_completed_to_pending(client):
    gid = _create(client)
    # Go pending → active → completed
    client.put("/goals/update", json={"bot_id": "b1", "goal_id": gid, "status": "active"})
    client.put("/goals/update", json={"bot_id": "b1", "goal_id": gid, "status": "completed"})
    # Try to go back — should fail
    r = client.put("/goals/update", json={"bot_id": "b1", "goal_id": gid, "status": "pending"})
    assert r.status_code == 400


def test_list_filtered_by_status(client):
    gid1 = _create(client, text="goal one")
    gid2 = _create(client, text="goal two")
    # Activate gid2
    client.put("/goals/update", json={"bot_id": "b1", "goal_id": gid2, "status": "active"})
    r = client.get("/goals/list?bot_id=b1&status=active")
    assert r.status_code == 200
    body = r.json()
    ids = [item["id"] for item in body["items"]]
    assert gid2 in ids
    assert gid1 not in ids


def test_complete_records_memory(client):
    gid = _create(client, text="kill the lich king")
    # Must be active before completing
    client.put("/goals/update", json={"bot_id": "b1", "goal_id": gid, "status": "active"})
    r = client.post("/goals/complete", json={
        "bot_id": "b1", "goal_id": gid, "outcome": "completed",
        "also_record_memory": True,
    })
    assert r.status_code == 200
    body = r.json()
    assert body["updated"] is True
    assert body["memory_id"] is not None
    # Verify the memory was actually written
    mr = client.post("/memory/recall", json={
        "bot_id": "b1", "query": "lich king", "top_k": 5
    })
    texts = [m["text"] for m in mr.json()["memories"]]
    assert any("lich king" in t for t in texts)


def test_complete_already_terminal_returns_400(client):
    gid = _create(client)
    client.put("/goals/update", json={"bot_id": "b1", "goal_id": gid, "status": "active"})
    client.post("/goals/complete", json={
        "bot_id": "b1", "goal_id": gid, "outcome": "completed",
        "also_record_memory": False,
    })
    # Try again — already terminal
    r = client.post("/goals/complete", json={
        "bot_id": "b1", "goal_id": gid, "outcome": "completed",
        "also_record_memory": False,
    })
    assert r.status_code == 400
