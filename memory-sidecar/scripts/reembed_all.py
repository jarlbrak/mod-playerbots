"""One-shot re-embedding script for the Q4_K_M → Q8_0 migration.

Iterates all rows in memories ordered by (bot_id, id), embeds each
text via the live llama-embed service, and updates both
memories.embedding and vec_memories.embedding (the latter via
DELETE+INSERT since vec0 is a virtual table).

Designed to run inside the memory-sidecar container OR from the
host (just point at the right db path and endpoint).

Idempotent re-run: if interrupted, restart from the last bot_id+id
checkpoint (printed every 1000 rows). The script supports a
--resume-from <bot_id>:<id> flag.

Run:
    python3 scripts/reembed_all.py \\
        --db /var/memory/db.sqlite \\
        --embed-endpoint http://127.0.0.1:8081 \\
        --batch-commit-every 1000
"""
from __future__ import annotations

import argparse
import sqlite3
import sys
import time

import httpx
import numpy as np
import sqlite_vec


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--db", default="/var/memory/db.sqlite")
    p.add_argument("--embed-endpoint", default="http://127.0.0.1:8081")
    p.add_argument("--batch-commit-every", type=int, default=1000)
    p.add_argument("--resume-from", default=None,
                   help="bot_id:id checkpoint to resume from")
    p.add_argument("--dry-run", action="store_true")
    args = p.parse_args()

    print(f"[reembed] db={args.db}")
    print(f"[reembed] embed_endpoint={args.embed_endpoint}")
    print(f"[reembed] commit_every={args.batch_commit_every}")

    conn = sqlite3.connect(args.db)
    conn.enable_load_extension(True)
    sqlite_vec.load(conn)
    conn.enable_load_extension(False)
    conn.execute("PRAGMA journal_mode=WAL")

    total = conn.execute("SELECT COUNT(*) FROM memories").fetchone()[0]
    print(f"[reembed] total memories: {total}")

    where = ""
    params = ()
    if args.resume_from:
        rb, rid = args.resume_from.split(":", 1)
        where = "WHERE (bot_id, id) > (?, ?)"
        params = (rb, rid)
        print(f"[reembed] resuming from {rb!r} / {rid!r}")

    cur = conn.execute(
        f"SELECT id, bot_id, text FROM memories {where} ORDER BY bot_id, id"
    )
    rows = cur.fetchall()
    print(f"[reembed] rows to process: {len(rows)}")

    if args.dry_run:
        print(f"[reembed] DRY RUN — exiting without writes")
        return 0

    client = httpx.Client(timeout=15.0)
    started = time.time()
    n_done = 0
    n_err = 0
    last_checkpoint = ("", "")

    try:
        for memory_id, bot_id, text in rows:
            try:
                resp = client.post(
                    f"{args.embed_endpoint}/v1/embeddings",
                    json={"input": text, "model": "embedding"},
                )
                resp.raise_for_status()
                vec = np.array(resp.json()["data"][0]["embedding"], dtype=np.float32)
                if len(vec) != 384:
                    raise ValueError(f"unexpected dim {len(vec)}")
                blob = vec.tobytes()
                conn.execute(
                    "UPDATE memories SET embedding=? WHERE id=?",
                    (blob, memory_id),
                )
                conn.execute("DELETE FROM vec_memories WHERE memory_id=?", (memory_id,))
                conn.execute(
                    "INSERT INTO vec_memories (memory_id, bot_id, embedding) "
                    "VALUES (?, ?, ?)",
                    (memory_id, bot_id, blob),
                )
                n_done += 1
                last_checkpoint = (bot_id, memory_id)
            except Exception as e:
                n_err += 1
                if n_err > 100:
                    print(f"[reembed] too many errors ({n_err}); aborting")
                    raise
                print(f"[reembed] error on {memory_id}: {e}", file=sys.stderr)

            if n_done % args.batch_commit_every == 0:
                conn.commit()
                elapsed = time.time() - started
                rate = n_done / max(elapsed, 0.001)
                eta = (len(rows) - n_done) / max(rate, 0.001)
                print(f"[reembed] {n_done}/{len(rows)} done, "
                      f"{rate:.1f}/s, eta {eta/60:.1f}min, "
                      f"errors {n_err}, last={last_checkpoint}")

        conn.commit()
        elapsed = time.time() - started
        print(f"[reembed] DONE: {n_done} rows in {elapsed/60:.1f}min, "
              f"{n_err} errors. last_checkpoint={last_checkpoint}")
        return 0
    finally:
        client.close()
        conn.close()


if __name__ == "__main__":
    sys.exit(main())
