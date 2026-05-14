# LLM Agent — Phase 5 (T3 chat brain) — Design Spec

**Status:** approved (brainstorm)
**Date:** 2026-05-13
**Parent design:** [`docs/llm_agent_design.md`](../../llm_agent_design.md) §12 (phased rollout) and §3.3 / §7.3 (chat seam + chat envelope schema)
**Predecessor phases:** Phase 1 (plumbing), Phase 2 (T1 validator+apply), Phase 3 (memory sidecar), Phase 4 (T2 tool-calling)

## 1. Goal

Phase 5 ships a third LLM tier — **T3 (chat brain)** — that makes bots converse with humans in-character. When a whisper, party invite, or group-join event arrives for an LLM-enabled bot, T3 fires *instead of* Phase 4's T2 path: one LLM call returns a `{utterance, side_effects}` envelope. The utterance is dispatched through AzerothCore's existing `chatReplies` queue; the side_effects flow through Phase 4's `ParseToolCalls`/`Validate`/`ApplyToolCall` pipeline unchanged.

The single-call envelope is the core mitigation against the local 7B model's "I'll help!" → does-nothing failure mode (parent design §7.3): the same generation produces the words *and* the actions that make the words true.

## 2. Scope decisions (locked during brainstorm)

| # | Decision | Choice |
|---|---|---|
| 1 | Trigger surface | Whisper + party invite + group-join (all three Phase-4 interaction events) |
| 2 | Relationship to Phase 4's T2 | T3 **replaces** T2 on these three events; one combined LLM call |
| 3 | Output channel routing | Hardcoded per event (whisper-event→whisper-back; invite-event→whisper-inviter; join-event→`party_chat`) |
| 4 | Side-effect tool set | Same 4 tools as Phase 4 (`accept_party_invite`, `leave_party`, `set_goal`, `memory.remember`) |
| 5 | Bot-to-bot policy | Humans only in production; the `.playerbots t3 inject_whisper` admin command bypasses for mechanical smoke (mirrors Phase 4 Task 11) |
| 6 | Failure mode | Silent (no canned fallback, no T2-fallback) |
| 7 | Memory recall depth | `recall_about(sender_name, hops=2, top_k=3)` per fire + per-`(bot, sender)` dialogue history buffer |
| 8 | Rate limiting | Per-bot cooldown only (~5s nominal). No per-human cap — this is a solo server, no DOS surface. |

## 3. Architecture

Phase 5 plugs into Phase 4's plumbing without replacing it. `LlmAgentManager` is already tier-aware (commit `a2a6d16d`, Phase 4 Task 10): `IsInFlight(guid, tier)`, `DrainResults(guid, tier)`, `LlmRequest::tier`. The worker thread already leaves `parsed_status = "ok"` for tier ≠ 1 (commit `c7f35ec7`, Phase 4 Task 13 hotfix), so T3 envelope parsing happens in the action without manager-side changes.

Strategy registration changes: `LlmAgentStrategy::InitTriggers` **unregisters** the Phase 4 `"llm interact"` (T2) trigger node and registers `"llm chat"` (T3) at relevance 16.0. The Phase 4 T2 implementation files stay in tree as future-use scaffolding (e.g., non-chat interaction events later, or Phase 5.1 chat-suppressed fast-path) but no trigger node references them.

Two new pieces of in-memory state:

- **`WhisperBuffer`** — per-`(bot_guid, sender_guid)` rolling window of recent whisper exchanges, both directions. Bounded by `Tier3.DialogueHistorySize` (default 6 per pair) and aged by `Tier3.WhisperWindowSeconds` (default 600).
- **`PersonaCache`** — in-process cache over `MemoryHttpClient::GetPersonality(bot_guid)`. TTL: `Tier3.PersonaCacheTtlSeconds` (default 600). Honors the memory client's existing sticky-down behavior.

## 4. File layout

```
src/Bot/LlmAgent/
├── Tiers/
│   └── Tier3_ChatBrain.h/.cpp        # BuildT3Digest + BuildT3RequestBody
├── Triggers/
│   └── LlmChatTrigger.h/.cpp
├── Actions/
│   └── LlmChatAction.h/.cpp
├── Chat/
│   ├── WhisperBuffer.h/.cpp
│   └── PersonaCache.h/.cpp
├── Schemas/
│   └── ChatEnvelope.h/.cpp           # ParsedChatEnvelope POD + ParseChatEnvelope free function
├── Context/
│   ├── LlmAgentTier3ActionContext.h
│   └── LlmAgentTier3TriggerContext.h
└── Tools/
    └── ToolCatalog.cpp               # Extract shared kToolCallOneOf; add kT3OutputSchema

tests/llmagent/
├── test_whisper_buffer.cpp
├── test_persona_cache.cpp
└── test_chat_envelope_parser.cpp
```

