import pytest
from fastapi.testclient import TestClient

from memory_sidecar.main import create_app


@pytest.fixture
def client(tmp_path, stub_embedder, monkeypatch):
    db_path = str(tmp_path / "mem.sqlite")
    monkeypatch.setenv("MEM_DB_PATH", db_path)
    app = create_app(embedder=stub_embedder)
    with TestClient(app) as c:
        yield c


def _remember(client, bot, text, ents=None, salience=0.5):
    return client.post(
        "/memory/remember",
        json={
            "bot_id": bot,
            "text": text,
            "entities": ents or [],
            "salience": salience,
        },
    ).json()


def test_recall_empty_bot_returns_empty(client):
    r = client.post("/memory/recall", json={"bot_id": "unknown", "query": "x", "top_k": 5})
    assert r.status_code == 200
    assert r.json()["memories"] == []


def test_recall_returns_inserted_memory(client):
    _remember(client, "b1", "killed Murloc")
    r = client.post(
        "/memory/recall", json={"bot_id": "b1", "query": "Murloc", "top_k": 3}
    )
    assert r.status_code == 200
    mems = r.json()["memories"]
    assert len(mems) == 1
    assert mems[0]["text"] == "killed Murloc"


def test_recall_respects_top_k(client):
    for i in range(10):
        _remember(client, "b1", f"event {i}")
    r = client.post("/memory/recall", json={"bot_id": "b1", "query": "x", "top_k": 3})
    assert r.status_code == 200
    assert len(r.json()["memories"]) == 3


def test_recall_isolates_by_bot(client):
    _remember(client, "b1", "alpha")
    _remember(client, "b2", "beta")
    r = client.post("/memory/recall", json={"bot_id": "b1", "query": "x", "top_k": 5})
    texts = [m["text"] for m in r.json()["memories"]]
    assert "alpha" in texts
    assert "beta" not in texts


def test_recall_score_is_present_and_in_range(client):
    _remember(client, "b1", "killed Murloc", salience=0.4)
    r = client.post("/memory/recall", json={"bot_id": "b1", "query": "x", "top_k": 1})
    m = r.json()["memories"][0]
    assert "score" in m
    assert 0.0 <= m["score"] <= 1.5


def test_recall_updates_last_recalled_ts(client, monkeypatch):
    _remember(client, "b1", "first")
    r1 = client.post("/memory/recall", json={"bot_id": "b1", "query": "x", "top_k": 1})
    r2 = client.post("/memory/recall", json={"bot_id": "b1", "query": "x", "top_k": 1})
    assert r1.json()["memories"][0]["text"] == r2.json()["memories"][0]["text"]
