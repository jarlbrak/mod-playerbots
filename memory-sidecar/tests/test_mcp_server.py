"""MCP server boots and registers all 15 tools.

The streamable-HTTP transport (StreamableHTTPSessionManager) requires
anyio task-group initialization via run(). We don't spin that up in
unit tests — instead we verify the FastMCP object itself has the right
tool set via list_tools(), mirroring the harness-daemon pattern.
The /mcp/mcp HTTP wire is exercised by integration/test_v02_endpoints.py
against the live deploy.
"""
import pytest
from pathlib import Path

from memory_sidecar.mcp_auth import TokenStore, TokenRecord
from memory_sidecar.mcp_server import build_mcp_server
from memory_sidecar.tool_schemas import TOOL_SCHEMAS


@pytest.fixture
def dummy_token_store():
    return TokenStore([
        TokenRecord(token="test-token", identity="tester", scope=("memory.*", "goals.*"))
    ])


@pytest.fixture
def dummy_dispatchers():
    from unittest.mock import AsyncMock
    return {name: AsyncMock(return_value={"ok": True}) for name in TOOL_SCHEMAS}


@pytest.mark.asyncio
async def test_build_mcp_server_registers_all_15_tools(dummy_token_store, dummy_dispatchers):
    mcp = build_mcp_server(
        token_store=dummy_token_store,
        dispatchers=dummy_dispatchers,
    )
    tools = await mcp.list_tools()
    names = {t.name for t in tools}
    assert names == set(TOOL_SCHEMAS.keys()), (
        f"missing={set(TOOL_SCHEMAS) - names}, extra={names - set(TOOL_SCHEMAS)}"
    )
    assert len(tools) == 15


@pytest.mark.asyncio
async def test_all_tools_have_descriptions(dummy_token_store, dummy_dispatchers):
    mcp = build_mcp_server(
        token_store=dummy_token_store,
        dispatchers=dummy_dispatchers,
    )
    tools = await mcp.list_tools()
    for t in tools:
        assert t.description, f"{t.name} missing description"


def test_token_store_find():
    store = TokenStore([TokenRecord(token="abc", identity="user", scope=("memory.*",))])
    assert store.find("abc") is not None
    assert store.find("wrong") is None


def test_mcp_server_skips_missing_dispatcher(dummy_token_store, capsys):
    partial = {"memory.write": dummy_token_store}  # wrong type but we only care about key presence
    from unittest.mock import AsyncMock
    partial_dispatchers = {k: AsyncMock() for k in list(TOOL_SCHEMAS.keys())[:5]}
    mcp = build_mcp_server(
        token_store=dummy_token_store,
        dispatchers=partial_dispatchers,
    )
    captured = capsys.readouterr()
    # Should print [mcp] dispatcher missing for ... lines
    assert "[mcp] dispatcher missing for" in captured.out
