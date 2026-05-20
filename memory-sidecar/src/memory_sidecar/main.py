"""FastAPI app factory for the memory sidecar."""
import os
from contextlib import asynccontextmanager
from typing import Any, Optional

from fastapi import FastAPI

from memory_sidecar.db import open_db, run_migrations as run_legacy_migrations
from memory_sidecar.embed import EmbeddingClient
from memory_sidecar.recall import ScoringWeights
from memory_sidecar import routes_memory, routes_personality


def create_app(embedder: Optional[Any] = None) -> FastAPI:
    db_path = os.environ.get("MEM_DB_PATH", "/var/memory/db.sqlite")
    embed_endpoint = os.environ.get("MEM_EMBED_ENDPOINT", "http://127.0.0.1:8081")
    cap_per_bot = int(os.environ.get("MEM_CAP_PER_BOT", "2000"))

    state: dict[str, Any] = {
        "db_path": db_path,
        "cap_per_bot": cap_per_bot,
        "embedder": embedder,
        "embed_endpoint": embed_endpoint,
        "weights": ScoringWeights(
            w_rel=float(os.environ.get("MEM_W_REL", "0.5")),
            w_rec=float(os.environ.get("MEM_W_REC", "0.2")),
            w_imp=float(os.environ.get("MEM_W_IMP", "0.3")),
            tau_seconds=int(os.environ.get("MEM_TAU_SECONDS", "604800")),
        ),
    }

    @asynccontextmanager
    async def lifespan(app: FastAPI):
        conn = open_db(state["db_path"])
        run_legacy_migrations(conn)  # IF NOT EXISTS baseline (v0.1 schema)
        state["conn"] = conn
        if state["embedder"] is None:
            state["embedder"] = EmbeddingClient(state["embed_endpoint"], dim=384)
        yield
        conn.close()
        if hasattr(state["embedder"], "aclose"):
            await state["embedder"].aclose()

    app = FastAPI(lifespan=lifespan, title="memory-sidecar")
    app.state.mem = state

    @app.get("/health")
    async def health():
        return {"ok": True}

    app.include_router(routes_memory.build_router(state))
    app.include_router(routes_personality.build_router(state))
    return app
