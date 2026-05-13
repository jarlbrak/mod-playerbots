"""SQLite schema + data-access helpers for the memory sidecar."""
import sqlite3

import sqlite_vec


SCHEMA = [
    """
    CREATE TABLE IF NOT EXISTS bots (
        bot_id     TEXT PRIMARY KEY,
        persona    TEXT,
        created_ts INTEGER NOT NULL
    )
    """,
    """
    CREATE TABLE IF NOT EXISTS entities (
        id           INTEGER PRIMARY KEY,
        bot_id       TEXT NOT NULL,
        name_lower   TEXT NOT NULL,
        display_name TEXT NOT NULL,
        type         TEXT,
        UNIQUE(bot_id, name_lower)
    )
    """,
    """
    CREATE TABLE IF NOT EXISTS edges (
        src_entity_id INTEGER NOT NULL,
        rel           TEXT NOT NULL,
        dst_entity_id INTEGER NOT NULL,
        weight        REAL DEFAULT 1.0,
        last_seen_ts  INTEGER NOT NULL,
        PRIMARY KEY (src_entity_id, rel, dst_entity_id)
    )
    """,
    """
    CREATE TABLE IF NOT EXISTS memories (
        id                TEXT PRIMARY KEY,
        bot_id            TEXT NOT NULL,
        text              TEXT NOT NULL,
        salience          REAL NOT NULL,
        created_ts        INTEGER NOT NULL,
        last_recalled_ts  INTEGER NOT NULL,
        embedding         BLOB
    )
    """,
    """
    CREATE TABLE IF NOT EXISTS memory_entities (
        memory_id TEXT NOT NULL,
        entity_id INTEGER NOT NULL,
        PRIMARY KEY (memory_id, entity_id)
    )
    """,
    "CREATE INDEX IF NOT EXISTS idx_memories_bot ON memories(bot_id)",
    "CREATE INDEX IF NOT EXISTS idx_entities_bot_name ON entities(bot_id, name_lower)",
    """
    CREATE VIRTUAL TABLE IF NOT EXISTS vec_memories USING vec0(
        memory_id TEXT PRIMARY KEY,
        bot_id    TEXT,
        embedding FLOAT[384]
    )
    """,
]


def open_db(path: str) -> sqlite3.Connection:
    """Open a SQLite connection and load sqlite-vec into it."""
    conn = sqlite3.connect(path)
    conn.enable_load_extension(True)
    sqlite_vec.load(conn)
    conn.enable_load_extension(False)
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA synchronous=NORMAL")
    return conn


def run_migrations(conn: sqlite3.Connection) -> None:
    """Apply all schema statements. Idempotent (IF NOT EXISTS everywhere)."""
    for stmt in SCHEMA:
        conn.execute(stmt)
    conn.commit()
