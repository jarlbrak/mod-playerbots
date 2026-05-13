# LLM-Agent Phase 3 — Memory Sidecar + T0 Memory Hints Design

**Status:** Approved (2026-05-13)
**Parent design:** [`docs/llm_agent_design.md`](../../llm_agent_design.md) §8 + §12
**Predecessors:** Phase 0.5 Vulkan
([spec](2026-05-12-llm-agent-phase-0.5-hardware-validation-design.md));
Phase 0.5 ROCm ([spec](2026-05-12-llm-agent-phase-0.5-rocm-comparison-design.md));
Phase 1 plumbing
([spec](2026-05-12-llm-agent-phase-1-plumbing-spike-design.md),
[results](../../../results/2026-05-12-llm-phase-1-smoke/summary.md));
Phase 2 validator + apply
([spec](2026-05-13-llm-agent-phase-2-design.md),
[results](../../../results/2026-05-13-llm-phase-2-smoke/summary.md)).

## 1. Goal

Give bots a memory that grows from in-game events, expose it as a small
HTTP API, and feed relevant fragments into T0's digest before each LLM
call. The Phase 2 smoke test surfaced "100 % of LLM-proposed goals were
`idle`" because the digest is too sparse for the LLM to react to. This
phase is the unblock.

## 2. Phase 3 invariant

A sidecar outage degrades the LLM's decision quality but **never**
affects gameplay timing, state correctness, or stability. With
`AiPlayerbot.MemorySidecar.Endpoint` pointing at a non-existent host:
- Every `recall_about` returns `{}` after the configured timeout.
- Every `remember` is silently dropped.
- T0 digest is built with empty `memory_hints`.
- Tick latency matches Phase 2 baseline.
- Bot gameplay (via the rule-based fallback) is unchanged.

Disabling the entire LlmAgent (`AiPlayerbot.LlmAgent.Enabled = 0`)
returns the worldserver to byte-identical baseline, same as Phase 2.

## 3. Scope decisions

Settled during brainstorming (2026-05-13):

| Decision | Choice | Rationale |
|---|---|---|
| Scope | **Full Phase 3** — sidecar + C++ MemoryHttpClient + T0 recall integration + shadow writer + stub personality cards | Phase 2's "100% idle" finding makes the integration load-bearing; sidecar-only would defer the visible impact. |
| Embedding source | **Second llama.cpp Quadlet** (`llama-embed.service`) loading `bge-small-en-v1.5.Q4_K_M.gguf` with `--embedding` | Matches our existing Quadlet pattern. Shares GPU with the chat llama-server (plenty of VRAM headroom). |
| Sidecar transport | **Python FastAPI + HTTP** per parent design §8.1 | Keeps C++ out of the embedding / vector / graph ecosystem. |
| Storage | **SQLite + `sqlite-vec`** per parent design §8.2 | Single-file DB; `vec_distance_l2` for cosine-ish similarity; recursive CTE for graph traversal. |
| Recall scoring | `w_rel=0.5 / w_rec=0.2 / w_imp=0.3` per parent design §8.3 | Defaults from Park et al. 2023. |
| Writer | **Shadow writer** on existing Phase 2 hooks (kill + whisper) | T2 tool calls (LLM-initiated writes) are Phase 4; Phase 3 uses ground-truth events. |
| Personality cards | **Stub in Phase 3**, LLM-generated in Phase 4 | One-line stub: `"<race> <class>, level <level>, currently in <zone>"`. Lazy persist on first access. |
| Memory cap | **2 000 / bot**, evict lowest `salience × exp(-Δt/τ)` | Parent design §8.4. |

## 4. Architecture overview

Two new services on Heimdal alongside `llama-server.service`:

