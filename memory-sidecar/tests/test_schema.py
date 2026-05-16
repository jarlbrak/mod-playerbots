import sqlite3
import tempfile
from pathlib import Path

from memory_sidecar.db import open_db, run_migrations


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
