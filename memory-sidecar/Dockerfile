FROM python:3.12-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY pyproject.toml requirements.txt ./
RUN pip install --no-cache-dir -r requirements.txt

COPY src/ ./src/

ENV PYTHONPATH=/app/src
ENV MEM_DB_PATH=/var/memory/db.sqlite
ENV MEM_EMBED_ENDPOINT=http://127.0.0.1:8081

EXPOSE 8090

HEALTHCHECK --interval=30s --timeout=3s --start-period=10s --retries=3 \
    CMD python -c "import urllib.request as r; r.urlopen('http://127.0.0.1:8090/health').read()" || exit 1

CMD ["uvicorn", "memory_sidecar.main:create_app", "--factory", \
     "--host", "0.0.0.0", "--port", "8090"]