```
                 (existing)              (new in Phase 3)
                 ┌──────────────┐        ┌──────────────────┐
                 │ llama-server │        │  llama-embed     │
                 │  :8080       │        │  :8081           │
                 │  Qwen 2.5 7B │        │  bge-small-en    │
                 │  (chat)      │        │  (embeddings)    │
                 └──────────────┘        └──────────────────┘
                       ▲                          ▲
                       │ /v1/chat/completions     │ /v1/embeddings
                       │                          │
       ┌───────────────┴──────────────────────┐   │
       │           worldserver                │   │
       │   ┌──────────────────────────────┐   │   │ POST /memory/*
       │   │ LlmAgent                     │   │   ▼
       │   │  ┌──────────────────────┐    │   │  ┌────────────────────┐
       │   │  │ MemoryHttpClient     │────┼───┼─▶│  memory-sidecar    │
       │   │  └──────────────────────┘    │   │  │   :8090            │
       │   │  ┌──────────────────────┐    │   │  │   FastAPI          │
       │   │  │ Tier0_StateDigest    │    │   │  │     │              │
       │   │  │  recall_about → hints│    │   │  │     ▼              │
       │   │  └──────────────────────┘    │   │  │  /memory/recall_about
       │   │  ┌──────────────────────┐    │   │  │  /memory/remember  │
       │   │  │ LlmAgentHooks        │    │   │  │  /memory/recall    │
       │   │  │ whisper / kill ──────┼────┘   │  │  /memory/forget    │
       │   │  │ also POST .remember  │        │  │  /memory/personality
       │   │  └──────────────────────┘        │  │     │              │
       │   └──────────────────────────────────┘  │     ▼              │
       └──────────────────────────────────────────┴ SQLite+sqlite-vec ┘
                                                  /opt/containers/memory/db.sqlite
```

**Critical invariants:**

- memory-sidecar is **independent of the chat llama-server**. It calls
  llama-embed only.
- llama-embed only does embeddings. C++ never talks to it directly.
- All sidecar state lives in SQLite; the Python process is stateless
  across requests.
- C++ stays out of the Python ecosystem. One C++ dependency:
  cpp-httplib (already vendored).
- **Memory ops are best-effort.** A sidecar outage causes recalls to
  return empty and writes to be dropped. Gameplay never blocks.

Three Quadlets on Heimdal: `llama-server.service` (existing),
`llama-embed.service` (new), `memory-sidecar.service` (new).

## 5. Components

### 5.1 `memory-sidecar/main.py`

Single FastAPI app, ~400 lines. Six routes mapping 1-to-1 onto §8.1's
endpoint list:

- `POST /memory/recall_about`
- `POST /memory/remember`
- `POST /memory/recall`
- `POST /memory/forget`
- `POST /memory/personality/get`
- `POST /memory/personality/set`

Plus a `GET /health` for the Quadlet's healthcheck.

Startup: open SQLite at `${MEM_DB_PATH}`, load `sqlite-vec` extension,
run migrations (CREATE TABLE IF NOT EXISTS), instantiate an
`EmbeddingClient`, warm its cache with `embed("warmup")`.

Every request is a SQLite transaction. No persistent Python state
across requests.

### 5.2 `memory-sidecar/db.py`

Pure data-access. SQL helpers for `bots`, `entities`, `edges`,
`memories`, `memory_entities`, `vec_memories` (sqlite-vec virtual
table). Exposes typed Python dataclasses (`Memory`, `Entity`, `Edge`).

Per-bot cap enforcement: on every successful `remember`, count the
bot's memories. If over cap, DELETE the lowest
`salience * exp(-Δt/τ)` entries until under cap. τ defaults to
7 days (configurable).

### 5.3 `memory-sidecar/recall.py`

Pure functions for the §8.3 scoring formula:

```python
def score(memory: Memory, query_emb: np.ndarray, now: int) -> float:
    relevance = cosine(memory.embedding, query_emb)
    recency   = math.exp(-(now - memory.last_recalled_ts) / TAU_SECONDS)
    importance = memory.salience  # already in [0,1]
    return W_REL * relevance + W_REC * recency + W_IMP * importance
```

Weights from config (default `0.5 / 0.2 / 0.3`). Unit-testable with
hand-built `Memory` instances + stub embeddings.

`recall` returns the top-K candidates ranked by `score`.
`recall_about` first walks the graph (up to `max_hops`), gathers
attached memories, then ranks within that set.

On successful recall, the candidate's `last_recalled_ts` is updated
(recall is a refresh per parent design §8.3).

### 5.4 `memory-sidecar/embed.py`

`EmbeddingClient` wraps the llama-embed HTTP call:

```python
class EmbeddingClient:
    def __init__(self, endpoint: str, dim: int = 384, cache_size: int = 1024): ...
    async def embed(self, text: str) -> np.ndarray: ...   # float32, dim-sized
```

LRU cache for the most-recent 1024 queries — repeated `recall` calls
for the same zone / NPC are cheap.

### 5.5 `memory-sidecar/tests/`

