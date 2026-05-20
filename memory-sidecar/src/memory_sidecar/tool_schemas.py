"""MCP tool argument schemas (mirror HTTP route request models)."""
from __future__ import annotations
from typing import Optional
from pydantic import BaseModel, Field


class MemoryWriteArgs(BaseModel):
    bot_id: str
    text: str
    salience: float
    entities: list[str] = Field(default_factory=list)
    relations: list[dict[str, str]] = Field(default_factory=list)


class MemoryReadArgs(BaseModel):
    bot_id: str
    memory_id: str


class MemoryUpdateArgs(BaseModel):
    bot_id: str
    memory_id: str
    text: Optional[str] = None
    salience: Optional[float] = None
    memory_type: Optional[str] = None
    source: Optional[str] = None


class MemoryDeleteArgs(BaseModel):
    bot_id: str
    memory_id: str


class MemoryRecallArgs(BaseModel):
    bot_id: str
    query: str
    top_k: int = 5
    since_ts: Optional[int] = None
    until_ts: Optional[int] = None
    memory_type: Optional[str] = None


class MemoryRecallAboutArgs(BaseModel):
    bot_id: str
    entity: str
    max_hops: int = 2
    top_k: int = 3
    since_ts: Optional[int] = None
    until_ts: Optional[int] = None
    memory_type: Optional[str] = None


class MemorySearchArgs(BaseModel):
    bot_id: str
    query: str
    top_k: int = 5
    since_ts: Optional[int] = None
    until_ts: Optional[int] = None
    memory_type: Optional[str] = None


class MemoryListArgs(BaseModel):
    bot_id: str
    memory_type: Optional[str] = None
    source: Optional[str] = None
    since_ts: Optional[int] = None
    until_ts: Optional[int] = None
    limit: int = 50
    offset: int = 0


class PersonalityGetArgs(BaseModel):
    bot_id: str


class PersonalitySetArgs(BaseModel):
    bot_id: str
    persona: str


class GoalCreateArgs(BaseModel):
    bot_id: str
    text: str
    source: Optional[str] = None
    priority: int = 0
    origin_memory: Optional[str] = None


class GoalReadArgs(BaseModel):
    bot_id: str
    goal_id: str


class GoalUpdateArgs(BaseModel):
    bot_id: str
    goal_id: str
    text: Optional[str] = None
    status: Optional[str] = None
    priority: Optional[int] = None
    origin_memory: Optional[str] = None


class GoalListArgs(BaseModel):
    bot_id: str
    status: Optional[str] = None
    limit: int = 50
    offset: int = 0


class GoalCompleteArgs(BaseModel):
    bot_id: str
    goal_id: str
    outcome: str
    also_record_memory: bool = True


TOOL_SCHEMAS: dict[str, tuple[type[BaseModel], str]] = {
    "memory.write":            (MemoryWriteArgs,       "Create a memory."),
    "memory.read":             (MemoryReadArgs,        "Read a memory by id."),
    "memory.update":           (MemoryUpdateArgs,      "Update memory text/metadata; re-embeds on text change."),
    "memory.delete":           (MemoryDeleteArgs,      "Delete a memory + embeddings + entity links."),
    "memory.recall":           (MemoryRecallArgs,      "Semantic recall via vec_memories KNN + MMR."),
    "memory.recall_about":     (MemoryRecallAboutArgs, "Entity-anchored BFS recall + MMR."),
    "memory.search":           (MemorySearchArgs,      "Hybrid: BM25 + dense + entity, RRF + MMR."),
    "memory.list":             (MemoryListArgs,        "List memories filtered by type/source/time."),
    "memory.personality_get":  (PersonalityGetArgs,    "Get a bot's persona."),
    "memory.personality_set":  (PersonalitySetArgs,    "Set a bot's persona (<=4000 chars)."),
    "goals.create":            (GoalCreateArgs,        "Create a goal (status='pending')."),
    "goals.read":              (GoalReadArgs,          "Read a goal by id."),
    "goals.update":            (GoalUpdateArgs,        "Update goal; validates status transitions."),
    "goals.list":              (GoalListArgs,          "List goals filtered by status."),
    "goals.complete":          (GoalCompleteArgs,      "Mark completed/abandoned; optionally records a goal_link memory."),
}
