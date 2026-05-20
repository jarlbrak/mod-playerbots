"""Tests for GET /memory/{id} and GET /memory/list."""
import pytest
from pathlib import Path
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


def _remember(c, **kw):
    body = {"bot_id": "b1", "text": "x", "salience": 0.5, "entities": [], "relations": []}
    body.update(kw)
    r = c.post("/memory/remember", json=body)
    assert r.status_code == 200
    return r.json()["memory_id"]


def test_get_memory_by_id(client):
    mid = _remember(client, text="alice hello")
    r = client.get(f"/memory/{mid}?bot_id=b1")
    assert r.status_code == 200
    body = r.json()
    assert body["id"] == mid
    assert body["text"] == "alice hello"
    assert body["memory_type"] == "event"
    assert body["source"] is None


def test_get_memory_not_found(client):
    r = client.get("/memory/m_nonexistent?bot_id=b1")
    assert r.status_code == 404


def test_list_memories_default(client):
    for i in range(3):
        _remember(client, text=f"memory {i}")
    r = client.get("/memory/list?bot_id=b1")
    assert r.status_code == 200
    body = r.json()
    assert body["total"] == 3
    assert len(body["items"]) == 3


def test_list_memories_filter_by_type(client):
    mid = _remember(client, text="ev1")
    # Update one to fact type
    client.put("/memory/update", json={"bot_id": "b1", "memory_id": mid, "memory_type": "fact"})
    _remember(client, text="ev2")
    r = client.get("/memory/list?bot_id=b1&memory_type=fact")
    assert r.status_code == 200
    body = r.json()
    assert body["total"] == 1
    assert body["items"][0]["memory_type"] == "fact"


def test_list_memories_temporal_filter(client):
    import time
    _remember(client, text="old")
    r = client.get(f"/memory/list?bot_id=b1&since_ts={int(time.time()) - 10000}")
    assert r.status_code == 200
