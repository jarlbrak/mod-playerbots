# Phase 5 T3 Chat-Brain — Smoke Test Results (PARTIAL)

**Date:** 2026-05-14
**Branch:** `claude/llm-agent-phase-5-t3-chat-brain`
**Head commit:** `c6b4b8a1` (`feat(llm-agent): playerbots.conf.dist gets seven Tier3 keys`)
**Predecessor:** Phase 4 ([results](../2026-05-13-llm-phase-4-smoke/summary.md))

## TL;DR — Status: DONE_WITH_CONCERNS

Phase 5 code builds, deploys, and the new `.playerbots t3 inject_whisper` admin command echoes "T3 whisper injected for %s". **But the LlmAgent Manager never opens its JSONL file** (i.e., `LlmAgentManager::Start` either isn't running its full body or is silently failing) and the **worldserver SIGSEGVs repeatedly under any LlmAgent activity** (10 coredumps captured between 15:00 and 16:36 PDT).

§11.5 success criterion (≥3 T3 records with `parsed_status: "ok"`) **not met**. Phase 5 code is on the branch and the unit tests are green (161/161), but the integration deploy has a real crash that needs debugging.

LlmAgent has been **disabled** (`Enabled = 0` in playerbots.conf, worldserver restarted clean) and is safe on Heimdal.

## What worked

| Component | Evidence |
|---|---|
| C++ unit tests | 161/161 passing locally (137 baseline + 7 WhisperBuffer + 4 PersonaCache + 1 schema-existence + 7 ChatEnvelope + 3 counters + 2 config) |
| Source overlay → builder image rebuild | Heimdal `wow-server:phase5-t3-rebuild` image built successfully from the overlay tar |
| Image deploy | `wow-server:current` retagged; `wow-worldserver.service` started on the new image |
| Phase 5 strings in binary | `grep -aoE` confirmed `llm chat`, `kT3OutputSchema`, `Tier3`, `inject_whisper`, `LlmAgentManager.cpp`, `PersonaCache.cpp` symbols all present |
| `.playerbots t3 inject_whisper` admin command | Returns "T3 whisper injected for %s" via the attach pipe |
| Worldserver world-init | "World Initialized In 0 Minutes 16 Seconds" appeared in Server.log; tick mean 9 ms / p95 27 ms |

## What's broken

### 1. JSONL file never opens

Manager.Start in `LlmAgentManager.cpp:60-97` opens `jsonl_` with `std::ios::app` immediately after the `if (!cfg_.Enabled) return;` early-return. With `LlmAgent.Enabled = 1` and `JsonlPath = /azerothcore/env/dist/logs/llm_agent_phase4.jsonl`, the file should be created on Start. **It is not.** Confirmed multiple times: `find /opt/containers/wow/logs -name '*phase4*'` returns no match after worldserver restart; the equivalent inside the container also shows the file missing.

Other clues:
- Worldserver thread count: **12** (Phase 4 with the LLM enabled showed ~25+). Suggests the 4 LlmAgent worker threads were never spawned, which means `Manager::Start` either took the `if (!cfg_.Enabled) return` path or crashed before reaching the worker spawn loop.
- No "Loaded playerbots config in" → LlmAgent log message gap (Phase 4 didn't log Start either, but the JSONL was the proxy signal).

Hypothesis: **`LoadLlmAgentConfig` is returning `cfg.Enabled = false`** even though the conf reads `AiPlayerbot.LlmAgent.Enabled = 1`. Possible causes:
- A new config field added in Phase 5 (`Tier3_BuiltInSystemPromptSuffix`) gets initialized via direct assignment after `LoadLlmAgentConfig`; if the loader is called from `SConfigMgrSource`, the assignment to `Tier3_BuiltInSystemPromptSuffix` from `kDefaultTier3SystemPromptSuffix` might throw if the symbol resolves wrong at link time. **Untested.**
- Stale config-cache: AzerothCore loads conf at module-init; if the conf was being edited during init the file might have been read in a half-written state. (Unlikely — the conf has been stable for the last 30 minutes.)

### 2. Worldserver SIGSEGV pattern

`coredumpctl list` shows **10 SEGVs** of `worldserver` between 15:00:55 and 16:36:00 PDT, all `signal: 11 (SEGV)`. Stack trace from PID 656598 (most recent):

```
Stack trace of thread 15:
#0  0x000056416a8687ba n/a (/azerothcore/env/dist/bin/worldserver + 0xfe77ba)
#1  0x000056416c0b5b10 n/a (/azerothcore/env/dist/bin/worldserver + 0x2834b10)
```

Binary is `RelWithDebInfo` but stripped at install time — `addr2line` isn't available in the container or locally on the mac. Without symbol resolution, the offending function is unknown.

**Top hypotheses (most → least likely):**
- `LlmAgentHooks::OnWhisperReceived` calls `mgr.Whispers().Push(...)` on every incoming whisper (Phase 5 Task 9 addition). Bot-to-bot whispers happen frequently in idle simulation; if `mgr.Whispers()` accesses uninitialized state, every bot-to-bot whisper crashes the server.
- `LlmChatAction::Execute` references `mgr.Persona().Get(...)` indirectly via `BuildT3RequestBody`. `persona_` is a `std::unique_ptr<PersonaCache>` constructed in `Start` only inside `#ifndef LLMAGENT_UNIT_TESTS`. If `Manager::Start` exited early (Enabled=false), `persona_` is null and `.get()` returns null, then `*persona_` is UB.
- `Tier3_ChatBrain::BuildT3RequestBody` calls `mgr.MemoryClient().RecallAbout(...)` — Phase 4 used this safely, so unlikely to be the crash source by itself.
- `nlohmann::json::parse(kT3OutputSchema)` failure on malformed schema string. The macro-expanded JSON should be well-formed (unit test `kT3OutputSchema parses` passed locally) but the runtime build might have a different version.

## Run setup

- **Host:** Heimdal (Bazzite, AMD RX 6900 XT, Vulkan)
- **llama-server:** Qwen 2.5 7B Q4_K_M, `--parallel 8 --ctx-size 16384` (carried over from Phase 4)
- **Image:** `localhost/wow-server:phase5-t3-rebuild` (built ~1 hour before this report)
- **Worldserver tick:** mean 9 ms, p95 27 ms (healthy when LlmAgent disabled)
- **Bot population:** 1001 characters in world (auto-spawn), 1 connected GM session
- **Config:** `LlmAgent.Enabled = 0` at hand-off; Phase 5 Tier3 block present in conf

## Phase 5 success criteria

| # | Criterion | Status |
|---|---|---|
| 1 | C++ tests ≈ 161/161 | ✅ 161/161 locally |
| 2 | Python sidecar tests 45/45 | ✅ unchanged (no Python touched) |
| 3 | `Tier3.Enabled = 0` → zero behavior change | ⏳ untested (couldn't get T3 to work in the first place) |
| 4 | `LlmAgent.Enabled = 0` → byte-identical baseline | ✅ worldserver runs cleanly with `Enabled = 0` |
| 5 | ≥3 T3 records with `parsed_status: "ok"` + ≥1 side_effect applied | ❌ **not met** — JSONL never opened, T3 trigger never fired |
| 6 | No worldserver-tick regression | ✅ when LlmAgent disabled, tick is healthy. **When enabled, worldserver SEGVs.** |

## Next steps to unblock

1. **Build worldserver with debug symbols left in the install.** Re-strip the binary or build with `-DCMAKE_BUILD_TYPE=Debug` to make `addr2line` useful.
2. **Resolve the SEGV addresses.** With symbols, run `addr2line -e /path/to/worldserver -f -C 0xfe77ba 0x2834b10` to identify the function. Likely candidates listed under "Top hypotheses" above.
3. **Confirm `cfg_.Enabled` value at Start.** Add a `LOG_INFO("playerbots", "LlmAgent.Start: Enabled={}", cfg_.Enabled)` at the top of `LlmAgentManager::Start` (right after the `Shutdown()` call). Rebuild + redeploy. If it shows `Enabled=0` despite the conf, the loader is the bug; if `Enabled=1`, the early-return isn't the issue and the SEGV is in Start itself (or in a worker thread / hook called during init).
4. **Test bot-to-bot whisper hook in isolation.** Add a `LOG_INFO` at the top of `OnWhisperReceived` before any `mgr.*` access. Restart with `Enabled=1`. If the log line appears before each crash, the hook is the crash source.

## Operator state at hand-off

- `AiPlayerbot.LlmAgent.Enabled = 0` on Heimdal (verified)
- `wow-worldserver.service` is `active` and running stably on `wow-server:current` (image id `1ce2ddf4ee45`) with LlmAgent disabled
- All 13 Phase 5 task commits pushed to `origin/claude/llm-agent-phase-5-t3-chat-brain` (HEAD `c6b4b8a1`)
- 10 coredumps preserved in `/var/lib/systemd/coredump/` on Heimdal for later analysis
- Old Phase 4 `wow-server:phase4-tierdispatch` image still on Heimdal as the safe rollback if needed
- `wow-attach-p5.service` (transient systemd unit running `podman attach`) may still be running — `systemctl stop wow-attach-p5.service` to clean up

## Phase 5.1 (or 5.0 hotfix) candidates

Beyond the immediate SEGV / Start-isn't-running bug, these were flagged during the implementation:

1. **Counter-dump visibility** — `LlmCounters::DumpToLog()` writes via `LOG_INFO("playerbots", ...)` but the "playerbots" log channel doesn't surface to `Server.log` or `journalctl`. Carried over from Phase 4.1.
2. **Bot-to-bot whisper handling** — Phase 5 explicitly excludes bot-to-bot from T3 in production (only humans + admin command), but `OnWhisperReceived` is called for both. Confirm the bot-to-bot guard is correctly placed BEFORE any new Phase 5 buffer push.
3. **Real-player demo** still unblocked from Phase 4 plan but never attempted.