pytest, run in <5 s. Embedding mocked via deterministic hash-to-vector.
~40 cases across:

- `test_schema.py` — migrations idempotent, indices exist, sqlite-vec
  extension loads.
- `test_recall_scoring.py` — formula math, recency decay shape,
  monotonicity.
- `test_endpoints.py` — happy + error paths for all 6 routes.
- `test_eviction.py` — cap enforcement, concurrent insert+evict.
- `test_recall_about.py` — 2-hop traversal, unknown entity, empty
  graph, ranking within traversed set.

### 5.6 `infra/heimdal/llama-embed.container`

New Quadlet. Same `ghcr.io/ggml-org/llama.cpp:server-vulkan` image as
the chat server. Env file points at `bge-small-en-v1.5.Q4_K_M.gguf`
(downloaded to `/opt/containers/llama-embed/models/` during deploy).
`PublishPort=0.0.0.0:8081:8081`. `--embedding` flag in the command
line.

### 5.7 `infra/heimdal/memory-sidecar.container`

New Quadlet. Build a small image from `python:3.12-slim` +
`pip install fastapi uvicorn sqlite-vec httpx numpy`. Bind-mount
`/opt/containers/memory` for the DB and `sidecar.env`.
`PublishPort=0.0.0.0:8090:8090`. After `llama-embed.service`.

Container env:
```
MEM_DB_PATH=/var/memory/db.sqlite
MEM_EMBED_ENDPOINT=http://192.168.1.3:8081
MEM_W_REL=0.5
MEM_W_REC=0.2
MEM_W_IMP=0.3
MEM_TAU_SECONDS=604800
MEM_CAP_PER_BOT=2000
```

### 5.8 C++ — `src/Bot/LlmAgent/Client/MemoryHttpClient.{h,cpp}` (new)

Mirror of `LlmHttpClient`. Methods:

```cpp
class MemoryHttpClient {
  public:
    std::vector<std::string> RecallAbout(uint64_t bot_guid, const std::string& entity,
                                         uint32_t max_hops = 2, uint32_t top_k = 3);
    bool Remember(uint64_t bot_guid, const std::string& text,
                  const std::vector<std::string>& entities,
                  double salience,
                  const std::vector<std::tuple<std::string, std::string, std::string>>& relations = {});
    std::optional<std::string> GetPersonality(uint64_t bot_guid);
    bool SetPersonality(uint64_t bot_guid, const std::string& persona);
};
```

Returns empty / false on transport error. 30-second sticky failure
cache: after one failure, all subsequent calls within 30 s return
immediately without HTTP. WARN throttled to 1 per 60 s.

### 5.9 C++ — `src/Bot/LlmAgent/Memory/PersonalityCard.{h,cpp}` (new)

`StubPersonaText(LlmBotState)` returns
`"<race> <class>, level <level>, currently in <zone>."`
Used until Phase 4 wires the LLM-driven generator. The sidecar caches
whatever the C++ side POSTs.

### 5.10 C++ — `Tier0_StateDigest::SnapshotBot` (modified)

After building the existing self/location/goal/quest_log/inventory
blocks, issue up to three `recall_about` calls on the worldserver
thread:

1. entity = bot's current zone name
2. entity = each `nearby_humans[i].name` (top 3)
3. entity = current quest title (only if `RPG_DO_QUEST`)

Concatenate returned hints into the `memory_hints` array. Stop early
once cumulative size exceeds `cfg.HintMaxChars` (1200 default).

Also issue `GetPersonality(bot_guid)`. If `nullopt`, build the stub
via `StubPersonaText` and POST it via `SetPersonality` (best-effort).
The persona becomes the suffix of the T1 system prompt — not part of
the digest's `self` block.

### 5.11 C++ — `Hooks/LlmAgentHooks.cpp` (modified)

`OnWhisperReceived` and `OnKill` get one additional call each after
their existing `mgr.Events().Push(...)`:

```cpp
if (cfg.EnableWrites) {
    mgr.MemoryClient().Remember(
        bot_guid,
        /* text */ "...",
        /* entities */ {...},
        /* salience */ salience_for_event(...),
        /* relations */ {...}
    );
}
```

`salience_for_event`:
- killed mob → 0.1
- killed elite → 0.3
- killed named/boss → 0.5
- whisper from real player → 0.7

### 5.12 C++ — `LlmAgentManager` (modified)

