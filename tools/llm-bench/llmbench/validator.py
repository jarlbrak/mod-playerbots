"""Post-hoc structural validation of llama-server responses."""

import json
from typing import Any

import jsonschema


def validate_response(raw: str, schema: dict[str, Any]) -> tuple[bool, str | None]:
    """Parse `raw` as JSON, validate against `schema`. Returns (valid, error_message).

    Phase 0.5 structural only — does NOT check that quest_id exists in DB
    or that NPCs are in range. Those semantic validators are Phase 1's
    job (C++ side).
    """
    try:
        parsed = json.loads(raw)
    except json.JSONDecodeError as e:
        return False, f"JSON decode error: {e}"
    try:
        jsonschema.validate(parsed, schema)
    except jsonschema.ValidationError as e:
        return False, e.message
    return True, None
