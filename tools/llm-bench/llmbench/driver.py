"""Async request driver — one fixture per call, returns a ResultRow."""

import asyncio
import json
import time
from datetime import datetime, timezone
from typing import Any

import httpx

from llmbench.output import ResultRow
from llmbench.validator import validate_response


async def drive_once(
    *,
    client: httpx.AsyncClient,
    endpoint: str,
    model: str,
    grammar: str,
    slots: int,
    fixture_name: str,
    phase: str,
    request_body: dict[str, Any],
    schema: dict[str, Any],
    timeout_s: float,
) -> ResultRow:
    """Send one chat-completion, capture timings, validate response."""
    ts = datetime.now(timezone.utc).isoformat()
    wall_start = time.perf_counter()
    body_bytes = len(json.dumps(request_body).encode("utf-8"))
    try:
        resp = await client.post(
            f"{endpoint}/v1/chat/completions",
            json=request_body,
            timeout=timeout_s,
        )
        wall_ms = int((time.perf_counter() - wall_start) * 1000)
        if resp.status_code != 200:
            return ResultRow(
                timestamp=ts,
                model=model,
                grammar=grammar,
                slots=slots,
                fixture=fixture_name,
                phase=phase,
                request_body_bytes=body_bytes,
                response_status=resp.status_code,
                prompt_eval_count=0,
                eval_count=0,
                prompt_eval_duration_ns=0,
                eval_duration_ns=0,
                wall_clock_ms=wall_ms,
                grammar_valid=False,
                parse_error=f"HTTP {resp.status_code}: {resp.text[:200]}",
                raw_response=resp.text,
            )
        data = resp.json()
        choice = data["choices"][0]
        content = choice["message"]["content"]
        timings = data.get("timings", {}) or {}
        prompt_eval_count = int(timings.get("prompt_n", 0) or data.get("usage", {}).get("prompt_tokens", 0))
        eval_count = int(timings.get("predicted_n", 0) or data.get("usage", {}).get("completion_tokens", 0))
        prompt_ms = float(timings.get("prompt_ms", 0))
        decode_ms = float(timings.get("predicted_ms", 0))
        prompt_dur_ns = int(prompt_ms * 1_000_000)
        decode_dur_ns = int(decode_ms * 1_000_000)
        valid, err = validate_response(content, schema)
        return ResultRow(
            timestamp=ts,
            model=model,
            grammar=grammar,
            slots=slots,
            fixture=fixture_name,
            phase=phase,
            request_body_bytes=body_bytes,
            response_status=200,
            prompt_eval_count=prompt_eval_count,
            eval_count=eval_count,
            prompt_eval_duration_ns=prompt_dur_ns,
            eval_duration_ns=decode_dur_ns,
            wall_clock_ms=wall_ms,
            grammar_valid=valid,
            parse_error=err,
            raw_response=content,
        )
    except (httpx.TimeoutException, httpx.RequestError) as e:
        wall_ms = int((time.perf_counter() - wall_start) * 1000)
        return ResultRow(
            timestamp=ts,
            model=model,
            grammar=grammar,
            slots=slots,
            fixture=fixture_name,
            phase=phase,
            request_body_bytes=body_bytes,
            response_status=0,
            prompt_eval_count=0,
            eval_count=0,
            prompt_eval_duration_ns=0,
            eval_duration_ns=0,
            wall_clock_ms=wall_ms,
            grammar_valid=False,
            parse_error=f"{type(e).__name__}: {e}",
            raw_response="",
        )


async def run_steady_state(
    *,
    client: httpx.AsyncClient,
    endpoint: str,
    model: str,
    grammar: str,
    slots: int,
    phase_label: str,
    fixtures: list[tuple[str, dict[str, Any]]],
    request_builder,  # callable(fixture_dict) -> request_body
    schema: dict[str, Any],
    target_rps: float,
    duration_s: float,
    timeout_s: float,
) -> list[ResultRow]:
    """Drive at a target RPS for duration_s, cycling fixtures round-robin."""
    interval = 1.0 / target_rps
    rows: list[ResultRow] = []
    tasks: list[asyncio.Task[ResultRow]] = []
    deadline = asyncio.get_event_loop().time() + duration_s
    i = 0
    while asyncio.get_event_loop().time() < deadline:
        name, fixture = fixtures[i % len(fixtures)]
        body = request_builder(fixture)
        tasks.append(
            asyncio.create_task(
                drive_once(
                    client=client,
                    endpoint=endpoint,
                    model=model,
                    grammar=grammar,
                    slots=slots,
                    fixture_name=name,
                    phase=phase_label,
                    request_body=body,
                    schema=schema,
                    timeout_s=timeout_s,
                )
            )
        )
        i += 1
        await asyncio.sleep(interval)
    rows.extend(await asyncio.gather(*tasks))
    return rows


async def run_burst(
    *,
    client: httpx.AsyncClient,
    endpoint: str,
    model: str,
    grammar: str,
    slots: int,
    fixtures: list[tuple[str, dict[str, Any]]],
    request_builder,
    schema: dict[str, Any],
    n_requests: int,
    concurrency: int,
    timeout_s: float,
) -> list[ResultRow]:
    """Send n_requests as fast as a semaphore of size `concurrency` allows."""
    semaphore = asyncio.Semaphore(concurrency)

    async def one(idx: int) -> ResultRow:
        name, fixture = fixtures[idx % len(fixtures)]
        body = request_builder(fixture)
        async with semaphore:
            return await drive_once(
                client=client,
                endpoint=endpoint,
                model=model,
                grammar=grammar,
                slots=slots,
                fixture_name=name,
                phase="burst",
                request_body=body,
                schema=schema,
                timeout_s=timeout_s,
            )

    return await asyncio.gather(*[one(i) for i in range(n_requests)])
