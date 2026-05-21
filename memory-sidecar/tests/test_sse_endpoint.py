"""SSE endpoint integration tests using a real uvicorn server.

httpx.ASGITransport buffers the full response body before returning, making it
incompatible with infinite SSE streams. These tests spin up a real uvicorn server
on a random port so httpx can receive chunks as they arrive.

Tests cover:
  - subscribe + write + receive within 3s
  - replay on Last-Event-ID: receive only missed rows
  - prefix filter: non-matching writes do NOT emit events
  - different bot_id isolation
"""
import asyncio
import json
import socket

import httpx
import pytest
import uvicorn

from memory_sidecar.main import create_app

TEST_BEARER = "test-sse-token-456"


def _parse_sse_events(text: str) -> list[dict]:
    """Parse SSE text into a list of event dicts."""
    events = []
    current: dict = {"event": "message", "id": None, "data": []}
    for line in text.split("\n"):
        line = line.rstrip("\r")
        if line == "":
            if current["data"]:
                events.append({
                    "event": current["event"],
                    "id": current["id"],
                    "data": "\n".join(current["data"]),
                })
            current = {"event": "message", "id": None, "data": []}
        elif line.startswith("event: "):
            current["event"] = line[7:]
        elif line.startswith("id: "):
            current["id"] = line[4:]
        elif line.startswith("data: "):
            current["data"].append(line[6:])
    return events


