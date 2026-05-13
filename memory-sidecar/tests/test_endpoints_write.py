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


def test_health_returns_ok(client):
    r = client.get("/health")
    assert r.status_code == 200
    assert r.json()["ok"] is True


def test_remember_inserts_memory(client):
    r = client.post(
        "/memory/remember",
        json={
            "bot_id": "12345",
            "text": "killed Murloc in Westfall",
            "entities": ["Murloc", "Westfall"],
            "salience": 0.1,
        },
    )
    assert r.status_code == 200
    body = r.json()
    assert body["memory_id"].startswith("m_")
    assert body["evicted"] == 0


def test_remember_creates_entities(client):
    client.post(
        "/memory/remember",
        json={
            "bot_id": "12345",
            "text": "x",
            "entities": ["Murloc", "Westfall"],
            "salience": 0.1,
        },
    )
    r = client.post(
        "/memory/remember",
        json={
            "bot_id": "12345",
            "text": "y",
            "entities": ["Murloc", "Westfall"],
            "salience": 0.2,
        },
    )
    assert r.status_code == 200


def test_remember_rejects_missing_fields(client):
    r = client.post("/memory/remember", json={"bot_id": "12345"})
    assert r.status_code == 422


def test_remember_clamps_salience(client):
    r = client.post(
        "/memory/remember",
        json={
            "bot_id": "12345",
            "text": "x",
            "entities": [],
            "salience": 5.0,
        },
    )
    assert r.status_code == 200


def test_forget_removes_memory(client):
    r1 = client.post(
        "/memory/remember",
        json={"bot_id": "12345", "text": "x", "entities": [], "salience": 0.5},
    )
    memory_id = r1.json()["memory_id"]
    r2 = client.post("/memory/forget", json={"bot_id": "12345", "memory_id": memory_id})
    assert r2.status_code == 200
    assert r2.json()["forgotten"] is True


def test_forget_unknown_id_returns_false(client):
    r = client.post(
        "/memory/forget", json={"bot_id": "12345", "memory_id": "m_nonexistent"}
    )
    assert r.status_code == 200
    assert r.json()["forgotten"] is False


def test_remember_with_relations_writes_edges(client):
    r = client.post(
        "/memory/remember",
        json={
            "bot_id": "12345",
            "text": "killed Murloc in Westfall",
            "entities": ["Murloc", "Westfall"],
            "salience": 0.1,
            "relations": [
                {"src": "Murloc", "rel": "located_in", "dst": "Westfall"}
            ],
        },
    )
    assert r.status_code == 200
