"""In-process asyncio pubsub for fanning new memory rows to SSE subscribers."""
from __future__ import annotations

import asyncio
import logging
from collections import defaultdict

logger = logging.getLogger(__name__)


class PubSub:
    """Per-bot subscriber registry.

    Each subscriber owns an asyncio.Queue (maxsize=64). On publish, the new
    row is fanned out to all queues for that bot. On queue overflow (slow
    consumer), the oldest entry is dropped and a warning is logged; the
    subscriber will recover via Last-Event-ID replay on reconnect.
    """

    QUEUE_MAXSIZE = 64

    def __init__(self) -> None:
        self.subscribers: dict[str, set[asyncio.Queue]] = defaultdict(set)

    def subscribe(self, bot_id: str) -> asyncio.Queue:
        q: asyncio.Queue = asyncio.Queue(maxsize=self.QUEUE_MAXSIZE)
        self.subscribers[bot_id].add(q)
        return q

    def unsubscribe(self, bot_id: str, q: asyncio.Queue) -> None:
        self.subscribers[bot_id].discard(q)
        if not self.subscribers[bot_id]:
            del self.subscribers[bot_id]

    def publish(self, bot_id: str, row: dict) -> None:
        for q in list(self.subscribers.get(bot_id, ())):
            try:
                q.put_nowait(row)
            except asyncio.QueueFull:
                logger.warning("sse_subscriber_overflow bot_id=%s", bot_id)
                try:
                    q.get_nowait()
                    q.put_nowait(row)
                except Exception:
                    pass
