"""Bearer-token auth for MCP. Mirrors harness-daemon TokenStore (kb_642162c3)."""
from dataclasses import dataclass
from typing import Optional

import yaml
from mcp.server.auth.provider import AccessToken, TokenVerifier


@dataclass(frozen=True)
class TokenRecord:
    token: str
    identity: str
    scope: tuple[str, ...]


class TokenStore:
    def __init__(self, records: list[TokenRecord]) -> None:
        self._by_token = {r.token: r for r in records}

    def find(self, token: str) -> Optional[TokenRecord]:
        return self._by_token.get(token)

    @classmethod
    def load_yaml(cls, path: str) -> "TokenStore":
        with open(path, "r") as f:
            data = yaml.safe_load(f) or {}
        recs = [
            TokenRecord(token=e["token"], identity=e["identity"],
                        scope=tuple(e.get("scope", [])))
            for e in data.get("tokens", [])
        ]
        return cls(recs)


class TokenStoreVerifier(TokenVerifier):
    def __init__(self, store: TokenStore) -> None:
        self._store = store

    async def verify_token(self, token: str) -> Optional[AccessToken]:
        record = self._store.find(token)
        if record is None:
            return None
        return AccessToken(
            token=token, client_id=record.identity,
            scopes=list(record.scope), expires_at=None, resource=None,
        )
