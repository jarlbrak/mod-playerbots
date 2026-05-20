"""Tests for build_fts5_query() — converts NL queries to FTS5 OR-joined terms."""
import pytest

from memory_sidecar.helpers import build_fts5_query


def test_basic_drops_stopwords_and_or_joins():
    out = build_fts5_query("what did Alice and I plan about the mount")
    # tokens after lowercasing + stopword filter + len>=3:
    # alice, plan, about[stop], mount — wait "about" IS a stopword
    # Final: alice plan mount
    assert out == "alice OR plan OR mount"


def test_dedupes_preserving_order():
    out = build_fts5_query("Alice told Alice about Alice")
    # alice is repeated; should appear once
    assert out == "alice OR told"  # "about" is a stopword


def test_caps_at_max_terms():
    out = build_fts5_query(
        "alpha beta gamma delta epsilon zeta eta theta", max_terms=4
    )
    parts = out.split(" OR ")
    assert len(parts) == 4
    assert parts == ["alpha", "beta", "gamma", "delta"]


def test_all_stopwords_returns_none():
    # "one" (3 chars, not a stopword) and "only" (not a stopword) survive.
    # Use a query where every token is either a stopword or < 3 chars.
    assert build_fts5_query("is it the and") is None


def test_short_tokens_excluded():
    out = build_fts5_query("at on in it is by no")
    assert out is None


def test_empty_string_returns_none():
    assert build_fts5_query("") is None


def test_apostrophes_stripped_for_fts5_safety():
    out = build_fts5_query("Alice's mount and brother's gold")
    # Apostrophes break FTS5 MATCH syntax — they're stripped, so
    # "Alice's" becomes "alice" + "s" (and "s" drops for <3 chars).
    assert "alice" in out
    assert "brother" in out
    assert "mount" in out
    assert "gold" in out
    assert "'" not in out  # critical: no apostrophe leaks to FTS5


def test_punctuation_stripped():
    out = build_fts5_query("VanCleef!? -- Stonemasons; Deadmines.")
    assert out == "vancleef OR stonemasons OR deadmines"


def test_long_natural_query():
    q = "Tell me everything you remember about my conversations with Alice about gold and the mount"
    out = build_fts5_query(q, max_terms=6)
    parts = out.split(" OR ")
    assert len(parts) == 6
    # First 6 non-stopword, non-short, unique tokens in order:
    # tell, everything, remember, conversations, alice, gold
    # ("mount" is #7 — capped out by max_terms=6)
    assert parts == ["tell", "everything", "remember", "conversations", "alice", "gold"]


def test_unicode_word_characters_lost():
    # The regex [a-zA-Z'] strips non-ASCII; documented behavior.
    out = build_fts5_query("café over mountains")
    # "café" becomes "caf" + "" → "caf" survives (3 chars), then "over"[stop], then "mountains"
    # Actually "caf" is 3 chars, kept. "mountains" kept.
    assert out is not None
    parts = out.split(" OR ")
    assert "mountains" in parts