Owns a `MemoryHttpClient` instance. Accessor `MemoryClient()`. Started
in `Start()` with `cfg.MemorySidecar.Endpoint` and
`cfg.MemorySidecar.RequestTimeoutMs`.

### 5.13 C++ — `LlmAgentConfig` (modified)

Five new keys (see §7).

## 6. Sidecar API + data shapes

All endpoints: POST, JSON in/out.

### 6.1 `/memory/recall_about`

```json
// request
{"bot_id": "12345", "entity": "Tarren Mill", "max_hops": 2, "top_k": 3}
// response 200
{"hints": ["You've turned in 3 quests in Tarren Mill this week", ...]}
// 404 (unknown entity for this bot) → {"hints": []}  (easier on C++)
```

### 6.2 `/memory/remember`

```json
// request
{
  "bot_id": "12345",
  "text": "killed Syndicate Footpad in Tarren Mill",
  "entities": ["Syndicate Footpad", "Tarren Mill"],
  "salience": 0.1,
  "relations": [
    {"src": "Syndicate Footpad", "rel": "located_in", "dst": "Tarren Mill"}
  ]
}
// response 200
{"memory_id": "m_a1b2c3", "evicted": 0}
// 503 if sidecar's internal write buffer is full → C++ skips
```

### 6.3 `/memory/recall`

```json
// request
{"bot_id": "12345", "query": "where can I make money?", "top_k": 5}
// response 200
{"memories": [{"text": "...", "score": 0.83, "ts": 1747095123}, ...]}
```

### 6.4 `/memory/forget`

```json
// request
{"bot_id": "12345", "memory_id": "m_a1b2c3"}
// response 200
{"forgotten": true}
```

### 6.5 `/memory/personality/get` / `set`

```json
// get request
{"bot_id": "12345"}
// 200 → {"persona": "..."}; 404 if no card yet
// set request
{"bot_id": "12345", "persona": "..."}    // max 4000 chars; 400 if larger
// 200 → {"ok": true}
```

### 6.6 SQLite schema

```sql
CREATE TABLE bots (
  bot_id TEXT PRIMARY KEY,
  persona TEXT,
  created_ts INTEGER NOT NULL
);
CREATE TABLE entities (
  id INTEGER PRIMARY KEY,
  bot_id TEXT NOT NULL,
  name_lower TEXT NOT NULL,
  display_name TEXT NOT NULL,
  type TEXT,
  UNIQUE(bot_id, name_lower)
);
CREATE TABLE edges (
  src_entity_id INTEGER NOT NULL,
  rel TEXT NOT NULL,
  dst_entity_id INTEGER NOT NULL,
  weight REAL DEFAULT 1.0,
  last_seen_ts INTEGER NOT NULL,
  PRIMARY KEY (src_entity_id, rel, dst_entity_id)
);
CREATE TABLE memories (
  id TEXT PRIMARY KEY,
  bot_id TEXT NOT NULL,
  text TEXT NOT NULL,
  salience REAL NOT NULL,
  created_ts INTEGER NOT NULL,
  last_recalled_ts INTEGER NOT NULL,
  embedding BLOB
);
CREATE TABLE memory_entities (
  memory_id TEXT NOT NULL,
  entity_id INTEGER NOT NULL,
  PRIMARY KEY (memory_id, entity_id)
);
CREATE INDEX idx_memories_bot ON memories(bot_id);
CREATE INDEX idx_entities_bot_name ON entities(bot_id, name_lower);
CREATE VIRTUAL TABLE vec_memories USING vec0(
  memory_id TEXT PRIMARY KEY,
  bot_id TEXT,
  embedding FLOAT[384]
);
```

Bot IDs are TEXT (more uniform across SQLite driver bindings). Memory
IDs are externally-visible `"m_" + base32(uuid7 prefix)` strings.

## 7. Configuration

Five new keys in `conf/playerbots.conf.dist` (total Phase 1 + 2 + 3 = 18):

| Key | Default | Description |
|---|---|---|
| `AiPlayerbot.MemorySidecar.Endpoint` | `"http://127.0.0.1:8090"` | Sidecar base URL. |
| `AiPlayerbot.MemorySidecar.RequestTimeoutMs` | `2000` | Tight budget — keeps T0 build snappy. |
| `AiPlayerbot.MemorySidecar.EnableWrites` | `1` | When `0`, recall still happens but kill/whisper events skip `remember`. |
| `AiPlayerbot.MemorySidecar.RecallTopK` | `3` | Hints per `recall_about` call. |
| `AiPlayerbot.MemorySidecar.HintMaxChars` | `1200` | Total budget across all hints in one digest. |