Files modified (not created):

- `src/Bot/LlmAgent/LlmAgentManager.{h,cpp}` — add `WhisperBuffer whispers_` + `Whispers()`, add `BotCooldownMap t3_cooldowns_` + `T3Cooldowns()`, extend `Shutdown()` to clear both
- `src/Bot/LlmAgent/LlmAgentConfig.{h,cpp}` — six new `Tier3.*` keys; default T3 system-prompt-suffix string
- `src/Bot/LlmAgent/Strategy/LlmAgentStrategy.cpp` — swap T2 trigger node for T3
- `src/Bot/Engine/BuildSharedActionContexts.cpp` — register `LlmAgentTier3ActionContext`; drop `LlmAgentTier2ActionContext`
- `src/Bot/Engine/BuildSharedTriggerContexts.cpp` — register `LlmAgentTier3TriggerContext`; drop `LlmAgentTier2TriggerContext`
- `src/Bot/LlmAgent/Hooks/LlmAgentHooks.cpp` — `OnWhisperReceived` also pushes to `WhisperBuffer` with `direction = incoming`
- `src/Bot/LlmAgent/Tools/ToolCatalog.{h,cpp}` — extract per-tool `oneOf` branches into shared `kToolCallOneOf`; add `kT3OutputSchema` that references it
- `src/Bot/LlmAgent/Telemetry/LlmCounters.{h,cpp}` — `chat_envelope_parsed{ok,schema_error,missing_utterance}`, `chat_utterances_queued`, `chat_event_kind{whisper,invite,join}`, `chat_sender_offline`
- `src/Script/PlayerbotCommandScript.cpp` — add `.playerbots t3 inject_whisper <bot> <text>` admin command
- `conf/playerbots.conf.dist` — six new keys

## 5. Data flow

### 5.1 Inbound — event arrives, T3 request enqueued

```
1. Human whispers bot (or invites, or joins bot's group)
2. AzerothCore fires the corresponding Playerbots script hook:
   - OnPlayerCanUseChat       (whisper)
   - OnInviteMember           (party invite)
   - OnAddMember              (group join)
3. Phase 4 hook forwards to LlmAgentHooks:
   - OnWhisperReceived        → InteractionEventBuffer.PushWhisper
                              + WhisperBuffer.Push(bot, sender, dir=incoming, text, ts)  [NEW]
   - OnPartyInviteReceived    → InteractionEventBuffer.PushInvite
   - OnGroupJoined            → InteractionEventBuffer.PushJoin
4. Next worldserver tick → LlmChatTrigger::IsActive:
   - Tier3.Enabled && LlmAgent.Enabled
   - bot is LLM-enabled (Selector.IsLlmBot)
   - t3_cooldowns.Eligible(guid)
   - Interactions.HasPending(guid)  OR  HasPendingResults(guid, tier=3)
5. LlmChatAction::Execute (worldserver thread):
   - If HasPendingResults: see 5.2 (drain).
   - Else build T3 request, enqueue, set t3_cooldown.
```

Request build (`Tier3_ChatBrain::BuildT3RequestBody`):

```cpp
nlohmann::json digest = BuildDigestJson(SnapshotBot(botAI));   // Phase 0 digest
digest.erase("quest_log");                                     // Phase 4 trim, kept
digest.erase("interaction_context");                           // T2 field, T3 replaces

// T3 additions
digest["event_kind"]      = "whisper" | "invite" | "join";
digest["sender_name"]     = sender_name;
digest["sender_message"]  = whisper_text;                      // empty for invite/join
digest["dialogue_history"] = whispers_.SnapshotFor(bot, sender);
digest["memory_about_sender"] = mem.RecallAbout(bot, sender_name, 2, 3);

std::string persona = persona_cache_.Get(bot_guid);            // sticky if sidecar down
std::string system  = persona + "\n\n" + cfg.Tier3_SystemPromptSuffix;

body = {
  "model":           cfg.Model,
  "messages":        [{"role":"system","content":system},
                      {"role":"user","content":digest.dump()}],
  "response_format": {"type":"json_schema",
                      "json_schema":{"name":"chat_envelope","schema":parse(kT3OutputSchema)}},
  "temperature":     0.7,
  "max_tokens":      300
}
```

### 5.2 Outbound — result arrives, utterance dispatched

