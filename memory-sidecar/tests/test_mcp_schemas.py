"""Parity: TOOL_SCHEMAS field names match the matching HTTP request models."""
import pytest

from memory_sidecar.tool_schemas import TOOL_SCHEMAS
from memory_sidecar import routes_memory as rm, routes_personality as rp, routes_goals as rg


TOOL_TO_REQUEST_MODEL: dict[str, type | None] = {
    "memory.write":            rm.RememberRequest,
    "memory.read":             None,   # GET — kwarg splat in main.py
    "memory.update":           rm.UpdateRequest,
    "memory.delete":           rm.ForgetRequest,
    "memory.recall":           rm.RecallRequest,
    "memory.recall_about":     rm.RecallAboutRequest,
    "memory.search":           rm.SearchRequest,
    "memory.list":             None,   # GET — kwarg splat in main.py
    "memory.personality_get":  rp.PersonalityGetRequest,
    "memory.personality_set":  rp.PersonalitySetRequest,
    "goals.create":            rg.GoalCreateRequest,
    "goals.read":              None,   # GET — kwarg splat in main.py
    "goals.update":            rg.GoalUpdateRequest,
    "goals.list":              None,   # GET — kwarg splat in main.py
    "goals.complete":          rg.GoalCompleteRequest,
}


def test_all_15_tools_registered():
    assert len(TOOL_SCHEMAS) == 15


@pytest.mark.parametrize("tool_name", sorted(TOOL_SCHEMAS.keys()))
def test_every_tool_in_dispatcher_map(tool_name):
    assert tool_name in TOOL_TO_REQUEST_MODEL


@pytest.mark.parametrize("tool_name", sorted(TOOL_SCHEMAS.keys()))
def test_schema_fields_match_request_model(tool_name):
    mcp_cls, _ = TOOL_SCHEMAS[tool_name]
    req_cls = TOOL_TO_REQUEST_MODEL[tool_name]
    if req_cls is None:
        # GET endpoint — parity ensured by _get_kw splat in main.py
        return
    mcp_fields = set(mcp_cls.model_fields.keys())
    req_fields = set(req_cls.model_fields.keys())
    assert mcp_fields == req_fields, (
        f"{tool_name}: MCP fields {mcp_fields} != request model fields {req_fields}"
    )
