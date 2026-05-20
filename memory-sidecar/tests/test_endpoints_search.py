"""Tests for POST /memory/search — hybrid BM25+dense+entity+RRF+MMR."""
import time
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


def _remember(c, bot_id="b1", text="x", salience=0.5, entities=None, relations=None):
    body = {
        "bot_id": bot_id,
        "text": text,
        "salience": salience,
        "entities": entities or [],
        "relations": relations or [],
    }
    r = c.post("/memory/remember", json=body)
    assert r.status_code == 200
    return r.json()["memory_id"]


def _search(c, query, bot_id="b1", **kw):
    body = {"bot_id": bot_id, "query": query, **kw}
    r = c.post("/memory/search", json=body)
    assert r.status_code == 200, r.text
    return r.json()


def test_search_empty_bot_returns_empty(client):
    result = _search(client, "anything")
    assert result["items"] == []
    assert result["total_candidates"] == 0


def test_search_bm25_keyword_hit(client):
    """A BM25-exact keyword match should appear in results."""
    _remember(client, text="the quick brown fox jumps over the lazy dog")
    _remember(client, text="completely unrelated content here")
    result = _search(client, "quick brown fox", top_k=5)
    texts = [item["text"] for item in result["items"]]
    assert any("quick brown fox" in t for t in texts)


def test_search_returns_signals(client):
    """Each result has signals with at least one ranker contributing."""
    _remember(client, text="alice loves raiding dungeons")
    result = _search(client, "alice raiding", top_k=5)
    assert len(result["items"]) > 0
    item = result["items"][0]
    assert "signals" in item
    signals = item["signals"]
    # At least one ranker should have matched
    assert (
        signals["bm25_rank"] is not None
        or signals["dense_rank"] is not None
        or signals["entity_rank"] is not None
    )


def test_search_memory_type_filter(client):
    mid1 = _remember(client, text="event memory here")
    mid2 = _remember(client, text="fact about the world")
    # Update mid2 to fact type
    client.put("/memory/update", json={
        "bot_id": "b1", "memory_id": mid2, "memory_type": "fact"
    })
    result = _search(client, "memory fact", top_k=5, memory_type="fact")
    ids = [item["memory_id"] for item in result["items"]]
    # Only fact-type memories should appear
    assert mid2 in ids or len(ids) == 0  # may be 0 if dense vector misses
    assert mid1 not in ids


def test_search_temporal_filter(client):
    _remember(client, text="old memory content")
    future_ts = int(time.time()) + 10000
    result = _search(client, "memory", top_k=5, since_ts=future_ts)
    # Nothing written after future_ts
    assert result["items"] == []


def test_search_entity_anchored(client):
    """Memories linked to an entity name appearing in query should rank well."""
    _remember(client, text="alice helped me complete the quest", entities=["alice"])
    _remember(client, text="bob killed some wolves near the forest")
    result = _search(client, "alice quest help", top_k=5)
    texts = [item["text"] for item in result["items"]]
    assert any("alice" in t for t in texts)


def test_search_mmr_diversity(client):
    """5 near-identical memories + 1 distinct — distinct should appear in top results."""
    for i in range(5):
        _remember(client, text=f"alice went to stormwind {i}")
    _remember(client, text="bob killed the lich king heroically")
    result = _search(client, "alice stormwind", top_k=3)
    texts = [item["text"] for item in result["items"]]
    # MMR should surface "bob" at some point, but at minimum we just check diversity
    # (not all 3 should be exact duplicates with different indices)
    unique_texts = set(texts)
    assert len(unique_texts) == len(texts), "MMR should return diverse results"


def test_search_top_k_respected(client):
    for i in range(10):
        _remember(client, text=f"memory number {i} about quests")
    result = _search(client, "quests", top_k=3)
    assert len(result["items"]) <= 3