Sidecar's own config (DB path, embed endpoint, scoring weights, cap)
lives in `/opt/containers/memory/sidecar.env`.

## 8. Data flow

### 8.1 Read path (T0 builds `memory_hints` before enqueue)

1. `LlmReplanIdleAction::Execute` is eligible to enqueue.
2. `SnapshotBot` constructs hint queries: zone, up to 3 nearby_humans,
   current quest title (if `DoQuest`).
3. For each query, serial `RecallAbout` on the worldserver thread.
   Typical 1-2 ms cached, 5-15 ms cold. Stop adding hints once
   cumulative size > `HintMaxChars`.
4. Personality: `GetPersonality(bot)`. If `nullopt`, build stub and
   `SetPersonality` best-effort.
5. Digest built. Hints in `memory_hints`. Persona suffixes the system
   prompt. Enqueue T1 request as before.

### 8.2 Write path (kill / whisper events)

1. `OnPlayerbotCheckKillTask` (existing) fires → `OnKill(bot, victim)`.
2. Hook does its existing event_log push.
3. If `EnableWrites`, hook calls `Remember(...)` with structured
   payload. **This runs on the worldserver thread initially.** The
   `MemoryHttpClient::Remember` implementation:
   - Queues the request to the existing LlmAgent worker pool.
   - Returns immediately (fire-and-forget from the worldserver tick).
4. Worker thread POSTs to `/memory/remember`. Sidecar:
   - Embeds the text (cache miss → ~30-100 ms on llama-embed).
   - INSERTS into `memories`, `vec_memories`, `memory_entities`,
     `entities` (upsert), `edges`.
   - Enforces cap → evicts the lowest `salience * exp(-Δt/τ)`.
   - Returns 200 with the memory_id (which C++ discards).

The worldserver tick never blocks on a memory write.

## 9. Error handling

| Failure | Detection | Action |
|---|---|---|
| Sidecar unreachable | cpp-httplib transport error | Recall returns `[]`. Remember dropped. Persona returns `nullopt`. WARN 1/60 s. **No counter increment** for these (operational, not behavioral). 30-s sticky failure cache (no retry attempts during the window). |
| Sidecar 5xx | HTTP 500-503 | Same as transport. Body truncated to 1 KB and logged. |
| Sidecar 4xx | HTTP 4xx | 404 expected (unknown entity / no persona) — silent empty result. 400 means malformed — one ERROR per occurrence. |
| Sidecar slow / timeout | `RequestTimeoutMs` hit | Same as transport. Tick stays snappy. |
| Embedding dimension mismatch | sqlite-vec INSERT fails | Sidecar returns 500. C++ treats as unreachable. Sidecar logs expected vs. actual dim. |
| Cap eviction race | Two concurrent `remember` for same bot exceeding cap | SQLite serializes via exclusive transaction. Brief blocking, no data loss. |
| Sidecar internal write buffer full | >1000 queued `remember` POSTs | Sidecar returns 503 → C++ skips. Burst absorption without slowing C++. |

## 10. Testing strategy

### 10.1 Layer 0 — Python sidecar unit tests (NEW)

~40 cases via pytest. Embedding mocked. Run in <5 s.

- `test_schema.py` — migrations, indices, sqlite-vec load.
- `test_recall_scoring.py` — formula math, recency decay, monotonicity.
- `test_endpoints.py` — happy + error paths for all 6 routes.
- `test_eviction.py` — cap enforcement, concurrent inserts.
- `test_recall_about.py` — graph traversal, unknown entity, empty graph.

### 10.2 Layer 1 — C++ unit tests (extend existing)

- `test_memory_client.cpp` (~5 cases): mirror of test_http_client.cpp.
  In-process stub server. Verify happy path, transport error → `{}`,
  30 s sticky failure cache, timeout, 200 hints unchanged.
- Extend `test_digest_shape.cpp` with one `memory_hints` round-trip
  case.

