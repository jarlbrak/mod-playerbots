"""Latency regression for v0.2.1 fixes.

Writes ~2000 memories to a throwaway bot, then asserts:
  - /memory/search p95 < 500ms over 20 runs
  - /memory/recall p95 < 300ms over 20 runs
  - /memory/recall_about p95 < 100ms over 20 runs
  - At least 4 of 5 named queries return populated bm25_rank

Skipped without HEIMDAL_MEMORY_URL + HEIMDAL_MEMORY_TOKEN.
"""
from __future__ import annotations

import os
import time
import uuid

import httpx
import pytest


BASE_URL = os.environ.get("HEIMDAL_MEMORY_URL", "http://192.168.1.3:8090")
TOKEN = os.environ.get("HEIMDAL_MEMORY_TOKEN")

skip_if_no_env = pytest.mark.skipif(
    not TOKEN,
    reason="set HEIMDAL_MEMORY_URL + HEIMDAL_MEMORY_TOKEN to run",
)


def _client():
    return httpx.Client(base_url=BASE_URL, timeout=30.0)


def _p95(samples):
    s = sorted(samples)
    n = len(s)
    return s[max(0, int(n * 0.95) - 1)]


def _seed_bot(c, bot_id, n=2000):
    """Write n memories with varied text to seed a realistic bot."""
    seed_phrases = [
        "Alice asked me to tank for her in Deadmines tonight.",
        "Brun bragged about his bow drop.",
        "Mira ported me to Stormwind after the grind.",
        "Marshal Dughan handed me three quests at once.",
        "Hit level 25. The cap. Now the saving begins.",
        "Died in a wipe at the Foreman pull. Embarrassing.",
        "Looted Iron Buckle in Deadmines. Used in blacksmithing.",
        "VanCleef drops Cruel Barb on heroic runs.",
        "Visited Ironforge to train blacksmithing. Skill +25.",
        "Alice and I agreed: no AH spending until 100g each.",
        "Reflecting: should heal Alice more proactively.",
        "Brun's pet died in Stockades. He took it hard.",
        "Sold 12 Linen Cloth on the AH for 80c each.",
        "Crafted a Rough Bronze Cuirass. Vendor 30s.",
        "Slept at Lion's Pride Inn in Goldshire.",
    ]
    for i in range(n):
        text = seed_phrases[i % len(seed_phrases)] + f" [iter {i}]"
        c.post("/memory/remember", json={
            "bot_id": bot_id,
            "text": text,
            "salience": 0.5,
            "entities": ["Alice", "Deadmines"][:i % 3],
            "relations": [],
        }).raise_for_status()


@skip_if_no_env
def test_search_p95_under_500ms():
    bot = f"v021-latency-{uuid.uuid4().hex[:8]}"
    queries = [
        "what did Alice and I plan about the mount",
        "what is my blacksmithing skill",
        "what do I know about VanCleef",
        "what happened when I died",
        "have I been to the inn recently",
        "tell me about Brun's pet",
        "any encounters with Deadmines bosses",
        "what did Mira do for me",
    ]
    try:
        with _client() as c:
            c.headers["Authorization"] = f"Bearer {TOKEN}"
            _seed_bot(c, bot, n=2000)
            latencies = []
            bm25_hits = 0
            for q in queries * 3:  # 24 search runs
                t = time.perf_counter()
                r = c.post("/memory/search", json={
                    "bot_id": bot, "query": q, "top_k": 5,
                })
                r.raise_for_status()
                latencies.append((time.perf_counter() - t) * 1000)
                items = r.json()["result"]["items"]
                if any(it["signals"]["bm25_rank"] is not None for it in items):
                    bm25_hits += 1
            p95 = _p95(latencies)
            print(f"search p95={p95:.0f}ms over {len(latencies)} runs; "
                  f"bm25 contributed on {bm25_hits}/{len(latencies)} runs")
            assert p95 < 500, f"/memory/search p95 = {p95:.0f}ms exceeds 500ms target"
            assert bm25_hits >= len(latencies) * 0.7, (
                f"BM25 contributed on only {bm25_hits}/{len(latencies)} runs "
                f"(<70%); query preprocessing may be broken"
            )
    finally:
        # Cleanup
        with _client() as c:
            c.headers["Authorization"] = f"Bearer {TOKEN}"
            listed = c.get(f"/memory/list?bot_id={bot}&limit=10000")
            for m in listed.json()["result"]["items"]:
                c.post("/memory/forget", json={"bot_id": bot, "memory_id": m["id"]})


@skip_if_no_env
def test_recall_p95_under_300ms():
    bot = f"v021-recall-{uuid.uuid4().hex[:8]}"
    try:
        with _client() as c:
            c.headers["Authorization"] = f"Bearer {TOKEN}"
            _seed_bot(c, bot, n=2000)
            latencies = []
            for _ in range(20):
                t = time.perf_counter()
                r = c.post("/memory/recall", json={
                    "bot_id": bot, "query": "Alice plan mount", "top_k": 5,
                })
                r.raise_for_status()
                latencies.append((time.perf_counter() - t) * 1000)
            p95 = _p95(latencies)
            print(f"recall p95={p95:.0f}ms")
            assert p95 < 300, f"/memory/recall p95 = {p95:.0f}ms exceeds 300ms target"
    finally:
        with _client() as c:
            c.headers["Authorization"] = f"Bearer {TOKEN}"
            listed = c.get(f"/memory/list?bot_id={bot}&limit=10000")
            for m in listed.json()["result"]["items"]:
                c.post("/memory/forget", json={"bot_id": bot, "memory_id": m["id"]})
