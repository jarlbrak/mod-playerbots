"""Tests for the schema_version migration runner."""
import sqlite3
from pathlib import Path

import pytest

from memory_sidecar.db import open_db, run_migrations as run_legacy_migrations
from memory_sidecar.migrations import current_version, apply_migrations


@pytest.fixture
def temp_db(tmp_path):
    db = tmp_path / "test.sqlite"
    conn = open_db(str(db))
    run_legacy_migrations(conn)
    yield conn
    conn.close()


def test_current_version_is_zero_on_fresh_legacy_db(temp_db):
    assert current_version(temp_db) == 0


def test_apply_migrations_advances_version(temp_db, tmp_path):
    # Create one trivial migration file.
    mdir = tmp_path / "migrations"
    mdir.mkdir()
    (mdir / "0001_test.sql").write_text(
        "CREATE TABLE IF NOT EXISTS dummy (id INTEGER);"
    )
    n = apply_migrations(temp_db, str(mdir))
    assert n == 1
    assert current_version(temp_db) == 1
    # Verify the migration actually applied.
    temp_db.execute("INSERT INTO dummy VALUES (42)")
    cur = temp_db.execute("SELECT id FROM dummy")
    assert cur.fetchone()[0] == 42


def test_apply_migrations_is_idempotent(temp_db, tmp_path):
    mdir = tmp_path / "migrations"
    mdir.mkdir()
    (mdir / "0001_test.sql").write_text("CREATE TABLE IF NOT EXISTS d1 (id INTEGER);")
    n1 = apply_migrations(temp_db, str(mdir))
    n2 = apply_migrations(temp_db, str(mdir))
    assert n1 == 1
    assert n2 == 0
    assert current_version(temp_db) == 1


def test_apply_migrations_applies_multiple_in_order(temp_db, tmp_path):
    mdir = tmp_path / "migrations"
    mdir.mkdir()
    (mdir / "0001_first.sql").write_text("CREATE TABLE a (id INTEGER);")
    (mdir / "0002_second.sql").write_text("CREATE TABLE b (id INTEGER);")
    n = apply_migrations(temp_db, str(mdir))
    assert n == 2
    assert current_version(temp_db) == 2
    temp_db.execute("SELECT * FROM a")
    temp_db.execute("SELECT * FROM b")


def test_apply_migrations_partial_resume(temp_db, tmp_path):
    """If only migration 1 is applied, then migration 2 is added, only 2 runs."""
    mdir = tmp_path / "migrations"
    mdir.mkdir()
    (mdir / "0001_first.sql").write_text("CREATE TABLE a (id INTEGER);")
    apply_migrations(temp_db, str(mdir))
    assert current_version(temp_db) == 1
    (mdir / "0002_second.sql").write_text("CREATE TABLE b (id INTEGER);")
    n = apply_migrations(temp_db, str(mdir))
    assert n == 1
    assert current_version(temp_db) == 2