Total C++ unit tests: 90 (Phase 2's 85 + 5).

### 10.3 Layer 2 — Sidecar integration smoke (Python-only)

`memory-sidecar/scripts/integration_smoke.py`:
1. Assume llama-embed running at `127.0.0.1:8081`.
2. Spin up sidecar.
3. POST 100 synthetic memories.
4. Verify `recall_about("Westfall")` returns Westfall-relevant hints.
5. Verify `recall("where can I make money?")` returns plausibly
   relevant top result.

Tagged `pytest -m integration`; not run by default.

### 10.4 Layer 3 — Heimdal smoke (3-run sequence)

1. **Setup.** Deploy `llama-embed.service` + `memory-sidecar.service`.
   Verify `curl 127.0.0.1:8081/v1/embeddings` and
   `curl 127.0.0.1:8090/health` return 200.
2. **Run 1 — Cold memory, ApplyMode=log, SamplePct=10, 5 min window.**
   Recall returns `[]` (no memories yet). Writes start populating
   SQLite. Inspect for ~50-100 memories. JSONL records show
   `memory_hints: []`.
3. **Run 2 — Warm memory, ApplyMode=log, SamplePct=10, 5 min window.**
   JSONL records show `memory_hints` populated. Spot-check 5 records:
   hints reference real entities the bot has encountered.
4. **Run 3 — ApplyMode=apply, SamplePct=10, 15 min window.** Observe
   one sampled bot in-game. Goal distribution should diversify past
   100 % idle — target ≥20 % non-idle goals. Tick latency stays within
   Phase 2 baseline (mean ≤ 20 ms, p95 ≤ 50 ms).

**Phase 3 done** = Layers 0 + 1 green + Layer 3 Run 3 produces at
least one non-idle goal that the apply path actually transitions a
bot through.

### 10.5 Explicitly not tested in Phase 3

- 24-hour memory growth (eviction at scale).
- Cross-bot memory leakage (we trust the SQL `WHERE bot_id = ?`).
- Phase 4 T2 memory-tool calls.

## 11. Out of scope

Stays outside Phase 3:

- **LLM-driven personality cards.** Phase 3 stubs them; Phase 4 wires
  the LLM-generated version.
- **Memory tool calls by the LLM.** That's the T2 tool catalog,
  Phase 4.
- **Nightly memory-compaction job.** Parent design §8.4: "optional in
  v0." Skipped.
- **Kuzu graph DB.** Two-hop SQL CTE is enough; parent design §8.2
  defers Kuzu.
- **Per-realm / cross-bot memory sharing.** All memories partitioned
  by `bot_id`.
- **Memory inspection admin command.** Read SQLite directly.
- **TLS / auth on the sidecar.** LAN-only for now; Phase 6+ if ever
  exposed externally.
- **Embedding model auto-update.** Quadlet pins a specific GGUF.
- **Quest event writers** (accept / complete / turn-in). Phase 4
  follow-up when the apply path produces quest progression.

## 12. Success criteria summary

1. Python tests: ~40/40 green via pytest in <5 s.
2. C++ tests: 90/90 green (Phase 2's 85 + 5 new).
3. `MemorySidecar.Endpoint=<unreachable>`: zero-degraded behavior
   (tick stable, `memory_hints` empty, gameplay unaffected).
4. `MemorySidecar.Endpoint=<live>` + 10 min ApplyMode=log:
   `memory_hints` populates with real entities; ~50-100 memories in
   SQLite.
5. `MemorySidecar.Endpoint=<live>` + 15 min ApplyMode=apply: at least
   one sampled bot directly observed transitioning to a non-idle goal
   via LLM dispatch; tick latency mean ≤ 20 ms, p95 ≤ 50 ms.

When all five hold, Phase 3 is done. Phase 4 (T2 interactive
tool-calling) is the natural next phase per parent design §12.

## 13. Phase 2 follow-ups closed by Phase 3

- ✅ Digest sparsity (Phase 2 finding: 100 % idle proposals).
- ✅ Memory subsystem (the §12 deliverable).
- ✅ T0 memory hints (the §6 design pattern).
- ⚠️ Personality cards — stubbed in Phase 3; LLM-driven in Phase 4.

## 14. Open items deferred to Phase 4

- LLM-generated personality cards.
- T2 tool catalog including `memory.remember` / `memory.recall` /
  `memory.recall_about` for the LLM to invoke directly.
- Quest accept / complete / turn-in writers.
- Nightly memory-compaction job (if memory growth makes it useful).