```
6. Worker thread completes HTTP call; posts LlmResult to results_[guid][tier=3].
   parsed_status = "ok" (no manager-side schema check for tier ≠ 1, per Phase 4 c7f35ec7).
7. Next tick → LlmChatTrigger::IsActive sees HasPendingResults(guid, 3).
8. LlmChatAction::Execute:
   a. mgr.DrainResults(guid, 3) → vector<LlmResult>
   b. For each result:
      - if parsed_status != "ok": IncFallbackUsed; t3_cooldown=FallbackCooldownMs; continue
      - envelope = ParseChatEnvelope(r.raw_response)
        - on ParseError: IncChatEnvelope("schema_error"); t3_cooldown=FallbackCooldownMs; continue
      - For each side_effect in envelope.side_effects:
        - Validate(call, InteractionContext) — Phase 4 validators
        - If accepted: ApplyToolCall — Phase 4 executors; IncToolApplied
        - If rejected: IncToolRejected(reason); continue with next side_effect
                       (memory.remember rejection rule retained from Phase 4: never halts;
                        all others halt the loop only on apply-throw)
      - Build ChatQueuedReply from envelope.utterance + event_kind:
        - whisper: {type=CHAT_MSG_WHISPER, name=sender_name, guid1=sender_guid}
        - invite:  {type=CHAT_MSG_WHISPER, name=inviter_name, guid1=inviter_guid}
        - join:    {type=CHAT_MSG_PARTY,   guid1=group_guid}
      - botAI->QueueChatResponse(reply); IncChatUtterancesQueued
      - WhisperBuffer.Push(bot, sender, dir=outgoing, utterance, now)
   c. Interactions.Clear(guid) — consumed; don't refire on next tick
9. Next tick → ChatReplyAction picks up chatReplies and sends the packet (AC's existing path)
```

### 5.3 JSONL record per T3 fire

In addition to Phase 1 fields:

```json
{
  "tier": 3,
  "event_kind": "whisper" | "invite" | "join",
  "sender_name": "...",
  "raw_response": "<envelope JSON>",
  "utterance_chars": 87,
  "side_effects_applied": ["accept_party_invite", "memory.remember"],
  "side_effects_rejected": []
}
```

## 6. Schemas

### 6.1 T3 output envelope (`kT3OutputSchema`)

```json
{
  "type": "object",
  "required": ["utterance", "side_effects"],
  "additionalProperties": false,
  "properties": {
    "utterance":    {"type": "string", "minLength": 1, "maxLength": 200},
    "side_effects": {
      "type": "array",
      "maxItems": 3,
      "items": {"oneOf": [<refs to kToolCallOneOf>]}
    }
  }
}
```

### 6.2 Shared `kToolCallOneOf`

Phase 4 commit `ac9233cc` defined a 4-branch `oneOf` inside `kT2ToolCallOutputSchema`. Phase 5 extracts those four branch literals into a separate `kToolCallOneOf` string constant in `ToolCatalog.cpp`. Both `kT2ToolCallOutputSchema` (kept for future use) and `kT3OutputSchema` reference it. The four branches are: `accept_party_invite{from}`, `leave_party{}`, `set_goal{goal, params, reasoning, ttl_minutes}`, `memory.remember{text, entities, salience, relations?}`.

### 6.3 T3 system prompt suffix (after persona text)

```
You are this character. A real human player has whispered/invited/joined-the-group-of you. Reply with ONLY a JSON object: {"utterance": "<what you say>", "side_effects": [<0..N tool calls>]}.
Utterance: in-character, ≤200 chars, no markdown, no third-person narration.
Side-effects: actions you take ALONGSIDE the utterance — accept_party_invite, leave_party, set_goal, memory.remember.
If you say "I'll come" you MUST emit accept_party_invite. If you say "let me finish first" emit set_goal with the current quest. Never say one thing and do another.
When no action fits, side_effects is [].
```

### 6.4 `ParsedChatEnvelope` POD

```cpp
struct ParsedChatEnvelope {
    std::string utterance;
    std::vector<ParsedToolCall> side_effects;    // same variant as Phase 4
};

std::variant<ParsedChatEnvelope, ParseError>
ParseChatEnvelope(const std::string& raw_json);
```

## 7. Memory + persona integration

### 7.1 Persona

Phase 3 already calls `MemoryHttpClient::GetPersonality(guid)` lazily during `Tier0_StateDigest::BuildDigestJson` and writes a stub via `LlmAgentPersonality::StubPersonaText` on first access. Phase 3 stores but does NOT surface the persona to the LLM. Phase 5 fixes this:

