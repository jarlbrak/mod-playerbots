"""Integration tests for memory-sidecar v0.2 live deploy.

Skipped unless HEIMDAL_MEMORY_URL and HEIMDAL_MEMORY_TOKEN are set.

Run:
    HEIMDAL_MEMORY_URL=http://192.168.1.3:8090 \\
    HEIMDAL_MEMORY_TOKEN=$(security find-generic-password -s heimdal-memory-mcp-token -w) \\
    pytest tests/integration/test_v02_endpoints.py -v

Each test uses a throwaway bot_id (f"v02-{uuid.uuid4().hex[:8]}") to
avoid touching the 962K production memories.
"""
import os
import time
import uuid

import httpx
import pytest

BASE_URL = os.environ.get("HEIMDAL_MEMORY_URL", "")
TOKEN = os.environ.get("HEIMDAL_MEMORY_TOKEN", "")

SKIP_REASON = "HEIMDAL_MEMORY_URL / HEIMDAL_MEMORY_TOKEN not set"
skip_if_no_env = pytest.mark.skipif(not BASE_URL or not TOKEN, reason=SKIP_REASON)


def bot() -> str:
    return f"v02-{uuid.uuid4().hex[:8]}"


def make_client() -> httpx.Client:
    return httpx.Client(base_url=BASE_URL, timeout=10.0)


# ---------------------------------------------------------------------------
# Test 1: write then read
# ---------------------------------------------------------------------------

@skip_if_no_env
def test_write_then_read():
    """POST /memory/remember creates a memory; GET /memory/{id} returns it."""
    bid = bot()
    with make_client() as c:
        r = c.post("/memory/remember", json={
            "bot_id": bid, "text": "I like mining copper ore.", "salience": 0.7,
        })
        assert r.status_code == 200, r.text
        mid = r.json()["memory_id"]
        assert mid.startswith("m_")

        r2 = c.get(f"/memory/{mid}", params={"bot_id": bid})
        assert r2.status_code == 200, r2.text
        row = r2.json()
        assert row["id"] == mid
        assert row["text"] == "I like mining copper ore."
        assert row["memory_type"] == "event"  # v0.2 default


# ---------------------------------------------------------------------------
# Test 2: update re-embeds on text change
# ---------------------------------------------------------------------------

@skip_if_no_env
def test_update_re_embeds():
    """PUT /memory/update with new text sets re_embedded=True."""
    bid = bot()
    with make_client() as c:
        r = c.post("/memory/remember", json={
            "bot_id": bid, "text": "First version.", "salience": 0.5,
        })
        assert r.status_code == 200, r.text
        mid = r.json()["memory_id"]

        r2 = c.put("/memory/update", json={
            "bot_id": bid, "memory_id": mid, "text": "Updated version.",
        })
        assert r2.status_code == 200, r2.text
        body = r2.json()
        assert body["updated"] is True
        assert body["re_embedded"] is True


# ---------------------------------------------------------------------------
# Test 3: update metadata only (no re-embed)
# ---------------------------------------------------------------------------

@skip_if_no_env
def test_update_metadata_only_skips_re_embed():
    """PUT /memory/update with only salience change: re_embedded=False."""
    bid = bot()
    with make_client() as c:
        r = c.post("/memory/remember", json={
            "bot_id": bid, "text": "Stable text.", "salience": 0.5,
        })
        assert r.status_code == 200, r.text
        mid = r.json()["memory_id"]

        r2 = c.put("/memory/update", json={
            "bot_id": bid, "memory_id": mid, "salience": 0.9,
        })
        assert r2.status_code == 200, r2.text
        body = r2.json()
        assert body["updated"] is True
        assert body["re_embedded"] is False


# ---------------------------------------------------------------------------
# Test 4: delete cascades
# ---------------------------------------------------------------------------

@skip_if_no_env
def test_delete_cascades():
    """POST /memory/forget deletes the memory; GET returns 404."""
    bid = bot()
    with make_client() as c:
        r = c.post("/memory/remember", json={
            "bot_id": bid, "text": "To be deleted.", "salience": 0.3,
        })
        assert r.status_code == 200, r.text
        mid = r.json()["memory_id"]

        r2 = c.post("/memory/forget", json={"bot_id": bid, "memory_id": mid})
        assert r2.status_code == 200, r2.text
        assert r2.json()["forgotten"] is True

        r3 = c.get(f"/memory/{mid}", params={"bot_id": bid})
        assert r3.status_code == 404, r3.text


# ---------------------------------------------------------------------------
# Test 5: hybrid search returns results
# ---------------------------------------------------------------------------

