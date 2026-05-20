"""HTTP routes for /memory/personality/*."""
import time
from typing import Any

from fastapi import APIRouter, HTTPException
from pydantic import BaseModel


PERSONA_MAX_CHARS = 4000


class PersonalityGetRequest(BaseModel):
    bot_id: str


class PersonalityGetResponse(BaseModel):
    persona: str


class PersonalitySetRequest(BaseModel):
    bot_id: str
    persona: str


class PersonalitySetResponse(BaseModel):
    ok: bool


def build_router(state: dict[str, Any]) -> APIRouter:
    router = APIRouter()

    @router.post("/memory/personality/get", response_model=PersonalityGetResponse)
    async def personality_get(req: PersonalityGetRequest):
        conn = state["conn"]
        cur = conn.execute(
            "SELECT persona FROM bots WHERE bot_id=?", (req.bot_id,)
        )
        row = cur.fetchone()
        if not row or row[0] is None:
            raise HTTPException(status_code=404, detail="no persona for this bot")
        return PersonalityGetResponse(persona=row[0])

    @router.post("/memory/personality/set", response_model=PersonalitySetResponse)
    async def personality_set(req: PersonalitySetRequest):
        if len(req.persona) > PERSONA_MAX_CHARS:
            raise HTTPException(
                status_code=400,
                detail=f"persona exceeds {PERSONA_MAX_CHARS} chars",
            )
        now = int(time.time())
        conn = state["conn"]
        conn.execute(
            "INSERT INTO bots (bot_id, persona, created_ts) VALUES (?, ?, ?) "
            "ON CONFLICT(bot_id) DO UPDATE SET persona=excluded.persona",
            (req.bot_id, req.persona, now),
        )
        conn.commit()
        return PersonalitySetResponse(ok=True)

    return router
