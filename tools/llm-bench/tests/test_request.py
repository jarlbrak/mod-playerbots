import json
import pytest

from llmbench.request import build_request, SYSTEM_PROMPT


@pytest.fixture
def tiny_fixture():
    return {"self": {"name": "Tiny", "level": 1}, "location": {"zone": "Test"}}


@pytest.fixture
def tiny_schema():
    return {"type": "object", "required": ["goal"], "properties": {"goal": {"type": "string"}}}


def test_build_request_json_schema_mode(tiny_fixture, tiny_schema):
    body = build_request(
        fixture=tiny_fixture,
        grammar_mode="json_schema",
        schema=tiny_schema,
        gbnf=None,
        max_tokens=200,
        temperature=0.3,
    )
    assert body["messages"][0]["role"] == "system"
    assert body["messages"][0]["content"] == SYSTEM_PROMPT
    assert body["messages"][1]["role"] == "user"
    # user content includes the fixture as JSON
    parsed = json.loads(body["messages"][1]["content"])
    assert parsed == tiny_fixture
    # response_format wires the schema
    assert body["response_format"]["type"] == "json_schema"
    assert body["response_format"]["json_schema"]["schema"] == tiny_schema
    assert "grammar" not in body
    assert body["max_tokens"] == 200
    assert body["temperature"] == 0.3


def test_build_request_gbnf_mode(tiny_fixture):
    gbnf = 'root ::= "{}"'
    body = build_request(
        fixture=tiny_fixture,
        grammar_mode="gbnf",
        schema=None,
        gbnf=gbnf,
        max_tokens=200,
        temperature=0.3,
    )
    assert body["grammar"] == gbnf
    assert "response_format" not in body


def test_build_request_unknown_mode_raises(tiny_fixture):
    with pytest.raises(ValueError):
        build_request(
            fixture=tiny_fixture,
            grammar_mode="rumpelstiltskin",
            schema=None,
            gbnf=None,
            max_tokens=200,
            temperature=0.3,
        )


def test_build_request_json_schema_mode_requires_schema(tiny_fixture):
    with pytest.raises(ValueError):
        build_request(
            fixture=tiny_fixture,
            grammar_mode="json_schema",
            schema=None,
            gbnf=None,
            max_tokens=200,
            temperature=0.3,
        )


def test_build_request_gbnf_mode_requires_gbnf(tiny_fixture):
    with pytest.raises(ValueError):
        build_request(
            fixture=tiny_fixture,
            grammar_mode="gbnf",
            schema=None,
            gbnf=None,
            max_tokens=200,
            temperature=0.3,
        )
