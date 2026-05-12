import json
import pytest

from llmbench.validator import validate_response


SCHEMA_PATH = "schemas/goal_schema.json"


@pytest.fixture(scope="module")
def schema():
    with open(SCHEMA_PATH) as f:
        return json.load(f)


def test_validate_response_good(schema):
    raw = json.dumps(
        {
            "goal": "do_quest",
            "params": {"quest_id": 502, "starting_objective_idx": 1},
            "ttl_minutes": 30,
            "reasoning": "Continue the quest.",
        }
    )
    valid, err = validate_response(raw, schema)
    assert valid is True
    assert err is None


def test_validate_response_bad_enum(schema):
    raw = json.dumps(
        {
            "goal": "invent_new_goal",
            "params": {},
            "ttl_minutes": 30,
            "reasoning": "x",
        }
    )
    valid, err = validate_response(raw, schema)
    assert valid is False
    assert "invent_new_goal" in (err or "")


def test_validate_response_missing_field(schema):
    raw = json.dumps({"goal": "rest", "params": {}})
    valid, err = validate_response(raw, schema)
    assert valid is False
    assert "ttl_minutes" in (err or "")


def test_validate_response_not_json(schema):
    valid, err = validate_response("definitely not JSON", schema)
    assert valid is False
    assert "JSON" in (err or "") or "decode" in (err or "").lower()


def test_validate_response_ttl_out_of_range(schema):
    raw = json.dumps(
        {
            "goal": "rest",
            "params": {},
            "ttl_minutes": 9999,
            "reasoning": "too long",
        }
    )
    valid, err = validate_response(raw, schema)
    assert valid is False
