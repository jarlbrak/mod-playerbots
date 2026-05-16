"""Shared pytest fixtures for the memory sidecar."""
import numpy as np
import pytest


class StubEmbeddingClient:
    """Deterministic hash-to-vector embeddings. No model load."""

    def __init__(self, dim: int = 384):
        self.dim = dim
        self.calls = 0

    async def embed(self, text: str) -> np.ndarray:
        self.calls += 1
        # Deterministic: hash the text into a 384-float32 vector in [0, 1).
        rng = np.random.default_rng(abs(hash(text)) % (2**32))
        return rng.random(self.dim, dtype=np.float32)


@pytest.fixture
def stub_embedder() -> StubEmbeddingClient:
    return StubEmbeddingClient()
