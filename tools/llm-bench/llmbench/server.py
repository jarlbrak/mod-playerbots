"""Control the llama-server Quadlet on Heimdal."""

import asyncio
import subprocess
import time
from pathlib import Path

import httpx


SERVICE_NAME = "llama-server.service"
ENV_FILE_DEST = "/etc/llama-server.env"
ENV_FILE_STAGE = "/tmp/llama-server.env.next"


def write_env_and_restart(model_filename: str, slots: int) -> None:
    """Rewrite /etc/llama-server.env atomically via sudo install, then restart."""
    Path(ENV_FILE_STAGE).write_text(
        f"LLAMA_MODEL={model_filename}\nLLAMA_SLOTS={slots}\n"
    )
    subprocess.run(
        ["sudo", "-n", "/usr/bin/install", "-m", "0644", ENV_FILE_STAGE, ENV_FILE_DEST],
        check=True,
    )
    subprocess.run(["sudo", "-n", "/usr/bin/systemctl", "restart", SERVICE_NAME], check=True)


async def wait_for_health(
    endpoint: str = "http://127.0.0.1:8080",
    timeout_s: float = 180.0,
    poll_interval_s: float = 2.0,
) -> None:
    """Poll /health until 200 or timeout."""
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


def stop_server() -> None:
    """Stop the service (best-effort)."""
    subprocess.run(["sudo", "-n", "/usr/bin/systemctl", "stop", SERVICE_NAME], check=False)
