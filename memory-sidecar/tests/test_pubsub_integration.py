"""Integration test: HTTP write triggers pubsub.publish for SSE subscribers."""
import asyncio

import pytest
from fastapi.testclient import TestClient

from memory_sidecar.main import create_app


@pytest.fixture
def app_with_pubsub(tmp_path, stub_embedder, monkeypatch):
    db_path = str(tmp_path / "mem.sqlite")
    monkeypatch.setenv("MEM_DB_PATH", db_path)
    app = create_app(embedder=stub_embedder)
    return app


@pytest.mark.asyncio
async def test_http_write_publishes_to_pubsub(app_with_pubsub):
    """Writing via /memory/remember triggers pubsub.publish for the bot."""
    pubsub = app_with_pubsub.state.pubsub
    q = pubsub.subscribe("1003")

    with TestClient(app_with_pubsub) as client:
        response = client.post(
            "/memory/remember",
            json={
                "bot_id": "1003",
                "text": "received whisper from Tebrack: hi",
                "salience": 0.7,
                "entities": ["1003", "Tebrack"],
                "relations": [],
            },
        )
        assert response.status_code == 200

    # Queue should have received the row synchronously (publish is non-blocking in-memory)
    row = q.get_nowait()
    assert row["bot_id"] == "1003"
    assert row["text"].startswith("received whisper")
    assert "rowid" in row  # the integer rowid assigned by sqlite
    assert "memory_id" in row  # the string PK


@pytest.mark.asyncio
async def test_http_write_does_not_publish_to_other_bot(app_with_pubsub):
    """Write for bot 1003 does NOT publish to bot 9999's subscriber."""
    pubsub = app_with_pubsub.state.pubsub
    q_other = pubsub.subscribe("9999")

    with TestClient(app_with_pubsub) as client:
        client.post(
            "/memory/remember",
            json={
                "bot_id": "1003",
                "text": "received whisper from Tebrack: hi",
                "salience": 0.7,
                "entities": [],
                "relations": [],
            },
        )

    assert q_other.empty()


@pytest.mark.asyncio
async def test_pubsub_accessible_on_app_state(app_with_pubsub):
    """app.state.pubsub is a PubSub instance."""
    from memory_sidecar.pubsub import PubSub
    assert isinstance(app_with_pubsub.state.pubsub, PubSub)