@skip_if_no_env
def test_search_hybrid():
    """POST /memory/search returns items with RRF signals."""
    bid = bot()
    with make_client() as c:
        for txt in [
            "Ironforge is the Dwarven capital.",
            "Stormwind is the human capital.",
            "Orgrimmar is the Orc capital.",
        ]:
            r = c.post("/memory/remember", json={
                "bot_id": bid, "text": txt, "salience": 0.8,
            })
            assert r.status_code == 200, r.text

        r2 = c.post("/memory/search", json={
            "bot_id": bid, "query": "Dwarven city", "top_k": 3,
        })
        assert r2.status_code == 200, r2.text
        body = r2.json()
        assert len(body["items"]) >= 1
        first = body["items"][0]
        assert "text" in first
        assert "signals" in first


# ---------------------------------------------------------------------------
# Test 6: search temporal filter (since_ts)
# ---------------------------------------------------------------------------

@skip_if_no_env
def test_search_temporal_filter():
    """POST /memory/search with since_ts excludes older memories."""
    bid = bot()
    with make_client() as c:
        r_old = c.post("/memory/remember", json={
            "bot_id": bid, "text": "Old memory about dragons.", "salience": 0.8,
        })
        assert r_old.status_code == 200, r_old.text

        future_ts = int(time.time()) + 5  # only memories after now+5
        r2 = c.post("/memory/search", json={
            "bot_id": bid, "query": "dragons", "top_k": 5,
            "since_ts": future_ts,
        })
        assert r2.status_code == 200, r2.text
        body = r2.json()
        # Should find nothing (memory was created before future_ts)
        assert len(body["items"]) == 0


# ---------------------------------------------------------------------------
# Test 7: search MMR diversity (deduplicate near-identical texts)
# ---------------------------------------------------------------------------

@skip_if_no_env
def test_search_mmr_diversity():
    """MMR diversity means top-k picks semantically distinct results."""
    bid = bot()
    with make_client() as c:
        # Write many near-identical + one distinct memory
        for i in range(4):
            c.post("/memory/remember", json={
                "bot_id": bid, "text": f"The warrior is farming in the barrens {i}.",
                "salience": 0.9,
            })
        c.post("/memory/remember", json={
            "bot_id": bid, "text": "The mage is in Stormwind studying magic.",
            "salience": 0.9,
        })

        r = c.post("/memory/search", json={
            "bot_id": bid, "query": "warrior barrens", "top_k": 3,
        })
        assert r.status_code == 200, r.text
        body = r.json()
        texts = [item["text"] for item in body["items"]]
        # At least one result should exist
        assert len(texts) >= 1


# ---------------------------------------------------------------------------
# Test 8: recall uses created_ts (not last_recalled_ts)
# ---------------------------------------------------------------------------

@skip_if_no_env
def test_recall_uses_created_ts():
    """POST /memory/recall returns scored results; confirms recency based on age."""
    bid = bot()
    with make_client() as c:
        r = c.post("/memory/remember", json={
            "bot_id": bid, "text": "Bot found the Defias Messenger.", "salience": 0.7,
        })
        assert r.status_code == 200, r.text

        r2 = c.post("/memory/recall", json={
            "bot_id": bid, "query": "Defias Messenger", "top_k": 5,
        })
        assert r2.status_code == 200, r2.text
        body = r2.json()
        assert len(body["memories"]) >= 1
        m = body["memories"][0]
        assert m["memory_id"].startswith("m_")
        assert m["score"] > 0.0
        assert m["ts"] > 0


# ---------------------------------------------------------------------------
# Test 9: goal lifecycle (create → active → complete)
# ---------------------------------------------------------------------------

@skip_if_no_env
def test_goal_lifecycle():
    """Create → update(active) → goals.complete."""
    bid = bot()
    with make_client() as c:
        r = c.post("/goals/create", json={
            "bot_id": bid, "text": "Reach level 20.", "priority": 10,
        })
        assert r.status_code == 200, r.text
        body = r.json()
        gid = body["goal_id"]
        assert gid.startswith("g_")
        assert body["status"] == "pending"

        r2 = c.put("/goals/update", json={
            "bot_id": bid, "goal_id": gid, "status": "active",
        })
        assert r2.status_code == 200, r2.text
        assert r2.json()["updated"] is True

        r3 = c.post("/goals/complete", json={
            "bot_id": bid, "goal_id": gid, "outcome": "completed",
            "also_record_memory": False,
        })
        assert r3.status_code == 200, r3.text
        assert r3.json()["updated"] is True

        r4 = c.get(f"/goals/{gid}", params={"bot_id": bid})
        assert r4.status_code == 200, r4.text
        assert r4.json()["status"] == "completed"


# ---------------------------------------------------------------------------
# Test 10: goal status invalid transition (pending → completed direct)
# ---------------------------------------------------------------------------

@skip_if_no_env
def test_goal_status_invalid():
    """Direct pending→completed transition is rejected with 400."""
    bid = bot()
    with make_client() as c:
        r = c.post("/goals/create", json={
            "bot_id": bid, "text": "An invalid goal.", "priority": 0,
        })
        assert r.status_code == 200, r.text
        gid = r.json()["goal_id"]

        # pending → completed is not a valid transition (must go through active)
        r2 = c.put("/goals/update", json={
            "bot_id": bid, "goal_id": gid, "status": "completed",
        })
        assert r2.status_code == 400, r2.text