- `PersonaCache::Get(guid)` returns the cached persona text, falling back to `MemoryHttpClient::GetPersonality` on miss, falling back to `StubPersonaText` if the sidecar is down. TTL = `Tier3.PersonaCacheTtlSeconds` (default 600).
- The persona text is prepended to the T3 system message, ahead of `kT3SystemPromptSuffix`. llama.cpp's continuous-batch prefix cache hits on every subsequent call from the same bot, amortizing the ~150-300 persona tokens.

### 7.2 Dialogue history (`WhisperBuffer`)

```cpp
struct WhisperEntry {
    enum Direction : uint8_t { Incoming, Outgoing };
    Direction   direction;
    std::string text;
    int64_t     ts;       // unix seconds
};

class WhisperBuffer {
public:
    void Push(uint64_t bot_guid, uint64_t sender_guid,
              WhisperEntry::Direction dir, std::string text, int64_t ts);

    // Returns last N entries newest-first, ages out entries older than window.
    // N defaults to Tier3.DialogueHistorySize; window to WhisperWindowSeconds.
    std::vector<WhisperEntry> SnapshotFor(uint64_t bot_guid, uint64_t sender_guid,
                                          int64_t now, int64_t window_seconds,
                                          size_t max_n);

    void ExpireOlderThan(int64_t now, int64_t window_seconds);
    void Clear(uint64_t bot_guid, uint64_t sender_guid);
    void ClearAll();

private:
    mutable std::mutex mu_;
    std::map<std::pair<uint64_t, uint64_t>, std::deque<WhisperEntry>> by_pair_;
};
```

Push on every whisper, both directions. Pushed on incoming by `LlmAgentHooks::OnWhisperReceived`. Pushed on outgoing by `LlmChatAction` right after `QueueChatResponse`.

### 7.3 `recall_about(sender_name)`

On each T3 fire, `Tier3_ChatBrain::BuildT3Digest` calls `MemoryHttpClient::RecallAbout(bot_guid, sender_name, hops=2, top_k=3)`. The returned hints land in `digest.memory_about_sender`. Per Phase 3 measurement, this adds ~10-30ms to digest assembly. If the sidecar is down, the client's existing 30s sticky-down cache returns empty and the digest field is `[]`.

## 8. Config surface

New keys appended to `conf/playerbots.conf.dist`:

```ini
# Phase 5 — Tier 3 chat brain
AiPlayerbot.LlmAgent.Tier3.Enabled = 1
AiPlayerbot.LlmAgent.Tier3.CooldownMs = 5000
AiPlayerbot.LlmAgent.Tier3.DialogueHistorySize = 6
AiPlayerbot.LlmAgent.Tier3.WhisperWindowSeconds = 600
AiPlayerbot.LlmAgent.Tier3.MaxUtteranceChars = 200
AiPlayerbot.LlmAgent.Tier3.SystemPromptSuffix = ""
AiPlayerbot.LlmAgent.Tier3.PersonaCacheTtlSeconds = 600
```

`Tier2.Enabled` remains in the config (Phase 4 added it) but no trigger references the T2 path after Phase 5; the key becomes dead until/unless Phase 5.1 reactivates non-chat T2 events.

## 9. Failure modes (locked to silent per scope Q6)

| Failure | Detection | Counter | Effect |
|---|---|---|---|
| llama-server unreachable | worker: `parsed_status = "transport_error"` | `parsed_transport_error++` | T3 cooldown = `FallbackCooldownMs` (5min); no chat; event stays buffered |
| HTTP 4xx/5xx | `parsed_status = "http_error"` | `parsed_http_error++` | Same |
| Envelope missing `utterance` | `ParseChatEnvelope` → `ParseError` | `chat_envelope_parsed[missing_utterance]++` | Bot silent; side_effects discarded |
| Envelope malformed JSON | Same | `chat_envelope_parsed[schema_error]++` | Same |
| `side_effect` element fails Validate | Phase 4 validators | `tool_rejected[reason]++` | Skip that side-effect; utterance still sent; other side-effects continue (memory.remember rejection never halts) |
| `ApplyToolCall` throws | Phase 4 try/catch | `tool_threw[name]++` | Halt remaining side-effects; utterance still sent if not yet queued |
| `QueueChatResponse` throws | Try/catch in `LlmChatAction` | `chat_queue_threw++` | Utterance lost; side-effects already applied stay |
| Sender no longer online when result lands | `ObjectAccessor::FindPlayer(sender_guid)` null | `chat_sender_offline++` | Utterance dropped; sender-dependent validators (e.g., `accept_party_invite`) hard-reject |

