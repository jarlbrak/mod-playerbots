import os

import pytest
from fastapi.testclient import TestClient

from memory_sidecar.main import create_app


@pytest.fixture
def client_small_cap(tmp_path, stub_embedder, monkeypatch):
    db_path = str(tmp_path / "mem.sqlite")
    monkeypatch.setenv("MEM_DB_PATH", db_path)
    monkeypatch.setenv("MEM_CAP_PER_BOT", "5")
    app = create_app(embedder=stub_embedder)
    with TestClient(app) as c:
        yield c


def _r(client, text, salience):
    return client.post(
        "/memory/remember",
        json={"bot_id": "b1", "text": text, "entities": [], "salience": salience},
    ).json()


def test_eviction_kicks_in_when_over_cap(client_small_cap):
    for i in range(5):
        _r(client_small_cap, f"e{i}", 0.5)
    resp = _r(client_small_cap, "overflow", 0.5)
    assert resp["evicted"] == 1


def test_eviction_keeps_higher_salience(client_small_cap):
    _r(client_small_cap, "low1", 0.1)
    _r(client_small_cap, "low2", 0.1)
    _r(client_small_cap, "low3", 0.1)
    _r(client_small_cap, "low4", 0.1)
    _r(client_small_cap, "low5", 0.1)
    _r(client_small_cap, "high", 0.9)

    r = client_small_cap.post(
        "/memory/recall", json={"bot_id": "b1", "query": "x", "top_k": 10}
    )
    texts = [m["text"] for m in r.json()["memories"]]
    assert "high" in texts


def test_eviction_is_per_bot(client_small_cap):
    for i in range(10):
        client_small_cap.post(
            "/memory/remember",
            json={"bot_id": "b_other", "text": f"x{i}", "entities": [],
                  "salience": 0.5},
        )
    r = client_small_cap.post(
        "/memory/recall", json={"bot_id": "b_other", "query": "x", "top_k": 100}
    )
    assert len(r.json()["memories"]) == 5


def test_eviction_zero_when_under_cap(client_small_cap):
    for i in range(3):
        resp = _r(client_small_cap, f"e{i}", 0.5)
        assert resp["evicted"] == 0
