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


def _remember(client, bot, text, ents=None, salience=0.5, rels=None):
    return client.post(
        "/memory/remember",
        json={
            "bot_id": bot, "text": text,
            "entities": ents or [], "salience": salience,
            "relations": rels or [],
        },
    ).json()


def test_recall_about_unknown_entity_returns_empty(client):
    r = client.post(
        "/memory/recall_about",
        json={"bot_id": "b1", "entity": "Stormwind", "max_hops": 2, "top_k": 3},
    )
    assert r.status_code == 200
    assert r.json()["hints"] == []


def test_recall_about_finds_directly_attached_memory(client):
    _remember(client, "b1", "turned in quest in Tarren Mill", ents=["Tarren Mill"])
    r = client.post(
        "/memory/recall_about",
        json={"bot_id": "b1", "entity": "Tarren Mill", "max_hops": 1, "top_k": 3},
    )
    hints = r.json()["hints"]
    assert len(hints) == 1
    assert "Tarren Mill" in hints[0]


def test_recall_about_walks_two_hops(client):
    _remember(
        client, "b1",
        "killed Syndicate Footpad",
        ents=["Syndicate Footpad", "Tarren Mill"],
        rels=[{"src": "Syndicate Footpad", "rel": "located_in", "dst": "Tarren Mill"}],
    )
    r = client.post(
        "/memory/recall_about",
        json={"bot_id": "b1", "entity": "Tarren Mill", "max_hops": 2, "top_k": 3},
    )
    hints = r.json()["hints"]
    assert any("Syndicate Footpad" in h for h in hints)


def test_recall_about_isolates_by_bot(client):
    _remember(client, "b1", "alpha event", ents=["Westfall"])
    _remember(client, "b2", "beta event", ents=["Westfall"])
    r = client.post(
        "/memory/recall_about",
        json={"bot_id": "b1", "entity": "Westfall", "max_hops": 1, "top_k": 5},
    )
    hints = r.json()["hints"]
    assert any("alpha" in h for h in hints)
    assert not any("beta" in h for h in hints)


def test_recall_about_case_insensitive(client):
    _remember(client, "b1", "event in Westfall", ents=["Westfall"])
    r = client.post(
        "/memory/recall_about",
        json={"bot_id": "b1", "entity": "westfall", "max_hops": 1, "top_k": 3},
    )
    assert len(r.json()["hints"]) == 1


def test_recall_about_respects_top_k(client):
    for i in range(10):
        _remember(client, "b1", f"event {i} in Westfall", ents=["Westfall"])
    r = client.post(
        "/memory/recall_about",
        json={"bot_id": "b1", "entity": "Westfall", "max_hops": 1, "top_k": 3},
    )
    assert len(r.json()["hints"]) == 3
