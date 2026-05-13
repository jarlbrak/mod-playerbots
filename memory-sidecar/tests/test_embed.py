import asyncio
import json
from typing import Any

import httpx
import numpy as np
import pytest

from memory_sidecar.embed import EmbeddingClient


class FakeTransport(httpx.AsyncBaseTransport):
    """Replays a fixed JSON response for /v1/embeddings."""

    def __init__(self, embedding: list[float]):
        self.embedding = embedding
        self.calls = 0

    async def handle_async_request(self, request: httpx.Request) -> httpx.Response:
        self.calls += 1
        body = {
            "object": "list",
            "data": [{"object": "embedding", "index": 0, "embedding": self.embedding}],
            "model": "test",
        }
        return httpx.Response(200, json=body)


@pytest.mark.asyncio
async def test_embed_returns_float32_of_correct_dim():
    transport = FakeTransport([0.1] * 384)
    client = EmbeddingClient("http://x", dim=384, transport=transport)
    vec = await client.embed("hello")
    assert isinstance(vec, np.ndarray)
    assert vec.dtype == np.float32
    assert vec.shape == (384,)


@pytest.mark.asyncio
async def test_embed_cache_hit_avoids_http():
    transport = FakeTransport([0.1] * 384)
    client = EmbeddingClient("http://x", dim=384, transport=transport, cache_size=10)
    await client.embed("hello")
    await client.embed("hello")
    await client.embed("hello")
    assert transport.calls == 1


@pytest.mark.asyncio
async def test_embed_different_queries_miss_cache():
    transport = FakeTransport([0.1] * 384)
    client = EmbeddingClient("http://x", dim=384, transport=transport, cache_size=10)
    await client.embed("hello")
    await client.embed("world")
    assert transport.calls == 2


@pytest.mark.asyncio
async def test_embed_raises_on_dimension_mismatch():
    transport = FakeTransport([0.1] * 256)  # wrong dim
    client = EmbeddingClient("http://x", dim=384, transport=transport)
    with pytest.raises(ValueError, match="dimension"):
        await client.embed("hello")
