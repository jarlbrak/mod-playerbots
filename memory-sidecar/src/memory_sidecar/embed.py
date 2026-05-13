"""Async embedding client backed by llama.cpp's /v1/embeddings."""
from collections import OrderedDict
from typing import Optional

import httpx
import numpy as np


class EmbeddingClient:
    """Wraps llama.cpp's /v1/embeddings with an LRU cache.

    Args:
      endpoint: base URL, e.g. "http://127.0.0.1:8081"
      dim: expected output dimension (384 for bge-small-en-v1.5)
      cache_size: LRU capacity for recent embeddings (text → ndarray)
      transport: optional httpx transport (for testing)
    """

    def __init__(
        self,
        endpoint: str,
        dim: int = 384,
        cache_size: int = 1024,
        transport: Optional[httpx.AsyncBaseTransport] = None,
    ):
        self.endpoint = endpoint.rstrip("/")
        self.dim = dim
        self.cache_size = cache_size
        self._cache: OrderedDict[str, np.ndarray] = OrderedDict()
        self._client = httpx.AsyncClient(transport=transport, timeout=5.0)

    async def embed(self, text: str) -> np.ndarray:
        if text in self._cache:
            self._cache.move_to_end(text)
            return self._cache[text]

        resp = await self._client.post(
            f"{self.endpoint}/v1/embeddings",
            json={"input": text, "model": "embedding"},
        )
        resp.raise_for_status()
        payload = resp.json()
        raw = payload["data"][0]["embedding"]
        if len(raw) != self.dim:
            raise ValueError(
                f"embedding dimension mismatch: expected {self.dim}, got {len(raw)}"
            )
        vec = np.array(raw, dtype=np.float32)

        self._cache[text] = vec
        if len(self._cache) > self.cache_size:
            self._cache.popitem(last=False)
        return vec

    async def aclose(self) -> None:
        await self._client.aclose()