No canned-text fallback. No T2-fallback. Phase 2/3/4 pattern: counters increment, JSONL records the error, bot stays silent.

## 10. Testing

### 10.1 Unit tests (added to existing 137)

```
test_whisper_buffer.cpp                — 6 cases
test_persona_cache.cpp                 — 4 cases
test_chat_envelope_parser.cpp          — 7 cases
test_counters.cpp                      — +3 cases for chat_* buckets
                                      ─────
Total after Phase 5:                     157
```

Specific cases (one-line summary each):

- WhisperBuffer: push+snapshot newest-first; bounded at DialogueHistorySize; ExpireOlderThan drops stale; Clear(pair); ClearAll; concurrent push from two threads.
- PersonaCache: miss-fetches-from-client; hit-no-fetch; TTL expiry triggers refetch; sidecar-down keeps last good value.
- ChatEnvelopeParser: happy path (utterance + 1 side_effect); empty side_effects; missing utterance field; utterance not a string; side_effect element fails inner schema; malformed top-level JSON; utterance >200 chars (parser passes — schema rejects upstream).
- Counters: per-event_kind increment; envelope_parsed buckets; utterance_queued total.

### 10.2 Smoke validation (Heimdal)

- **Mechanical:** `.playerbots t3 inject_whisper <bot> "want to group up?"`. Expect within ~7s: JSONL record `tier:3, event_kind:"whisper", parsed_status:"ok"`, envelope with non-empty utterance, ≥1 side_effect attempted, `chat_utterances_queued ≥ 1`.
- **Soak:** 5-10 min after enable. Inject for 10 different bots across the three event kinds. Inspect distribution of `event_kind`, mean utterance length, side_effect application rate, T3 inference p50/p95.
- **Real player (optional):** log into WoW client; whisper a bot; observe in-character whisper reply within ~7s.

### 10.3 Success criteria

1. C++ tests ≈ 157/157
2. Python sidecar tests still 45/45 (no Python touched)
3. `Tier3.Enabled = 0` → zero behavior change from Phase 4
4. `LlmAgent.Enabled = 0` → byte-identical baseline
5. ≥ 3 T3 records observed end-to-end with `parsed_status: "ok"`, valid envelope, utterance dispatched to `chatReplies`, ≥ 1 side_effect applied
6. No worldserver-tick regression (mean ≤ 20ms, p95 ≤ 50ms)

## 11. Out of scope for Phase 5 (Phase 5.1+ candidates)

- **Public-channel chat** (`/say`, `/yell`, raid/guild). T3 only fires on whisper/invite/join; broader channels are atmospheric features for later.
- **Bot-to-bot chat.** Suppressed in Phase 5 except for the `.playerbots t3 inject_whisper` admin path. Parent design Open Q #4.
- **Per-human rate limiting.** Not needed on solo server.
- **Conversation persistence past restart.** `WhisperBuffer` is in-memory; cleared on worldserver restart. Phase 5.1 may write transcript summaries through `memory.remember`.
- **Channel choice by the model.** Hardcoded routing per event_kind. Phase 5.1 could add `channel` to the envelope schema.
- **Phase 4 T2 reactivation for non-chat events.** Files retained in tree but no trigger fires them. Phase 5.1 could add a separate T2 trigger for events that don't warrant chat (e.g., bot-on-bot invites if/when that path opens).
- **Cloud opt-in (T3+ per parent §10).** Phase 6.

## 12. Plan deviations from parent design

- **Persona in system prompt (not digest).** Parent design §6 puts persona in the digest field. We move it to the system prompt for prefix-caching. Same content; better cache behavior.
- **`Tier2.Enabled` not removed.** Phase 4 added this key; Phase 5 leaves the key in config but it's dead since the T2 trigger is unregistered. Cleaner than a removed key that someone might re-add expecting the old behavior. Phase 5.1 either removes it or reactivates T2 for non-chat events.

## 13. Open questions

1. **Utterance refusal pattern.** If the model returns `utterance = ""` (empty after `minLength: 1` is rejected by the schema generator), do we treat that as schema_error or as "I have nothing to say"? Current spec: schema_error, fall back silent. Revisit after smoke if Qwen produces empty strings often.
2. **Group-join self-leader filter.** Phase 4's `PlayerbotsGroupScript::OnAddMember` already filters `leader == joiner` (bot forming its own group). Phase 5 inherits this — bot doesn't say hello to itself.
3. **Whisper anti-spam from one bot to one human.** Without per-human rate limiting, a model could in theory hallucinate a long monologue across multiple T3 fires. Bounded by the 5s per-bot cooldown. Watch in smoke; revisit if observed.