def _free_port() -> int:
    """Find a free TCP port on localhost."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


@pytest.fixture
async def live_server(tmp_path, stub_embedder, monkeypatch):
    """Start a real uvicorn server; yield (base_url, app); stop on teardown."""
    monkeypatch.setenv("MEM_DB_PATH", str(tmp_path / "mem.sqlite"))
    monkeypatch.setenv("MEM_SSE_BEARER", TEST_BEARER)
    app = create_app(embedder=stub_embedder)
    port = _free_port()
    config = uvicorn.Config(app, host="127.0.0.1", port=port, log_level="error")
    server = uvicorn.Server(config)

    server_task = asyncio.create_task(server.serve())
    # Wait for startup
    for _ in range(40):
        await asyncio.sleep(0.05)
        if server.started:
            break
    else:
        server_task.cancel()
        pytest.fail("uvicorn did not start within 2s")

    base_url = f"http://127.0.0.1:{port}"
    yield base_url, app

    server.should_exit = True
    await asyncio.wait_for(server_task, timeout=5.0)


@pytest.mark.asyncio
async def test_subscribe_receives_live_write(live_server):
    """Subscribe; write a matching memory; receive event within 3s."""
    base_url, _ = live_server
    headers = {"Authorization": f"Bearer {TEST_BEARER}"}

    received: list[dict] = []

    async def writer():
        await asyncio.sleep(0.1)
        async with httpx.AsyncClient(base_url=base_url) as wc:
            await wc.post(
                "/memory/remember",
                json={
                    "bot_id": "1003",
                    "text": "received whisper from Tebrack: hi",
                    "salience": 0.7,
                    "entities": ["1003", "Tebrack"],
                    "relations": [],
                },
            )

    async def reader():
        async with httpx.AsyncClient(base_url=base_url, timeout=httpx.Timeout(connect=5, read=None, write=5, pool=5)) as rc:
            async with rc.stream(
                "GET",
                "/v1/events/stream?bot_id=1003&prefixes=received+whisper,chat_received",
                headers=headers,
            ) as response:
                assert response.status_code == 200
                assert "text/event-stream" in response.headers["content-type"]
                buf = ""
                async with asyncio.timeout(3.0):
                    async for chunk in response.aiter_text():
                        buf += chunk
                        events = _parse_sse_events(buf)
                        memory_events = [e for e in events if e["event"] == "memory"]
                        if memory_events:
                            payload = json.loads(memory_events[0]["data"])
                            received.append(payload)
                            break

    writer_task = asyncio.create_task(writer())
    await reader()
    await writer_task

    assert len(received) == 1
    assert received[0]["text"].startswith("received whisper")
    assert "row_id" in received[0]
    assert "memory_id" in received[0]
    assert received[0]["bot_id"] == "1003"


@pytest.mark.asyncio
async def test_replay_on_last_event_id(live_server):
    """Pre-seed two writes; subscribe with Last-Event-ID after first; receive only second."""
    base_url, _ = live_server
    headers = {"Authorization": f"Bearer {TEST_BEARER}"}
    seen_ids: list[int] = []

    # Phase 1: subscribe live to observe both row_ids.
    async def writer_two():
        await asyncio.sleep(0.1)
        async with httpx.AsyncClient(base_url=base_url) as wc:
            await wc.post(
                "/memory/remember",
                json={"bot_id": "1003", "text": "received whisper old", "salience": 0.5,
                      "entities": [], "relations": []},
            )
            await asyncio.sleep(0.05)
            await wc.post(
                "/memory/remember",
                json={"bot_id": "1003", "text": "received whisper new", "salience": 0.5,
                      "entities": [], "relations": []},
            )

    async def reader_two():
        async with httpx.AsyncClient(base_url=base_url, timeout=httpx.Timeout(connect=5, read=None, write=5, pool=5)) as rc:
            async with rc.stream(
                "GET",
                "/v1/events/stream?bot_id=1003&prefixes=received+whisper",
                headers=headers,
            ) as response:
                buf = ""
                async with asyncio.timeout(4.0):
                    async for chunk in response.aiter_text():
                        buf += chunk
                        for ev in _parse_sse_events(buf):
                            if ev["event"] == "memory":
                                payload = json.loads(ev["data"])
                                if payload["row_id"] not in seen_ids:
                                    seen_ids.append(payload["row_id"])
                        if len(seen_ids) >= 2:
                            break

    writer_task = asyncio.create_task(writer_two())
    await reader_two()
    await writer_task

    assert len(seen_ids) == 2
    first_row_id = seen_ids[0]
    second_row_id = seen_ids[1]

    # Phase 2: reconnect with Last-Event-ID = first_row_id; only second arrives.
    replay_received: list[int] = []
    async with httpx.AsyncClient(base_url=base_url, timeout=httpx.Timeout(connect=5, read=None, write=5, pool=5)) as rc2:
        async with rc2.stream(
            "GET",
            "/v1/events/stream?bot_id=1003&prefixes=received+whisper",
            headers={**headers, "Last-Event-ID": str(first_row_id)},
        ) as response:
            assert response.status_code == 200
            buf = ""
            async with asyncio.timeout(2.0):
                async for chunk in response.aiter_text():
                    buf += chunk
                    for ev in _parse_sse_events(buf):
                        if ev["event"] == "memory":
                            payload = json.loads(ev["data"])
                            if payload["row_id"] not in replay_received:
                                replay_received.append(payload["row_id"])
                    if replay_received:
                        break

    assert replay_received == [second_row_id]
    assert first_row_id not in replay_received


@pytest.mark.asyncio
async def test_subscribe_excludes_non_matching_prefix(live_server):
    """Subscribe with one prefix; write with different prefix; no event within 700ms."""
    base_url, _ = live_server
    headers = {"Authorization": f"Bearer {TEST_BEARER}"}
    got_memory_event = False

    async def writer():
        await asyncio.sleep(0.1)
        async with httpx.AsyncClient(base_url=base_url) as wc:
            await wc.post(
                "/memory/remember",
                json={"bot_id": "1003", "text": "brain_no_op: nothing to do",
                      "salience": 0.1, "entities": [], "relations": []},
            )

    async def reader():
        nonlocal got_memory_event
        async with httpx.AsyncClient(base_url=base_url, timeout=httpx.Timeout(connect=5, read=None, write=5, pool=5)) as rc:
            async with rc.stream(
                "GET",
                "/v1/events/stream?bot_id=1003&prefixes=received+whisper",
                headers=headers,
            ) as response:
                buf = ""
                try:
                    async with asyncio.timeout(0.8):
                        async for chunk in response.aiter_text():
                            buf += chunk
                            if any(e["event"] == "memory" for e in _parse_sse_events(buf)):
                                got_memory_event = True
                                break
                except asyncio.TimeoutError:
                    pass

    writer_task = asyncio.create_task(writer())
    await reader()
    await writer_task

    assert not got_memory_event, "non-matching prefix should not emit memory event"


@pytest.mark.asyncio
async def test_different_bot_id_does_not_receive_other_bot_events(live_server):
    """Events for bot 9999 must not arrive on bot 1003's stream."""
    base_url, _ = live_server
    headers = {"Authorization": f"Bearer {TEST_BEARER}"}
    got_event = False

    async def writer():
        await asyncio.sleep(0.1)
        async with httpx.AsyncClient(base_url=base_url) as wc:
            await wc.post(
                "/memory/remember",
                json={"bot_id": "9999", "text": "received whisper from Tebrack: hi",
                      "salience": 0.7, "entities": [], "relations": []},
            )

    async def reader():
        nonlocal got_event
        async with httpx.AsyncClient(base_url=base_url, timeout=httpx.Timeout(connect=5, read=None, write=5, pool=5)) as rc:
            async with rc.stream(
                "GET",
                "/v1/events/stream?bot_id=1003&prefixes=received+whisper",
                headers=headers,
            ) as response:
                buf = ""
                try:
                    async with asyncio.timeout(0.8):
                        async for chunk in response.aiter_text():
                            buf += chunk
                            if any(e["event"] == "memory" for e in _parse_sse_events(buf)):
                                got_event = True
                                break
                except asyncio.TimeoutError:
                    pass

    writer_task = asyncio.create_task(writer())
    await reader()
    await writer_task

    assert not got_event, "events for a different bot must not arrive on this bot's stream"
