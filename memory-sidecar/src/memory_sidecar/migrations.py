"""Migration runner with schema_version tracking.

A migration is a single .sql file under the `migrations/` directory
named `NNNN_description.sql` where NNNN is a zero-padded integer.
Migrations run inside a single transaction each; if one fails the
DB is left at the prior version.
"""
import os
import sqlite3
import time
from pathlib import Path


SCHEMA_VERSION_DDL = """
CREATE TABLE IF NOT EXISTS schema_version (
    version     INTEGER PRIMARY KEY,
    applied_ts  INTEGER NOT NULL,
    description TEXT NOT NULL
)
"""


def _ensure_schema_version_table(conn: sqlite3.Connection) -> None:
    conn.execute(SCHEMA_VERSION_DDL)
    conn.commit()


def current_version(conn: sqlite3.Connection) -> int:
    """Return the highest applied schema version (0 if none applied)."""
    _ensure_schema_version_table(conn)
    cur = conn.execute("SELECT MAX(version) FROM schema_version")
    row = cur.fetchone()
    return row[0] if row and row[0] is not None else 0


def _parse_migration_filename(name: str) -> tuple[int, str] | None:
    """`0042_thing.sql` → (42, 'thing'). Returns None if name doesn't match."""
    if not name.endswith(".sql"):
        return None
    base = name[:-4]
    parts = base.split("_", 1)
    if len(parts) < 2:
        return None
    try:
        return int(parts[0]), parts[1]
    except ValueError:
        return None


def list_pending_migrations(
    migrations_dir: str, current: int
) -> list[tuple[int, str, str]]:
    """Return (version, description, path) tuples for migrations > current,
    sorted ascending by version."""
    out: list[tuple[int, str, str]] = []
    p = Path(migrations_dir)
    if not p.is_dir():
        return out
    for entry in sorted(p.iterdir()):
        parsed = _parse_migration_filename(entry.name)
        if parsed is None:
            continue
        version, desc = parsed
        if version > current:
            out.append((version, desc, str(entry)))
    out.sort(key=lambda t: t[0])
    return out


def apply_migrations(conn: sqlite3.Connection, migrations_dir: str) -> int:
    """Apply all pending migrations. Returns number applied."""
    _ensure_schema_version_table(conn)
    current = current_version(conn)
    pending = list_pending_migrations(migrations_dir, current)
    applied = 0
    for version, description, path in pending:
        with open(path, "r") as f:
            sql = f.read()
        # executescript implicitly commits open transactions. Each
        # migration is its own transaction.
        try:
            conn.executescript(sql)
            conn.execute(
                "INSERT INTO schema_version (version, applied_ts, description) "
                "VALUES (?, ?, ?)",
                (version, int(time.time()), description),
            )
            conn.commit()
            applied += 1
        except Exception:
            conn.rollback()
            raise
    return applied
