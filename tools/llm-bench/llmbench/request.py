"""Build chat-completion request bodies from a fixture + grammar choice."""

import json
from typing import Any, Literal

SYSTEM_PROMPT = (
    "You decide what this WoW bot does next. "
    "Return JSON matching the supplied schema."
)


GrammarMode = Literal["json_schema", "gbnf"]


def build_request(
    *,
    fixture: dict[str, Any],
    grammar_mode: GrammarMode,
    schema: dict[str, Any] | None,
    gbnf: str | None,
    max_tokens: int,
    temperature: float,
) -> dict[str, Any]:
    body: dict[str, Any] = {
        "messages": [
            {"role": "system", "content": SYSTEM_PROMPT},
            {"role": "user", "content": json.dumps(fixture)},
        ],
        "max_tokens": max_tokens,
        "temperature": temperature,
    }

    if grammar_mode == "json_schema":
        if schema is None:
            raise ValueError("json_schema mode requires schema=...")
        body["response_format"] = {
            "type": "json_schema",
            "json_schema": {"name": "BotGoal", "schema": schema, "strict": True},
        }
    elif grammar_mode == "gbnf":
        if gbnf is None:
            raise ValueError("gbnf mode requires gbnf=...")
        body["grammar"] = gbnf
    else:
        raise ValueError(f"unknown grammar_mode: {grammar_mode!r}")

    return body
