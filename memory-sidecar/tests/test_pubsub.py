"""Unit tests for the in-process asyncio PubSub fan-out."""
import asyncio
import logging

import pytest

from memory_sidecar.pubsub import PubSub


def test_subscribe_returns_queue():
    ps = PubSub()
    q = ps.subscribe("1003")
    assert isinstance(q, asyncio.Queue)
    assert q.maxsize == 64
    assert q in ps.subscribers["1003"]


def test_unsubscribe_removes_queue_and_cleans_empty_bot():
    ps = PubSub()
    q = ps.subscribe("1003")
    ps.unsubscribe("1003", q)
    assert "1003" not in ps.subscribers


def test_unsubscribe_keeps_other_subscribers():
    ps = PubSub()
    q1 = ps.subscribe("1003")
    q2 = ps.subscribe("1003")
    ps.unsubscribe("1003", q1)
    assert q2 in ps.subscribers["1003"]
    assert q1 not in ps.subscribers["1003"]


def test_publish_delivers_to_all_subscribers_for_bot():
    ps = PubSub()
    q1 = ps.subscribe("1003")
    q2 = ps.subscribe("1003")
    q_other = ps.subscribe("9999")
    row = {"rowid": 1, "memory_id": "m_abc", "bot_id": "1003", "text": "received whisper"}
    ps.publish("1003", row)
    assert q1.get_nowait() == row
    assert q2.get_nowait() == row
    assert q_other.empty()


def test_publish_to_no_subscribers_is_noop():
    ps = PubSub()
    ps.publish("nobody", {"rowid": 1})  # must not raise


def test_publish_overflow_drops_oldest_and_logs(caplog):
    caplog.set_level(logging.WARNING)
    ps = PubSub()
    q = ps.subscribe("1003")
    for i in range(64):
        q.put_nowait({"rowid": i})
    assert q.full()
    ps.publish("1003", {"rowid": 99})
    # Oldest dropped, new accepted
    assert q.qsize() == 64
    # Drain and verify order: 1..63, then 99
    drained = [q.get_nowait()["rowid"] for _ in range(64)]
    assert drained[0] == 1
    assert drained[-1] == 99
    assert "sse_subscriber_overflow" in caplog.text
