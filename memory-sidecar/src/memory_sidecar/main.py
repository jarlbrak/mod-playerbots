"""FastAPI app factory for the memory sidecar."""
import os
from contextlib import asynccontextmanager
from pathlib import Path
from typing import Any, Optional

from fastapi import FastAPI

from memory_sidecar.db import open_db, run_migrations as run_legacy_migrations
from memory_sidecar.embed import EmbeddingClient
from memory_sidecar.migrations import apply_migrations
from memory_sidecar.recall import ScoringWeights
from memory_sidecar import routes_memory, routes_personality, routes_goals
from memory_sidecar.mcp_auth import TokenStore
from memory_sidecar.mcp_server import build_mcp_server


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
        "recency_basis": os.environ.get("MEM_W_REC_TIMESTAMP", "created"),
        "mmr_lambda": float(os.environ.get("MEM_MMR_LAMBDA", "0.7")),
    }

    # Deferred-init holder for the MCP server. The lifespan checks this at
    # call time; if populated by the MCP build block below, the lifespan
    # wraps yield with `session_manager.run()` so the streamable-HTTP
    # transport's session manager is alive for the duration of the app.
    # Mirrors the harness-daemon pattern (kb_642162c3 app.py:240-258).
    mcp_holder: dict[str, Any] = {}

    @asynccontextmanager
    async def lifespan(app: FastAPI):
        conn = open_db(state["db_path"])
        # Legacy idempotent baseline (every CREATE has IF NOT EXISTS).
        # Safe to run on existing prod DB.
        run_legacy_migrations(conn)
        # New versioned migrations (0001 is the same baseline, so it
        # becomes a no-op on prod DBs already at-schema; just inserts
        # the schema_version=1 row).
        migrations_dir = os.environ.get(
            "MEM_MIGRATIONS_DIR",
            str(Path(__file__).resolve().parent.parent.parent / "migrations"),
        )
        n = apply_migrations(conn, migrations_dir)
        if n:
            print(f"[migrations] applied {n} migration(s); schema_version now updated")
        state["conn"] = conn
        if state["embedder"] is None:
            state["embedder"] = EmbeddingClient(state["embed_endpoint"], dim=384)
        try:
            mcp_server = mcp_holder.get("mcp_server")
            if mcp_server is not None:
                # FastMCP's streamable-HTTP transport requires its session
                # manager to be running for the duration of the app.
                async with mcp_server.session_manager.run():
                    yield
            else:
                yield
        finally:
            conn.close()
            if hasattr(state["embedder"], "aclose"):
                await state["embedder"].aclose()

    app = FastAPI(lifespan=lifespan, title="memory-sidecar")
    app.state.mem = state

    import json
    import time as _time
    from starlette.middleware.base import BaseHTTPMiddleware

    class JsonLogMiddleware(BaseHTTPMiddleware):
        async def dispatch(self, request, call_next):
            start = _time.time()
            response = await call_next(request)
            latency_ms = int((_time.time() - start) * 1000)
            bot_id = request.query_params.get("bot_id") if request.query_params else None
            log = {
                "ts": round(_time.time(), 3),
                "route": request.url.path,
                "transport": "http",
                "method": request.method,
                "status": response.status_code,
                "latency_ms": latency_ms,
                "bot_id": bot_id,
            }
            print(json.dumps(log), flush=True)
            return response

    app.add_middleware(JsonLogMiddleware)

    @app.get("/health")
    async def health():
        return {"ok": True}

    app.include_router(routes_memory.build_router(state))
    app.include_router(routes_personality.build_router(state))
    app.include_router(routes_goals.build_router(state))

    # Build the dispatcher map: tool_name -> async fn that takes pydantic args
    # and returns the route's response. Routes must be mounted FIRST (above).
    import memory_sidecar.routes_memory as rm
    import memory_sidecar.routes_personality as rp
    import memory_sidecar.routes_goals as rg

    # Look up route endpoints from the app.
    endpoints: dict[str, Any] = {}
    for route in app.routes:
        if hasattr(route, "endpoint") and hasattr(route, "methods") and hasattr(route, "path"):
            for m in (route.methods or set()):
                endpoints[f"{m} {route.path}"] = route.endpoint

    def _route(key):
        return endpoints[key]

    async def _post_body(req_cls, key, args):
        return await _route(key)(req_cls(**args.model_dump()))

    async def _get_kw(key, args):
        return await _route(key)(**args.model_dump(exclude_none=True))

    dispatchers = {
        "memory.write":           lambda a: _post_body(rm.RememberRequest,      "POST /memory/remember", a),
        "memory.read":            lambda a: _get_kw("GET /memory/{memory_id}", a),
        "memory.update":          lambda a: _post_body(rm.UpdateRequest,         "PUT /memory/update", a),
        "memory.delete":          lambda a: _post_body(rm.ForgetRequest,         "POST /memory/forget", a),
        "memory.recall":          lambda a: _post_body(rm.RecallRequest,         "POST /memory/recall", a),
        "memory.recall_about":    lambda a: _post_body(rm.RecallAboutRequest,    "POST /memory/recall_about", a),
        "memory.search":          lambda a: _post_body(rm.SearchRequest,         "POST /memory/search", a),
        "memory.list":            lambda a: _get_kw("GET /memory/list", a),
        "memory.personality_get": lambda a: _post_body(rp.PersonalityGetRequest, "POST /memory/personality/get", a),
        "memory.personality_set": lambda a: _post_body(rp.PersonalitySetRequest, "POST /memory/personality/set", a),
        "goals.create":           lambda a: _post_body(rg.GoalCreateRequest,     "POST /goals/create", a),
        "goals.read":             lambda a: _get_kw("GET /goals/{goal_id}", a),
        "goals.update":           lambda a: _post_body(rg.GoalUpdateRequest,     "PUT /goals/update", a),
        "goals.list":             lambda a: _get_kw("GET /goals/list", a),
        "goals.complete":         lambda a: _post_body(rg.GoalCompleteRequest,   "POST /goals/complete", a),
    }

    token_path = os.environ.get("MEM_TOKEN_STORE", "/etc/memory/tokens.yaml")
    if Path(token_path).exists():
        token_store = TokenStore.load_yaml(token_path)
        mcp_listen = os.environ.get("MEM_LISTEN_ADDR", "0.0.0.0:8090")
        mcp_hosts = [
            "127.0.0.1:*", "localhost:*", "192.168.1.3:*",
            mcp_listen,
        ]
        resource_url = f"http://{mcp_listen}/mcp"
        mcp_server = build_mcp_server(
            token_store=token_store,
            dispatchers=dispatchers,
            allowed_hosts=mcp_hosts,
            resource_url=resource_url,
        )
        # Call streamable_http_app() once so session_manager is accessible
        # (FastMCP raises if accessed before this — kb_642162c3 §similar).
        _streamable_app = mcp_server.streamable_http_app()
        # Register with the lifespan holder so session_manager.run() wraps yield.
        mcp_holder["mcp_server"] = mcp_server
        from starlette.routing import Mount
        app.router.routes.append(Mount("/mcp", app=_streamable_app))
    else:
        print(f"[mcp] no token store at {token_path}; MCP wire disabled")

    return app
