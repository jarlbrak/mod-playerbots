"""Unit tests for SSE route input validation + auth (no streaming)."""
import pytest
from fastapi.testclient import TestClient

from memory_sidecar.main import create_app

TEST_BEARER = "test-sse-token-123"


@pytest.fixture
def memory_sidecar_app(tmp_path, stub_embedder, monkeypatch):
    """App fixture with SSE bearer configured."""
    monkeypatch.setenv("MEM_DB_PATH", str(tmp_path / "mem.sqlite"))
    monkeypatch.setenv("MEM_SSE_BEARER", TEST_BEARER)
    app = create_app(embedder=stub_embedder)
    return app


def test_missing_bot_id_returns_400(memory_sidecar_app):
    client = TestClient(memory_sidecar_app)
    r = client.get(
        "/v1/events/stream?prefixes=received+whisper",
        headers={"Authorization": f"Bearer {TEST_BEARER}"},
    )
    assert r.status_code == 400
    assert "bot_id" in r.text.lower()


def test_missing_prefixes_returns_400(memory_sidecar_app):
    client = TestClient(memory_sidecar_app)
    r = client.get(
        "/v1/events/stream?bot_id=1003",
        headers={"Authorization": f"Bearer {TEST_BEARER}"},
    )
    assert r.status_code == 400
    assert "prefixes" in r.text.lower()


def test_too_many_prefixes_returns_400(memory_sidecar_app):
    client = TestClient(memory_sidecar_app)
    prefixes = ",".join([f"p{i}" for i in range(17)])
    r = client.get(
        f"/v1/events/stream?bot_id=1003&prefixes={prefixes}",
        headers={"Authorization": f"Bearer {TEST_BEARER}"},
    )
    assert r.status_code == 400


def test_missing_auth_returns_401(memory_sidecar_app):
    client = TestClient(memory_sidecar_app)
    r = client.get("/v1/events/stream?bot_id=1003&prefixes=received+whisper")
    assert r.status_code in (401, 403)


def test_wrong_bearer_returns_401(memory_sidecar_app):
    client = TestClient(memory_sidecar_app)
    r = client.get(
        "/v1/events/stream?bot_id=1003&prefixes=received+whisper",
        headers={"Authorization": "Bearer wrong"},
    )
    assert r.status_code in (401, 403)


def test_bot_id_too_long_returns_400(memory_sidecar_app):
    client = TestClient(memory_sidecar_app)
    long_id = "x" * 33
    r = client.get(
        f"/v1/events/stream?bot_id={long_id}&prefixes=received+whisper",
        headers={"Authorization": f"Bearer {TEST_BEARER}"},
    )
    assert r.status_code == 400


def test_prefix_too_long_returns_400(memory_sidecar_app):
    client = TestClient(memory_sidecar_app)
    long_prefix = "p" * 65
    r = client.get(
        f"/v1/events/stream?bot_id=1003&prefixes={long_prefix}",
        headers={"Authorization": f"Bearer {TEST_BEARER}"},
    )
    assert r.status_code == 400


def test_no_sse_bearer_configured_disables_auth(monkeypatch):
    """When MEM_SSE_BEARER is not set, _check_bearer passes without a token."""
    monkeypatch.delenv("MEM_SSE_BEARER", raising=False)
    from memory_sidecar.routes_events import _check_bearer
    # _check_bearer should not raise when no bearer is configured,
    # even if authorization header is absent.
    _check_bearer(request=None, authorization=None)  # type: ignore[arg-type]
    _check_bearer(request=None, authorization="Bearer anything")  # type: ignore[arg-type]
