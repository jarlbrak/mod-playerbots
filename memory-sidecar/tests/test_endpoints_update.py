"""Tests for PUT /memory/update — R1-Update closure."""
import time
from pathlib import Path

import pytest
from fastapi.testclient import TestClient

from memory_sidecar.main import create_app


@pytest.fixture
def client(tmp_path, monkeypatch, stub_embedder):
    db = tmp_path / "test.sqlite"
    monkeypatch.setenv("MEM_DB_PATH", str(db))
    monkeypatch.setenv(
        "MEM_MIGRATIONS_DIR",
        str(Path(__file__).resolve().parent.parent / "migrations"),
    )
    app = create_app(embedder=stub_embedder)
    with TestClient(app) as c:
        yield c


def _write_one(client, bot_id="b1", text="hello world", salience=0.5):
    r = client.post(
        "/memory/remember",
        json={"bot_id": bot_id, "text": text, "salience": salience, "entities": [], "relations": []},
    )
    assert r.status_code == 200
    return r.json()["memory_id"]


def test_update_text_re_embeds(client):
    mid = _write_one(client, text="initial text")
    r = client.put(
        "/memory/update",
        json={"bot_id": "b1", "memory_id": mid, "text": "updated text"},
    )
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["updated"] is True
    assert body["re_embedded"] is True
    # Read-back via the new GET endpoint (Task 10) or direct recall.
    r2 = client.post("/memory/recall", json={"bot_id": "b1", "query": "updated", "top_k": 5})
    texts = [m["text"] for m in r2.json()["memories"]]
    assert "updated text" in texts


def test_update_metadata_only_skips_re_embed(client):
    mid = _write_one(client, salience=0.3)
    r = client.put(
        "/memory/update",
        json={"bot_id": "b1", "memory_id": mid, "salience": 0.9},
    )
    body = r.json()
    assert body["updated"] is True
    assert body["re_embedded"] is False


def test_update_unknown_memory_returns_404(client):
    r = client.put(
        "/memory/update",
        json={"bot_id": "b1", "memory_id": "m_nonexistent", "text": "x"},
    )
    assert r.status_code == 404


def test_update_no_fields_returns_400(client):
    mid = _write_one(client)
    r = client.put("/memory/update", json={"bot_id": "b1", "memory_id": mid})
    assert r.status_code == 400


def test_update_with_memory_type(client):
    mid = _write_one(client)
    r = client.put(
        "/memory/update",
        json={"bot_id": "b1", "memory_id": mid, "memory_type": "fact"},
    )
    assert r.status_code == 200
    assert r.json()["updated"] is True
