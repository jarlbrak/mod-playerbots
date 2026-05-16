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


def test_get_unknown_returns_404(client):
    r = client.post("/memory/personality/get", json={"bot_id": "nope"})
    assert r.status_code == 404


def test_set_then_get_round_trips(client):
    set_r = client.post(
        "/memory/personality/set",
        json={"bot_id": "b1", "persona": "Orc warrior, level 37, in Hillsbrad."},
    )
    assert set_r.status_code == 200
    assert set_r.json()["ok"] is True

    get_r = client.post("/memory/personality/get", json={"bot_id": "b1"})
    assert get_r.status_code == 200
    assert "Orc warrior" in get_r.json()["persona"]


def test_set_replaces_existing(client):
    client.post("/memory/personality/set", json={"bot_id": "b1", "persona": "v1"})
    client.post("/memory/personality/set", json={"bot_id": "b1", "persona": "v2"})
    r = client.post("/memory/personality/get", json={"bot_id": "b1"})
    assert r.json()["persona"] == "v2"


def test_set_rejects_persona_too_long(client):
    huge = "x" * 5000
    r = client.post(
        "/memory/personality/set", json={"bot_id": "b1", "persona": huge}
    )
    assert r.status_code == 400
