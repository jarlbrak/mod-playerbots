# memory-sidecar

Per-bot memory service for the mod-playerbots LLM agent. FastAPI +
SQLite + sqlite-vec. Embeddings via HTTP to a llama.cpp `--embedding`
server.

Run locally:

    python -m venv .venv && source .venv/bin/activate
    pip install -e '.[dev]'
    MEM_DB_PATH=/tmp/mem.sqlite MEM_EMBED_ENDPOINT=http://127.0.0.1:8081 \
        uvicorn memory_sidecar.main:app --port 8090

Run tests:

    pytest

Endpoints: /memory/recall_about, /memory/remember, /memory/recall,
/memory/forget, /memory/personality/get, /memory/personality/set,
/health.
