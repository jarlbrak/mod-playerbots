"""FastMCP server factory for the memory sidecar.

Tool handlers call the FastAPI route functions directly so HTTP and
MCP cannot drift. Four SDK-drift workarounds from kb_642162c3 inline.
"""
from typing import Any, Awaitable, Callable, Optional

from mcp.server.fastmcp import Context, FastMCP
from mcp.server.transport_security import TransportSecuritySettings
from mcp.server.auth.middleware.auth_context import get_access_token
from mcp.server.auth.settings import AuthSettings
from pydantic import AnyHttpUrl

from memory_sidecar.mcp_auth import TokenStore, TokenStoreVerifier
from memory_sidecar.tool_schemas import TOOL_SCHEMAS


Dispatcher = Callable[[Any], Awaitable[Any]]


def _make_handler(tool_name: str, schema_cls: type, dispatch_fn: Dispatcher):
    async def _handler(ctx, args) -> dict:
        access = get_access_token()
        if access is None:
            return {"ok": False, "error": "unauthorized"}
        try:
            result = await dispatch_fn(args)
            if hasattr(result, "model_dump"):
                return {"ok": True, "result": result.model_dump()}
            return {"ok": True, "result": result}
        except Exception as e:
            return {"ok": False, "error": "internal_error", "detail": str(e)}

    # kb_642162c3 §2 workaround: explicit annotations so FastMCP's
    # inspect.signature(eval_str=True) can resolve them even with
    # from __future__ import annotations active in calling code.
    _handler.__annotations__ = {
        "ctx": Context, "args": schema_cls, "return": dict,
    }
    _handler.__name__ = f"tool_{tool_name.replace('.', '_')}"
    return _handler


def build_mcp_server(
    *,
    token_store: TokenStore,
    dispatchers: dict[str, Dispatcher],
    allowed_hosts: Optional[list[str]] = None,
    resource_url: str = "http://127.0.0.1:8090/mcp",
) -> FastMCP:
    hosts = allowed_hosts if allowed_hosts is not None else [
        "127.0.0.1:*", "localhost:*",
    ]
    origins = [f"http://{h}" for h in hosts] + [f"https://{h}" for h in hosts]
    transport_security = TransportSecuritySettings(
        enable_dns_rebinding_protection=True,
        allowed_hosts=hosts,
        allowed_origins=origins,
    )
    mcp = FastMCP(
        name="memory-sidecar",
        token_verifier=TokenStoreVerifier(token_store),
        auth=AuthSettings(
            issuer_url=AnyHttpUrl(resource_url),
            resource_server_url=AnyHttpUrl(resource_url),
            required_scopes=[],
        ),
        transport_security=transport_security,
    )
    for tool_name, (schema_cls, description) in TOOL_SCHEMAS.items():
        if tool_name not in dispatchers:
            print(f"[mcp] dispatcher missing for {tool_name}; skipping")
            continue
        handler = _make_handler(tool_name, schema_cls, dispatchers[tool_name])
        mcp.add_tool(handler, name=tool_name, description=description)
    return mcp
