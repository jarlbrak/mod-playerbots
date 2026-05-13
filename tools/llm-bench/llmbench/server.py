"""Control the llama-server Quadlets on Heimdal (Vulkan + ROCm)."""

import asyncio
import subprocess
import time
from pathlib import Path

import httpx


SERVICES: dict[str, dict[str, str]] = {
    "vulkan": {
        "service_name": "llama-server.service",
        "env_file_dest": "/etc/llama-server.env",
        "env_file_stage": "/tmp/llama-server.env.next",
        "opposite": "rocm",
    },
    "rocm": {
        "service_name": "llama-server-rocm.service",
        "env_file_dest": "/etc/llama-server-rocm.env",
        "env_file_stage": "/tmp/llama-server-rocm.env.next",
        "opposite": "vulkan",
    },
}


def _backend(backend: str) -> dict[str, str]:
    if backend not in SERVICES:
        raise ValueError(f"unknown backend: {backend!r} (expected one of {list(SERVICES)})")
    return SERVICES[backend]


def write_env_and_restart(model_filename: str, slots: int, *, backend: str = "vulkan") -> None:
    """Stop the opposite backend, rewrite env file, restart this backend's service.

    Both Quadlets bind 127.0.0.1:8080. The pre-flight stop is idempotent
    (systemctl stop on an inactive unit returns 0).

    Preserves any non-LLAMA_* lines from the on-disk env file (e.g. ROCm's
    HSA_OVERRIDE_GFX_VERSION), so a harness rewrite doesn't lose static
    backend-specific config.
    """
    cfg = _backend(backend)
    opposite_cfg = _backend(cfg["opposite"])

    # Pre-flight: stop the opposite-backend service (port-conflict guard).
    subprocess.run(
        ["sudo", "-n", "/usr/bin/systemctl", "stop", opposite_cfg["service_name"]],
        check=False,
    )

    # Read existing env-file (if any), preserve non-LLAMA_* lines.
    preserved: list[str] = []
    dest = Path(cfg["env_file_dest"])
    if dest.exists():
        for line in dest.read_text().splitlines():
            stripped = line.strip()
            if not stripped or stripped.startswith("#"):
                continue
            if stripped.startswith("LLAMA_MODEL=") or stripped.startswith("LLAMA_SLOTS="):
                continue
            preserved.append(stripped)

    body_lines = [f"LLAMA_MODEL={model_filename}", f"LLAMA_SLOTS={slots}"] + preserved
    Path(cfg["env_file_stage"]).write_text("\n".join(body_lines) + "\n")

    subprocess.run(
        ["sudo", "-n", "/usr/bin/install", "-m", "0644", cfg["env_file_stage"], cfg["env_file_dest"]],
        check=True,
    )
    subprocess.run(
        ["sudo", "-n", "/usr/bin/systemctl", "restart", cfg["service_name"]],
        check=True,
    )


async def wait_for_health(
    endpoint: str = "http://127.0.0.1:8080",
    timeout_s: float = 300.0,
    poll_interval_s: float = 2.0,
) -> None:
    """Poll /health until 200 or timeout.

    Default timeout raised from 180 s to 300 s — ROCm init is slower than
    Vulkan and the first request often blocks on JIT kernel compile.
    """
    deadline = time.monotonic() + timeout_s
    async with httpx.AsyncClient() as client:
        while time.monotonic() < deadline:
            try:
                resp = await client.get(f"{endpoint}/health", timeout=5.0)
                if resp.status_code == 200:
                    return
            except httpx.RequestError:
                pass
            await asyncio.sleep(poll_interval_s)
    raise TimeoutError(
        f"llama-server health check did not return 200 within {timeout_s}s"
    )


def stop_server(*, backend: str = "vulkan") -> None:
    """Stop the named backend's service (best-effort, no exception on failure)."""
    cfg = _backend(backend)
    subprocess.run(
        ["sudo", "-n", "/usr/bin/systemctl", "stop", cfg["service_name"]],
        check=False,
    )