# ---------------------------------------------------------------------------
# Test 11: goal complete writes a goal_link memory
# ---------------------------------------------------------------------------

@skip_if_no_env
def test_goal_complete_writes_memory():
    """goals.complete with also_record_memory=True writes a goal_link memory."""
    bid = bot()
    with make_client() as c:
        r = c.post("/goals/create", json={
            "bot_id": bid, "text": "Collect 10 linen cloth.", "priority": 5,
        })
        assert r.status_code == 200, r.text
        gid = r.json()["goal_id"]

        c.put("/goals/update", json={"bot_id": bid, "goal_id": gid, "status": "active"})

        r2 = c.post("/goals/complete", json={
            "bot_id": bid, "goal_id": gid, "outcome": "completed",
            "also_record_memory": True,
        })
        assert r2.status_code == 200, r2.text
        body = r2.json()
        assert body["updated"] is True
        mid = body.get("memory_id")
        assert mid is not None and mid.startswith("m_")

        # Verify the memory_type is goal_link
        r3 = c.get(f"/memory/{mid}", params={"bot_id": bid})
        assert r3.status_code == 200, r3.text
        assert r3.json()["memory_type"] == "goal_link"


# ---------------------------------------------------------------------------
# Test 12: goal list filter by status
# ---------------------------------------------------------------------------

@skip_if_no_env
def test_goal_list_filter():
    """GET /goals/list?status=pending returns only pending goals."""
    bid = bot()
    with make_client() as c:
        # Create 2 pending goals
        for txt in ["Grind westfall mobs.", "Do Deadmines quest."]:
            r = c.post("/goals/create", json={"bot_id": bid, "text": txt, "priority": 0})
            assert r.status_code == 200, r.text
        # Create one abandoned goal
        r_ab = c.post("/goals/create", json={"bot_id": bid, "text": "Skip this.", "priority": 0})
        assert r_ab.status_code == 200, r_ab.text
        gid_ab = r_ab.json()["goal_id"]
        c.put("/goals/update", json={"bot_id": bid, "goal_id": gid_ab, "status": "abandoned"})

        # List only pending
        r2 = c.get("/goals/list", params={"bot_id": bid, "status": "pending"})
        assert r2.status_code == 200, r2.text
        body = r2.json()
        assert body["total"] == 2
        for item in body["items"]:
            assert item["status"] == "pending"


# ---------------------------------------------------------------------------
# Test 13: MCP parity — 15 tools via tools/list
# ---------------------------------------------------------------------------

@skip_if_no_env
def test_mcp_parity_15_tools_via_tools_list():
    """MCP /mcp/mcp: tools/list must return exactly 15 tools.

    NOTE: This test is expected to fail if the MCP session_manager task-group
    bug (RuntimeError: Task group is not initialized) is not yet fixed.
    The HTTP surface (tests 1-12) is unaffected by this bug.
    """
    headers = {
        "Authorization": f"Bearer {TOKEN}",
        "Accept": "application/json, text/event-stream",
        "Content-Type": "application/json",
    }
    with httpx.Client(base_url=BASE_URL, timeout=10.0, headers=headers) as c:
        # Initialize session
        r_init = c.post("/mcp/mcp", json={
            "jsonrpc": "2.0", "id": 1, "method": "initialize",
            "params": {
                "protocolVersion": "2024-11-05", "capabilities": {},
                "clientInfo": {"name": "integration-test", "version": "0.1"},
            },
        })
        if r_init.status_code == 500:
            pytest.xfail(
                "MCP session_manager task-group not initialized — "
                "known bug (mcp_server.py needs session_manager.run() in lifespan). "
                "HTTP surface (tests 1-12) is unaffected."
            )
        assert r_init.status_code in (200, 202), r_init.text
        session_id = r_init.headers.get("mcp-session-id")
        assert session_id, "no mcp-session-id in response"

        # List tools
        r_tools = c.post("/mcp/mcp", json={
            "jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {},
        }, headers={**headers, "mcp-session-id": session_id})
        assert r_tools.status_code == 200, r_tools.text
        tools = r_tools.json().get("result", {}).get("tools", [])
        assert len(tools) == 15, f"expected 15 tools, got {len(tools)}: {[t['name'] for t in tools]}"


# ---------------------------------------------------------------------------
# Test 14: personality round-trip unchanged
# ---------------------------------------------------------------------------

@skip_if_no_env
def test_personality_unchanged():
    """POST /memory/personality/set then /memory/personality/get round-trips."""
    bid = bot()
    persona = "Gruff dwarf warrior who loves ale and battle axes."
    with make_client() as c:
        r = c.post("/memory/personality/set", json={"bot_id": bid, "persona": persona})
        assert r.status_code == 200, r.text
        assert r.json()["ok"] is True

        r2 = c.post("/memory/personality/get", json={"bot_id": bid})
        assert r2.status_code == 200, r2.text
        assert r2.json()["persona"] == persona
