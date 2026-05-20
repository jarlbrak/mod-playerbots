import sqlite3
import tempfile
from pathlib import Path

import pytest

from memory_sidecar.db import open_db, run_migrations
from memory_sidecar.db import run_migrations as run_legacy
from memory_sidecar.migrations import apply_migrations


@pytest.fixture
def temp_db_with_v02_migrations(tmp_path):
    """Fresh DB with legacy schema + all v0.2 migrations applied."""
    db = tmp_path / "test.sqlite"
    conn = open_db(str(db))
    run_legacy(conn)
    migrations_dir = Path(__file__).resolve().parent.parent / "migrations"
    apply_migrations(conn, str(migrations_dir))
    yield conn
    conn.close()


def test_open_db_creates_file_and_loads_extension(tmp_path: Path):
    db_path = tmp_path / "mem.sqlite"
    conn = open_db(str(db_path))
    # sqlite-vec extension should be loaded; test by selecting vec_version
    cur = conn.execute("SELECT vec_version()")
    version = cur.fetchone()[0]
    assert version is not None
    assert db_path.exists()
    conn.close()


def test_run_migrations_creates_all_tables(tmp_path: Path):
    db_path = tmp_path / "mem.sqlite"
    conn = open_db(str(db_path))
    run_migrations(conn)
    tables = {
        row[0]
        for row in conn.execute(
            "SELECT name FROM sqlite_master WHERE type IN ('table','view')"
        )
    }
    assert "bots" in tables
    assert "entities" in tables
    assert "edges" in tables
    assert "memories" in tables
    assert "memory_entities" in tables
    assert "vec_memories" in tables
    conn.close()


def test_run_migrations_is_idempotent(tmp_path: Path):
    db_path = tmp_path / "mem.sqlite"
    conn = open_db(str(db_path))
    run_migrations(conn)
    run_migrations(conn)
    run_migrations(conn)
    conn.close()


def test_entities_uniqueness_constraint(tmp_path: Path):
    db_path = tmp_path / "mem.sqlite"
    conn = open_db(str(db_path))
    run_migrations(conn)
    conn.execute(
        "INSERT INTO entities (bot_id, name_lower, display_name, type) "
        "VALUES (?, ?, ?, ?)",
        ("b1", "tarren mill", "Tarren Mill", "zone"),
    )
    try:
        conn.execute(
            "INSERT INTO entities (bot_id, name_lower, display_name, type) "
            "VALUES (?, ?, ?, ?)",
            ("b1", "tarren mill", "Tarren Mill", "zone"),
        )
        assert False, "duplicate entity should have raised IntegrityError"
    except sqlite3.IntegrityError:
        pass
    conn.close()


def test_memories_index_present(tmp_path: Path):
    db_path = tmp_path / "mem.sqlite"
    conn = open_db(str(db_path))
    run_migrations(conn)
    indexes = {
        row[0]
        for row in conn.execute("SELECT name FROM sqlite_master WHERE type='index'")
    }
    assert "idx_memories_bot" in indexes
    assert "idx_entities_bot_name" in indexes
    conn.close()


def test_memories_has_memory_type_and_source(temp_db_with_v02_migrations):
    """After v0.2 migrations, memories table has memory_type + source columns."""
    cur = temp_db_with_v02_migrations.execute("PRAGMA table_info(memories)")
    cols = {row[1]: row[2] for row in cur.fetchall()}
    assert "memory_type" in cols
    assert cols["memory_type"] == "TEXT"
    assert "source" in cols
    assert cols["source"] == "TEXT"


def test_memory_type_default_is_event(temp_db_with_v02_migrations):
    """Existing rows backfill to 'event' via DEFAULT."""
    conn = temp_db_with_v02_migrations
    import time
    now = int(time.time())
    conn.execute("INSERT INTO bots (bot_id, created_ts) VALUES ('b1', ?)", (now,))
    conn.execute(
        "INSERT INTO memories (id, bot_id, text, salience, created_ts, last_recalled_ts) "
        "VALUES ('m1', 'b1', 'hello', 0.5, ?, ?)",
        (now, now),
    )
    cur = conn.execute("SELECT memory_type, source FROM memories WHERE id='m1'")
    row = cur.fetchone()
    assert row[0] == "event"
    assert row[1] is None


def test_entities_has_type_column(temp_db_with_v02_migrations):
    cur = temp_db_with_v02_migrations.execute("PRAGMA table_info(entities)")
    cols = {row[1]: row[2] for row in cur.fetchall()}
    assert "type" in cols
    assert cols["type"] == "TEXT"


def test_entity_type_can_be_set(temp_db_with_v02_migrations):
    conn = temp_db_with_v02_migrations
    conn.execute(
        "INSERT INTO entities (bot_id, name_lower, display_name, type) "
        "VALUES ('b1', 'alice', 'Alice', 'player')"
    )
    cur = conn.execute("SELECT type FROM entities WHERE display_name='Alice'")
    assert cur.fetchone()[0] == "player"
