# Phase 0.5 — ROCm Comparison Pass Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Run a single-cell head-to-head on Heimdal — Qwen 2.5 7B + json_schema + slots=4 on `ghcr.io/ggml-org/llama.cpp:server-rocm` vs the existing Vulkan baseline at `results/2026-05-12-vulkan/`, then fold the decision into a new §15.8 of the parent design doc.

**Architecture:** A sibling Quadlet (`llama-server-rocm.container`) coexists with the Vulkan one; both bind `127.0.0.1:8080` so the harness stops one before starting the other. `bench.py` gains two flags: `--rocm` (selects backend) and `--single-cell` (filters the matrix to Qwen / slots=4 / json_schema). `server.py` gains a `backend` parameter that routes to the right service name and env-file path. Same thermal mitigations from the Vulkan rerun (gpu-fan-max + 237 W power cap + sysfs temp sampling).

**Tech Stack:** Podman Quadlet (rootful), `ghcr.io/ggml-org/llama.cpp:server-rocm`, ROCm 6.x on Bazzite, Python harness from Phase 0.5 (`tools/llm-bench/`).

---

## Preliminaries

Same as Phase 0.5: work on the Mac at `~/Documents/Projects/playerbots-dev/mod-playerbots/`, deploy/run on Heimdal at `192.168.1.3` via:

```bash
BAZPASS=$(security find-internet-password -s '192.168.1.3' -a 'brackin' -w)
sshpass -p "$BAZPASS" ssh -o StrictHostKeyChecking=no brackin@192.168.1.3 '<command>'
```

Branch: `claude/local-llm-bot-agents-O72i5` (continues from Phase 0.5; do not branch off).

The Heimdal clone is at `~/llm-bench-workspace/mod-playerbots/`. `gpu-fan-max.service` is already enabled and active from Phase 0.5 — do not touch.

---

## Task 1: ROCm Quadlet + env-file + sudoers update

**Files:**
- Create: `infra/heimdal/llama-server-rocm.container`
- Create: `infra/heimdal/llama-server-rocm.env`
- Modify: `infra/heimdal/sudoers-llama-server` (add 3 systemctl lines + 1 install line)
- Modify: `infra/heimdal/README.md` (add ROCm deploy section)

- [ ] **Step 1: Author the ROCm Quadlet unit on the Mac**

Create `infra/heimdal/llama-server-rocm.container`:

```ini
[Unit]
Description=llama-server (ROCm) for Phase 0.5 LLM-agent ROCm comparison
Wants=network-online.target
After=network-online.target

[Container]
Image=ghcr.io/ggml-org/llama.cpp:server-rocm
PublishPort=127.0.0.1:8080:8080
Volume=/var/lib/llama-models:/models:Z,ro
EnvironmentFile=/etc/llama-server-rocm.env
AddDevice=/dev/kfd
AddDevice=/dev/dri/renderD128
# Numeric GIDs for host render (105) and video (39). See infra/heimdal/README.md
# under Quadlet quirks for why this is numeric rather than named.
GroupAdd=105
GroupAdd=39
Exec=--host 0.0.0.0 --port 8080 --model /models/${LLAMA_MODEL} --parallel ${LLAMA_SLOTS} --n-gpu-layers 999 --ctx-size 8192 --batch-size 512 --no-mmap

[Service]
# Load env-file at unit level too so ${LLAMA_MODEL}/${LLAMA_SLOTS} in Exec= get
# substituted. Same pattern as the Vulkan unit.
EnvironmentFile=/etc/llama-server-rocm.env
Restart=on-failure
RestartSec=5
# ROCm initialization is slower than Vulkan; 5 min is defensive.
TimeoutStartSec=300

[Install]
WantedBy=default.target
```

- [ ] **Step 2: Author env-file template on the Mac**

Create `infra/heimdal/llama-server-rocm.env`:

```
LLAMA_MODEL=qwen2.5-7b-instruct-q4_k_m-00001-of-00002.gguf
LLAMA_SLOTS=4
HSA_OVERRIDE_GFX_VERSION=10.3.0
```

The `HSA_OVERRIDE_GFX_VERSION=10.3.0` defensively forces the runtime to treat the GPU as gfx1030 (Navi 21, the RX 6900 XT). A no-op on modern ROCm images that detect the card correctly; a fix on older images. Free insurance.

- [ ] **Step 3: Extend sudoers drop-in on the Mac**

Modify `infra/heimdal/sudoers-llama-server` — append four new lines after the existing entries (keep the existing block intact):

```
# ROCm sibling service for Phase 0.5 comparison pass.
brackin ALL=(root) NOPASSWD: /usr/bin/systemctl restart llama-server-rocm.service
brackin ALL=(root) NOPASSWD: /usr/bin/systemctl start llama-server-rocm.service
brackin ALL=(root) NOPASSWD: /usr/bin/systemctl stop llama-server-rocm.service
brackin ALL=(root) NOPASSWD: /usr/bin/install -m 0644 /tmp/llama-server-rocm.env.next /etc/llama-server-rocm.env
```

After editing, the full file is `infra/heimdal/sudoers-llama-server`. Verify locally:

```bash
cat ~/Documents/Projects/playerbots-dev/mod-playerbots/infra/heimdal/sudoers-llama-server
```

Expected: 11 active lines (3 prior systemctl + 1 prior install + 1 power_cap + 4 new = 9 NOPASSWD + 2 comment block headers, plus whitespace).

- [ ] **Step 4: Add ROCm deploy section to the README on the Mac**

Append to the end of `infra/heimdal/README.md`:

```markdown

ROCm sibling service (Phase 0.5 ROCm comparison pass):

```bash
BAZPASS=$(security find-internet-password -s '192.168.1.3' -a 'brackin' -w)
ALIAS="sshpass -p $BAZPASS ssh -o StrictHostKeyChecking=no brackin@192.168.1.3"

sshpass -p "$BAZPASS" scp -o StrictHostKeyChecking=no \
  infra/heimdal/llama-server-rocm.container \
  infra/heimdal/llama-server-rocm.env \
  infra/heimdal/sudoers-llama-server \
  brackin@192.168.1.3:/tmp/

$ALIAS 'sudo install -m 0644 /tmp/llama-server-rocm.container /etc/containers/systemd/llama-server-rocm.container'
$ALIAS 'sudo install -m 0644 /tmp/llama-server-rocm.env       /etc/llama-server-rocm.env'
$ALIAS 'sudo install -m 0440 /tmp/sudoers-llama-server        /etc/sudoers.d/llama-server'
$ALIAS 'sudo visudo -c -f /etc/sudoers.d/llama-server'
$ALIAS 'sudo systemctl daemon-reload'
# First pull the ROCm image (~8 GB; takes 1-3 min):
$ALIAS 'podman pull ghcr.io/ggml-org/llama.cpp:server-rocm'
# Vulkan and ROCm services both bind 127.0.0.1:8080 — stop one before starting the other.
$ALIAS 'sudo systemctl stop llama-server.service; sudo systemctl start llama-server-rocm.service'
$ALIAS 'curl -s http://127.0.0.1:8080/health'
```
```

- [ ] **Step 5: Deploy to Heimdal**

From the Mac:

```bash
cd ~/Documents/Projects/playerbots-dev/mod-playerbots
BAZPASS=$(security find-internet-password -s '192.168.1.3' -a 'brackin' -w)
sshpass -p "$BAZPASS" scp -o StrictHostKeyChecking=no \
  infra/heimdal/llama-server-rocm.container \
  infra/heimdal/llama-server-rocm.env \
  infra/heimdal/sudoers-llama-server \
  brackin@192.168.1.3:/tmp/
sshpass -p "$BAZPASS" ssh -o StrictHostKeyChecking=no brackin@192.168.1.3 "
  echo '$BAZPASS' | sudo -S install -m 0644 /tmp/llama-server-rocm.container /etc/containers/systemd/llama-server-rocm.container &&
  echo '$BAZPASS' | sudo -S install -m 0644 /tmp/llama-server-rocm.env /etc/llama-server-rocm.env &&
  echo '$BAZPASS' | sudo -S install -m 0440 /tmp/sudoers-llama-server /etc/sudoers.d/llama-server &&
  echo '$BAZPASS' | sudo -S visudo -c -f /etc/sudoers.d/llama-server &&
  echo '$BAZPASS' | sudo -S systemctl daemon-reload &&
  echo 'DEPLOY OK'
"
```

Expected: `parsed OK` from visudo, then `DEPLOY OK`.

- [ ] **Step 6: Verify new sudoers entries work**

```bash
BAZPASS=$(security find-internet-password -s '192.168.1.3' -a 'brackin' -w)
sshpass -p "$BAZPASS" ssh -o StrictHostKeyChecking=no brackin@192.168.1.3 "
  echo 'LLAMA_MODEL=qwen2.5-7b-instruct-q4_k_m-00001-of-00002.gguf' > /tmp/llama-server-rocm.env.next
  echo 'LLAMA_SLOTS=4' >> /tmp/llama-server-rocm.env.next
  echo 'HSA_OVERRIDE_GFX_VERSION=10.3.0' >> /tmp/llama-server-rocm.env.next
  sudo -n install -m 0644 /tmp/llama-server-rocm.env.next /etc/llama-server-rocm.env && echo INSTALL_ROCM_ENV_OK
  sudo -n systemctl stop llama-server-rocm.service 2>&1; echo STOP_EXIT=\$?
"
```

Expected: `INSTALL_ROCM_ENV_OK`, then `STOP_EXIT=0` (the service isn't running yet, but `systemctl stop` on an inactive unit returns 0).

- [ ] **Step 7: Commit the infra files**

```bash
cd ~/Documents/Projects/playerbots-dev/mod-playerbots
git add infra/heimdal/llama-server-rocm.container infra/heimdal/llama-server-rocm.env infra/heimdal/sudoers-llama-server infra/heimdal/README.md
git commit -m "$(cat <<'EOF'
chore(infra): add ROCm sibling Quadlet for Phase 0.5 comparison pass

llama-server-rocm.container sits next to the Vulkan unit; both bind
127.0.0.1:8080 so the harness will stop one before starting the other.
Uses ghcr.io/ggml-org/llama.cpp:server-rocm, /dev/kfd + /dev/dri/renderD128
passthrough, same render/video GIDs as the Vulkan unit. Defensive
HSA_OVERRIDE_GFX_VERSION=10.3.0 in the env file (no-op when ROCm
already detects gfx1030 correctly). TimeoutStartSec=300 because ROCm
init is slower than Vulkan. Sudoers extended with 4 NOPASSWD entries
covering the new service + its env-file install path.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Server controller backend dispatch

**Files:**
- Modify: `tools/llm-bench/llmbench/server.py`
- Create: `tools/llm-bench/tests/test_server.py`

The current `server.py` is hard-coded to the Vulkan service. Add a `backend` parameter to route between Vulkan and ROCm, plus a pre-flight stop of the opposite backend (port-conflict guard).

- [ ] **Step 1: Write the failing tests**

Create `tools/llm-bench/tests/test_server.py`:

```python
from unittest import mock

import pytest

from llmbench.server import (
    SERVICES,
    write_env_and_restart,
    stop_server,
)


def test_services_registry_has_both_backends():
    assert "vulkan" in SERVICES
    assert "rocm" in SERVICES
    assert SERVICES["vulkan"]["service_name"] == "llama-server.service"
    assert SERVICES["rocm"]["service_name"] == "llama-server-rocm.service"
    assert SERVICES["vulkan"]["env_file_dest"] == "/etc/llama-server.env"
    assert SERVICES["rocm"]["env_file_dest"] == "/etc/llama-server-rocm.env"


def test_write_env_and_restart_vulkan_default(tmp_path, monkeypatch):
    stage = tmp_path / "stage.env"
    monkeypatch.setattr("llmbench.server.SERVICES", {
        "vulkan": {"service_name": "llama-server.service", "env_file_dest": "/etc/llama-server.env", "env_file_stage": str(stage), "opposite": "rocm"},
        "rocm":   {"service_name": "llama-server-rocm.service", "env_file_dest": "/etc/llama-server-rocm.env", "env_file_stage": "/tmp/rocm.env.next", "opposite": "vulkan"},
    })
    with mock.patch("subprocess.run") as m:
        write_env_and_restart("gemma-2-9b-it-Q4_K_M.gguf", 4)
    # Three subprocess.run calls expected: stop opposite (rocm), install env, restart vulkan.
    assert m.call_count == 3
    stop_call, install_call, restart_call = m.call_args_list
    assert stop_call.args[0] == ["sudo", "-n", "/usr/bin/systemctl", "stop", "llama-server-rocm.service"]
    assert install_call.args[0] == ["sudo", "-n", "/usr/bin/install", "-m", "0644", str(stage), "/etc/llama-server.env"]
    assert restart_call.args[0] == ["sudo", "-n", "/usr/bin/systemctl", "restart", "llama-server.service"]
    # Env-file contents on disk
    assert stage.read_text() == "LLAMA_MODEL=gemma-2-9b-it-Q4_K_M.gguf\nLLAMA_SLOTS=4\n"


def test_write_env_and_restart_rocm_explicit(tmp_path, monkeypatch):
    stage = tmp_path / "rocm-stage.env"
    monkeypatch.setattr("llmbench.server.SERVICES", {
        "vulkan": {"service_name": "llama-server.service", "env_file_dest": "/etc/llama-server.env", "env_file_stage": "/tmp/vulkan.env.next", "opposite": "rocm"},
        "rocm":   {"service_name": "llama-server-rocm.service", "env_file_dest": "/etc/llama-server-rocm.env", "env_file_stage": str(stage), "opposite": "vulkan"},
    })
    with mock.patch("subprocess.run") as m:
        write_env_and_restart("qwen2.5-7b-instruct-q4_k_m-00001-of-00002.gguf", 4, backend="rocm")
    assert m.call_count == 3
    stop_call, install_call, restart_call = m.call_args_list
    # Opposite of rocm is vulkan
    assert stop_call.args[0] == ["sudo", "-n", "/usr/bin/systemctl", "stop", "llama-server.service"]
    assert install_call.args[0] == ["sudo", "-n", "/usr/bin/install", "-m", "0644", str(stage), "/etc/llama-server-rocm.env"]
    assert restart_call.args[0] == ["sudo", "-n", "/usr/bin/systemctl", "restart", "llama-server-rocm.service"]
    # ROCm env file gets the HSA override appended; the stage file should preserve any extra config
    # the harness chooses to add. For this minimal contract, we only assert the two required lines.
    content = stage.read_text()
    assert "LLAMA_MODEL=qwen2.5-7b-instruct-q4_k_m-00001-of-00002.gguf" in content
    assert "LLAMA_SLOTS=4" in content


def test_write_env_and_restart_unknown_backend_raises():
    with pytest.raises(ValueError):
        write_env_and_restart("x.gguf", 4, backend="cuda")


def test_stop_server_vulkan_default():
    with mock.patch("subprocess.run") as m:
        stop_server()
    assert m.call_args.args[0] == ["sudo", "-n", "/usr/bin/systemctl", "stop", "llama-server.service"]


def test_stop_server_rocm():
    with mock.patch("subprocess.run") as m:
        stop_server(backend="rocm")
    assert m.call_args.args[0] == ["sudo", "-n", "/usr/bin/systemctl", "stop", "llama-server-rocm.service"]
```

- [ ] **Step 2: Run, verify the tests fail**

```bash
cd ~/Documents/Projects/playerbots-dev/mod-playerbots/tools/llm-bench
uv run --extra dev pytest tests/test_server.py -v
```

Expected: `ImportError` on `SERVICES`, or `TypeError` on the `backend=` keyword.

- [ ] **Step 3: Rewrite `server.py` with the registry + dispatch**

Replace `tools/llm-bench/llmbench/server.py` entirely:

```python
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
    """
    cfg = _backend(backend)
    opposite_cfg = _backend(cfg["opposite"])

    # Pre-flight: stop the opposite-backend service (port-conflict guard).
    subprocess.run(
        ["sudo", "-n", "/usr/bin/systemctl", "stop", opposite_cfg["service_name"]],
        check=False,
    )

    # Stage + install env-file.
    Path(cfg["env_file_stage"]).write_text(
        f"LLAMA_MODEL={model_filename}\nLLAMA_SLOTS={slots}\n"
    )
    subprocess.run(
        ["sudo", "-n", "/usr/bin/install", "-m", "0644", cfg["env_file_stage"], cfg["env_file_dest"]],
        check=True,
    )

    # Restart this backend's service.
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
```

Note: ROCm-specific env vars (`HSA_OVERRIDE_GFX_VERSION`) are not written by the harness — they live in the on-disk `/etc/llama-server-rocm.env` deployed in Task 1 step 2. The harness's `LLAMA_MODEL` + `LLAMA_SLOTS` rewrite would overwrite that file, losing the HSA line. **The harness must preserve existing non-`LLAMA_*` lines on rewrite.**

Update `write_env_and_restart` to preserve those lines. Replace its body with:

```python
def write_env_and_restart(model_filename: str, slots: int, *, backend: str = "vulkan") -> None:
    """Stop the opposite backend, rewrite env file, restart this backend's service.

    Preserves any non-LLAMA_* lines from the on-disk env file (e.g. ROCm's
    HSA_OVERRIDE_GFX_VERSION), so a harness rewrite doesn't lose static
    backend-specific config.
    """
    cfg = _backend(backend)
    opposite_cfg = _backend(cfg["opposite"])

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
```

- [ ] **Step 4: Add a test for env-file preservation**

Append to `tools/llm-bench/tests/test_server.py`:

```python
def test_write_env_preserves_non_llama_lines(tmp_path, monkeypatch):
    """ROCm's HSA_OVERRIDE_GFX_VERSION (and any other non-LLAMA_ line) must
    survive a harness rewrite."""
    stage = tmp_path / "stage.env"
    dest = tmp_path / "dest.env"
    dest.write_text(
        "LLAMA_MODEL=old.gguf\n"
        "LLAMA_SLOTS=1\n"
        "HSA_OVERRIDE_GFX_VERSION=10.3.0\n"
        "# a comment\n"
        "\n"
    )
    monkeypatch.setattr("llmbench.server.SERVICES", {
        "vulkan": {"service_name": "llama-server.service", "env_file_dest": "/dev/null", "env_file_stage": "/tmp/v.env", "opposite": "rocm"},
        "rocm":   {"service_name": "llama-server-rocm.service", "env_file_dest": str(dest), "env_file_stage": str(stage), "opposite": "vulkan"},
    })
    with mock.patch("subprocess.run"):
        write_env_and_restart("new.gguf", 4, backend="rocm")

    body = stage.read_text()
    lines = [l for l in body.splitlines() if l.strip()]
    assert lines[0] == "LLAMA_MODEL=new.gguf"
    assert lines[1] == "LLAMA_SLOTS=4"
    assert "HSA_OVERRIDE_GFX_VERSION=10.3.0" in lines
    # No leftover stale LLAMA_* lines from the existing dest
    assert "LLAMA_MODEL=old.gguf" not in body
    assert "LLAMA_SLOTS=1" not in body
```

- [ ] **Step 5: Run all tests; verify the new tests pass and existing tests don't regress**

```bash
cd ~/Documents/Projects/playerbots-dev/mod-playerbots/tools/llm-bench
uv run --extra dev pytest -v
```

Expected: **42 passed** (35 prior + 7 new). The 7 new tests are:
- `test_services_registry_has_both_backends`
- `test_write_env_and_restart_vulkan_default`
- `test_write_env_and_restart_rocm_explicit`
- `test_write_env_and_restart_unknown_backend_raises`
- `test_stop_server_vulkan_default`
- `test_stop_server_rocm`
- `test_write_env_preserves_non_llama_lines` (from step 4 of this task)

- [ ] **Step 6: Commit**

```bash
cd ~/Documents/Projects/playerbots-dev/mod-playerbots
git add tools/llm-bench/llmbench/server.py tools/llm-bench/tests/test_server.py
git commit -m "$(cat <<'EOF'
feat(llm-bench): server controller backend dispatch + env-file preservation

server.py gains a SERVICES registry keyed on "vulkan" / "rocm", and a
backend kwarg on write_env_and_restart + stop_server. write_env_and_restart
now pre-flights a stop on the opposite-backend service (port-conflict
guard since both Quadlets bind 127.0.0.1:8080), and preserves any
non-LLAMA_* lines in the env file (so ROCm's HSA_OVERRIDE_GFX_VERSION
survives a harness rewrite). wait_for_health default timeout bumped
180 -> 300 s for ROCm init.

Seven new tests cover the registry, both-backend dispatch, the
unknown-backend error, stop_server dispatch, and env-file preservation.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: `--rocm` and `--single-cell` flags on bench.py

**Files:**
- Modify: `tools/llm-bench/bench.py`

- [ ] **Step 1: Read the current bench.py to confirm line numbers**

```bash
grep -n "GRAMMAR_MODES\|PARALLEL_SLOTS\|MODELS = " ~/Documents/Projects/playerbots-dev/mod-playerbots/tools/llm-bench/bench.py
```

Expected: the three list constants near the top of the file. They're the filter targets for `--single-cell`.

- [ ] **Step 2: Add the two flags + plumb backend through run_matrix**

Modify `tools/llm-bench/bench.py`. The change is in three places:

(a) Add a `backend` parameter to `run_matrix`, default `"vulkan"`. At the top of the function body, pass `backend=backend` into `write_env_and_restart` and `stop_server`. Find this block:

```python
                    write_env_and_restart(model.gguf_filename, slots)
                    await wait_for_health(endpoint)
```

Replace with:

```python
                    write_env_and_restart(model.gguf_filename, slots, backend=backend)
                    await wait_for_health(endpoint)
```

(b) Add backend to `run_metadata` (in the same function body, where the dict is built):

Find:

```python
    run_metadata = {
        "date": date.today().isoformat(),
        "backend": "vulkan",
```

Replace `"backend": "vulkan",` with `"backend": backend,`.

(c) Add `--rocm` + `--single-cell` flags + thread them in. Find the click decorators block:

```python
@click.command()
@click.option("--endpoint", default="http://127.0.0.1:8080")
@click.option("--fixtures-dir", default="fixtures", type=click.Path(exists=True, path_type=Path))
@click.option("--schema-path", default="schemas/goal_schema.json", type=click.Path(exists=True, path_type=Path))
@click.option("--gbnf-path", default="grammars/goal.gbnf", type=click.Path(exists=True, path_type=Path))
@click.option("--out-dir", required=True, type=click.Path(path_type=Path))
@click.option("--dry-run", is_flag=True, help="Smoke test: 1 model, 4 slots, json_schema only, 10 s steady, 5 burst, 5 s cooling.")
@click.option("--stop-server-on-exit", is_flag=True, help="Stop llama-server after the run.")
def main(
    endpoint: str,
    fixtures_dir: Path,
    schema_path: Path,
    gbnf_path: Path,
    out_dir: Path,
    dry_run: bool,
    stop_server_on_exit: bool,
) -> None:
    """Run the Phase 0.5 characterization matrix."""
    try:
        asyncio.run(
            run_matrix(
                endpoint=endpoint,
                fixtures_dir=fixtures_dir,
                schema_path=schema_path,
                gbnf_path=gbnf_path,
                out_dir=out_dir,
                dry_run=dry_run,
            )
        )
    finally:
        if stop_server_on_exit:
            stop_server()
```

Replace with:

```python
@click.command()
@click.option("--endpoint", default="http://127.0.0.1:8080")
@click.option("--fixtures-dir", default="fixtures", type=click.Path(exists=True, path_type=Path))
@click.option("--schema-path", default="schemas/goal_schema.json", type=click.Path(exists=True, path_type=Path))
@click.option("--gbnf-path", default="grammars/goal.gbnf", type=click.Path(exists=True, path_type=Path))
@click.option("--out-dir", required=True, type=click.Path(path_type=Path))
@click.option("--dry-run", is_flag=True, help="Smoke test: 1 model, 4 slots, json_schema only, 10 s steady, 5 burst, 5 s cooling.")
@click.option("--single-cell", is_flag=True, help="Run only the production cell (Qwen 2.5 7B + slots=4 + json_schema). Mutually exclusive with --dry-run.")
@click.option("--rocm", is_flag=True, help="Target the ROCm Quadlet (llama-server-rocm.service) instead of Vulkan.")
@click.option("--stop-server-on-exit", is_flag=True, help="Stop llama-server (the active backend) after the run.")
def main(
    endpoint: str,
    fixtures_dir: Path,
    schema_path: Path,
    gbnf_path: Path,
    out_dir: Path,
    dry_run: bool,
    single_cell: bool,
    rocm: bool,
    stop_server_on_exit: bool,
) -> None:
    """Run the Phase 0.5 characterization matrix."""
    if dry_run and single_cell:
        raise click.UsageError("--dry-run and --single-cell are mutually exclusive")
    backend = "rocm" if rocm else "vulkan"
    try:
        asyncio.run(
            run_matrix(
                endpoint=endpoint,
                fixtures_dir=fixtures_dir,
                schema_path=schema_path,
                gbnf_path=gbnf_path,
                out_dir=out_dir,
                dry_run=dry_run,
                single_cell=single_cell,
                backend=backend,
            )
        )
    finally:
        if stop_server_on_exit:
            stop_server(backend=backend)
```

(d) Update `run_matrix` signature + filter logic. Find:

```python
async def run_matrix(
    *,
    endpoint: str,
    fixtures_dir: Path,
    schema_path: Path,
    gbnf_path: Path,
    out_dir: Path,
    dry_run: bool,
) -> None:
    fixtures = load_fixtures(fixtures_dir)
    schema = load_schema(schema_path)
    gbnf = load_gbnf(gbnf_path)

    all_rows: list[ResultRow] = []
    summaries: list[CellSummary] = []

    models_to_run = MODELS[:1] if dry_run else MODELS
    slots_to_run = [4] if dry_run else PARALLEL_SLOTS
    grammars_to_run = ["json_schema"] if dry_run else GRAMMAR_MODES
    steady_duration = 10.0 if dry_run else STEADY_DURATION_S
    burst_n = 5 if dry_run else BURST_REQUESTS
    cooling_s = 5.0 if dry_run else INTER_CELL_COOLING_S
```

Replace with:

```python
async def run_matrix(
    *,
    endpoint: str,
    fixtures_dir: Path,
    schema_path: Path,
    gbnf_path: Path,
    out_dir: Path,
    dry_run: bool,
    single_cell: bool = False,
    backend: str = "vulkan",
) -> None:
    fixtures = load_fixtures(fixtures_dir)
    schema = load_schema(schema_path)
    gbnf = load_gbnf(gbnf_path)

    all_rows: list[ResultRow] = []
    summaries: list[CellSummary] = []

    if single_cell:
        # Production cell: Qwen 2.5 7B + slots=4 + json_schema. The Phase 0.5
        # winner; used for the ROCm comparison pass.
        models_to_run = [m for m in MODELS if m.label == "qwen-2.5-7b"]
        slots_to_run = [4]
        grammars_to_run = ["json_schema"]
        steady_duration = STEADY_DURATION_S
        burst_n = BURST_REQUESTS
        cooling_s = 0.0  # only one cell — no need for a cooling pause
    elif dry_run:
        models_to_run = MODELS[:1]
        slots_to_run = [4]
        grammars_to_run = ["json_schema"]
        steady_duration = 10.0
        burst_n = 5
        cooling_s = 5.0
    else:
        models_to_run = MODELS
        slots_to_run = PARALLEL_SLOTS
        grammars_to_run = GRAMMAR_MODES
        steady_duration = STEADY_DURATION_S
        burst_n = BURST_REQUESTS
        cooling_s = INTER_CELL_COOLING_S
```

(e) Also include `single_cell` and the chosen `backend` in `run_metadata`. Find:

```python
        "burst_requests": burst_n,
        "gpu_power_cap_watts": GPU_POWER_CAP_WATTS,
        "inter_cell_cooling_s": cooling_s,
        "dry_run": dry_run,
    }
```

Replace with:

```python
        "burst_requests": burst_n,
        "gpu_power_cap_watts": GPU_POWER_CAP_WATTS,
        "inter_cell_cooling_s": cooling_s,
        "dry_run": dry_run,
        "single_cell": single_cell,
    }
```

(`backend` was already changed in change (b).)

- [ ] **Step 3: Run all tests + verify CLI parses**

```bash
cd ~/Documents/Projects/playerbots-dev/mod-playerbots/tools/llm-bench
uv run --extra dev pytest -v 2>&1 | tail -5
uv run bench.py --help
```

Expected: 42 tests pass. `--help` lists `--rocm`, `--single-cell`, and the mutual-exclusion message in the `--single-cell` help text.

- [ ] **Step 4: Confirm mutual exclusion enforcement**

```bash
cd ~/Documents/Projects/playerbots-dev/mod-playerbots/tools/llm-bench
uv run bench.py --dry-run --single-cell --out-dir /tmp/x 2>&1 | head -5
```

Expected: a `click.UsageError` line containing `--dry-run and --single-cell are mutually exclusive`.

- [ ] **Step 5: Commit**

```bash
cd ~/Documents/Projects/playerbots-dev/mod-playerbots
git add tools/llm-bench/bench.py
git commit -m "$(cat <<'EOF'
feat(llm-bench): add --rocm and --single-cell flags to bench.py

--rocm selects backend="rocm" (targets the new llama-server-rocm.service
Quadlet); default is vulkan. --single-cell filters MODELS / PARALLEL_SLOTS /
GRAMMAR_MODES down to the production cell (Qwen 2.5 7B + slots=4 +
json_schema) — used for the ROCm comparison pass. --dry-run and
--single-cell are mutually exclusive. Backend + single_cell flow into
run_metadata so summary.md records the run shape.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: Pull ROCm image + smoke-test service on Heimdal

**Files:** None.

This is the highest-risk task — the ROCm image must actually run with GPU access. Failure modes (and what to do):
- Image pull fails → check network, retry.
- Service crashes on start → inspect `journalctl -u llama-server-rocm`. Common: missing `kfd` group or `HSA_OVERRIDE_GFX_VERSION` already set correctly. The env file in Task 1 step 2 already includes the defensive override.
- Service starts but no GPU detected (falls back to CPU) → check journalctl for "Vulkan device" / "ROCm device" / "Using CPU". If CPU-only, escalate.

- [ ] **Step 1: Pull the ROCm image on Heimdal**

```bash
BAZPASS=$(security find-internet-password -s '192.168.1.3' -a 'brackin' -w)
sshpass -p "$BAZPASS" ssh -o StrictHostKeyChecking=no brackin@192.168.1.3 \
  "podman pull ghcr.io/ggml-org/llama.cpp:server-rocm 2>&1 | tail -5"
```

Expected: a digest line + image ID. Pull time ~1-3 min depending on network.

- [ ] **Step 2: Stop the Vulkan service if running; start ROCm; poll /health**

```bash
BAZPASS=$(security find-internet-password -s '192.168.1.3' -a 'brackin' -w)
sshpass -p "$BAZPASS" ssh -o StrictHostKeyChecking=no brackin@192.168.1.3 "
  sudo -n /usr/bin/systemctl stop llama-server.service 2>&1
  sudo -n /usr/bin/systemctl start llama-server-rocm.service && echo START_OK
  for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25; do
    sleep 5
    code=\$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:8080/health)
    echo \"attempt \$i: HTTP \$code\"
    if [ \"\$code\" = '200' ]; then break; fi
  done
"
```

Expected: `START_OK` then HTTP 200 within ~125 s (ROCm initialization is slower than Vulkan; first start may trigger JIT kernel compilation).

If HTTP 200 doesn't arrive: stop the service, inspect logs, escalate.

- [ ] **Step 3: Confirm the GPU is actually being used**

```bash
BAZPASS=$(security find-internet-password -s '192.168.1.3' -a 'brackin' -w)
sshpass -p "$BAZPASS" ssh -o StrictHostKeyChecking=no brackin@192.168.1.3 \
  "journalctl -u llama-server-rocm.service -n 100 --no-pager 2>&1 | grep -iE 'rocm|hip|navi|gfx|cpu only|fallback|device' | head -20"
```

Expected: lines mentioning ROCm/HIP/gfx1030/Navi or a device count > 0. If only CPU lines appear, escalate.

- [ ] **Step 4: Send one trivial completion to confirm end-to-end inference works**

```bash
BAZPASS=$(security find-internet-password -s '192.168.1.3' -a 'brackin' -w)
sshpass -p "$BAZPASS" ssh -o StrictHostKeyChecking=no brackin@192.168.1.3 "
  curl -s http://127.0.0.1:8080/v1/chat/completions \
    -H 'Content-Type: application/json' \
    -d '{\"messages\":[{\"role\":\"user\",\"content\":\"Say hello in one word.\"}],\"max_tokens\":16,\"temperature\":0}' \
    | python3 -m json.tool
"
```

Expected: non-empty `content` in `choices[0].message.content`, plus a `timings` block with `predicted_per_second` > 20 tok/s (sanity-check that GPU is actually working). If `predicted_per_second` is < 5 tok/s, the GPU isn't engaged — escalate.

- [ ] **Step 5: Stop the ROCm service**

```bash
BAZPASS=$(security find-internet-password -s '192.168.1.3' -a 'brackin' -w)
sshpass -p "$BAZPASS" ssh -o StrictHostKeyChecking=no brackin@192.168.1.3 \
  "sudo -n /usr/bin/systemctl stop llama-server-rocm.service && echo STOPPED"
```

Expected: `STOPPED`.

- [ ] **Step 6: No commit needed; proceed to Task 5**

---

## Task 5: Push, pull on Heimdal, run the single-cell bench

**Files:** Produces `results/YYYY-MM-DD-rocm/{results.csv, summary.md}` on disk.

- [ ] **Step 1: Push the branch from Mac**

```bash
cd ~/Documents/Projects/playerbots-dev/mod-playerbots
git push origin claude/local-llm-bot-agents-O72i5
```

Expected: 4 new commits pushed (Tasks 1-3 plus this plan).

- [ ] **Step 2: Pull on Heimdal + reinstall deps**

```bash
BAZPASS=$(security find-internet-password -s '192.168.1.3' -a 'brackin' -w)
sshpass -p "$BAZPASS" ssh -o StrictHostKeyChecking=no brackin@192.168.1.3 \
  "cd ~/llm-bench-workspace/mod-playerbots && git fetch origin && git pull --ff-only origin claude/local-llm-bot-agents-O72i5 && \
   cd tools/llm-bench && uv sync --extra dev && uv run --extra dev pytest -v 2>&1 | tail -5"
```

Expected: 42 tests pass on Heimdal.

- [ ] **Step 3: Kick off the single-cell ROCm run**

```bash
BAZPASS=$(security find-internet-password -s '192.168.1.3' -a 'brackin' -w)
RUN_DATE=$(date +%Y-%m-%d)
sshpass -p "$BAZPASS" ssh -o StrictHostKeyChecking=no brackin@192.168.1.3 \
  "cd ~/llm-bench-workspace/mod-playerbots/tools/llm-bench && \
   uv run bench.py --rocm --single-cell --out-dir ../../results/${RUN_DATE}-rocm --stop-server-on-exit 2>&1 | tail -30"
```

Expected wall-clock: ~5-6 min (60 s steady + ~30-60 s burst + ROCm restart overhead).

Expected stdout:
- `[matrix] setting GPU power cap to 237 W`
- `[matrix] starting cell 1/1 model=qwen-2.5-7b slots=4`
- `[matrix]   /health 200; vram_idle=...; edge=...C junction=...C`
- `[matrix]   grammar=json_schema steady...`
- `[matrix]   mid-steady: vram_loaded=...; edge=...C junction=...C mem=...C`
- `[matrix]   grammar=json_schema burst...`
- `[matrix]   cell 1 flushed to ../../results/.../`
- `[matrix] done — results in ../../results/.../`
- `[matrix] restoring GPU power cap to hardware default`

- [ ] **Step 4: Inspect the results**

```bash
BAZPASS=$(security find-internet-password -s '192.168.1.3' -a 'brackin' -w)
RUN_DATE=$(date +%Y-%m-%d)
sshpass -p "$BAZPASS" ssh -o StrictHostKeyChecking=no brackin@192.168.1.3 \
  "cat ~/llm-bench-workspace/mod-playerbots/results/${RUN_DATE}-rocm/summary.md"
```

Expected: 1 idle + 2 phase rows (steady, burst). `backend: rocm` in metadata. `single_cell: True` in metadata.

- [ ] **Step 5: Pull results to Mac**

```bash
cd ~/Documents/Projects/playerbots-dev/mod-playerbots
BAZPASS=$(security find-internet-password -s '192.168.1.3' -a 'brackin' -w)
RUN_DATE=$(date +%Y-%m-%d)
mkdir -p results/${RUN_DATE}-rocm
sshpass -p "$BAZPASS" scp -o StrictHostKeyChecking=no \
  "brackin@192.168.1.3:~/llm-bench-workspace/mod-playerbots/results/${RUN_DATE}-rocm/*" \
  "results/${RUN_DATE}-rocm/"
ls -la results/${RUN_DATE}-rocm/
```

Expected: both files copied.

- [ ] **Step 6: Commit the results bundle**

```bash
cd ~/Documents/Projects/playerbots-dev/mod-playerbots
RUN_DATE=$(date +%Y-%m-%d)
git add results/${RUN_DATE}-rocm/
git commit -m "$(cat <<'EOF'
feat(llm-bench): record Phase 0.5 ROCm single-cell comparison results

Qwen 2.5 7B + json_schema + slots=4 on ROCm vs the Vulkan baseline in
results/2026-05-12-vulkan/. ~110 per-request rows in results.csv;
2 cell-summary rows in summary.md (steady + burst).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 6: Write §15.8 in the design doc

**Files:**
- Modify: `docs/llm_agent_design.md` (append §15.8)

- [ ] **Step 1: Pull the two-row summary numbers**

Read `results/YYYY-MM-DD-rocm/summary.md`. The Vulkan baseline cell is `qwen-2.5-7b | json_schema | 4 | steady` in `results/2026-05-12-vulkan/summary.md`:

```
| qwen-2.5-7b | json_schema | 4 | steady |  60 |  1603 |  2749 |  3405 | 66.4 | 282 | 100.0% | 5061 | 5073 | 69 | 86 |
| qwen-2.5-7b | json_schema | 4 | burst  |  50 |  3675 |  4362 |  4665 | 53.1 |  41 | 100.0% | 5061 | 5073 | 69 | 86 |
```

Write down the corresponding ROCm row in scratch form. Then compute:
- p50 delta % = (ROCm p50 − Vulkan p50) / Vulkan p50 × 100
- p95 delta %
- decode tok/s delta %
- VRAM delta MB
- Junction temp delta °C

- [ ] **Step 2: Append §15.8 to `docs/llm_agent_design.md`**

Add to the end of the file (after §15.7):

```markdown

### 15.8 ROCm vs Vulkan — head-to-head (production cell)

Run date: <YYYY-MM-DD>. Same hardware as §15.1 (Heimdal, RX 6900 XT, 16 GB).
Image: `ghcr.io/ggml-org/llama.cpp:server-rocm`. Workload: Qwen 2.5 7B Q4_K_M
+ `response_format` JSON Schema + `--parallel 4` (the Phase 0.5 production
cell). Methodology: [Phase 0.5 ROCm comparison spec](superpowers/specs/2026-05-12-llm-agent-phase-0.5-rocm-comparison-design.md).

Full per-request data: [`results/<YYYY-MM-DD>-rocm/results.csv`](../results/<YYYY-MM-DD>-rocm/results.csv).
Per-cell aggregates: [`results/<YYYY-MM-DD>-rocm/summary.md`](../results/<YYYY-MM-DD>-rocm/summary.md).

| Metric (steady, N=60) | Vulkan | ROCm | Δ |
| --- | ---: | ---: | ---: |
| p50 wall (ms) | 1,603 | <fill> | <±%> |
| p95 wall (ms) | 2,749 | <fill> | <±%> |
| decode (tok/s) | 66.4 | <fill> | <±%> |
| adherence | 100% | <fill> | <pp> |
| VRAM loaded (MB) | 5,073 | <fill> | <±MB> |
| junction °C peak | 86 | <fill> | <±°C> |

**Decision**: <one paragraph — does production go to ROCm or stay on Vulkan?
Cite specific number(s) that drove the call. Also note any unexpected
finding worth a follow-up.>

**Implication for §15.6**: <if ROCm wins, update §15.6's production
recommendation; if Vulkan stays, explicitly close this follow-up from
§15.7.>
```

Replace the `<fill>`, `<±%>`, `<±MB>`, `<±°C>`, `<pp>`, and `<one paragraph>` placeholders with the actual numbers and decision. **No placeholders in the final commit.**

If the ROCm pass surfaces unexpected issues (e.g. dramatically different VRAM use, adherence regression, thermal anomaly), surface them in the decision paragraph.

- [ ] **Step 3: Update §15.7 (open follow-ups) to mark the ROCm pass as resolved**

In `docs/llm_agent_design.md`, find the §15.7 bullet:

```markdown
- **ROCm comparison pass** (originally deferred). The 6900 XT supports
  ROCm; if the same matrix on ROCm gains ≥20% throughput, it justifies
  the heavier setup.
```

Replace with one of (pick the one matching reality):

If Vulkan stays:

```markdown
- ~~ROCm comparison pass~~ — resolved <YYYY-MM-DD>; see §15.8. Vulkan
  stays as the production backend.
```

If ROCm wins:

```markdown
- ~~ROCm comparison pass~~ — resolved <YYYY-MM-DD>; see §15.8. ROCm
  wins on the production cell; full-matrix ROCm rerun is the next
  follow-up.
```

- [ ] **Step 4: Verify the doc renders sensibly**

```bash
cd ~/Documents/Projects/playerbots-dev/mod-playerbots
grep -c "^### " docs/llm_agent_design.md
```

Expected: 9 (was 8 before; added §15.8).

Inspect §15.8:

```bash
awk '/^### 15\.8/,/EOF/' docs/llm_agent_design.md | head -30
```

Confirm no `<fill>` or `<±%>` placeholders remain.

- [ ] **Step 5: Commit + push**

```bash
cd ~/Documents/Projects/playerbots-dev/mod-playerbots
git add docs/llm_agent_design.md
git commit -m "$(cat <<'EOF'
docs(llm-agent): add §15.8 — ROCm vs Vulkan head-to-head decision

Fold the single-cell ROCm comparison numbers from
results/<YYYY-MM-DD>-rocm/ into the parent design doc with explicit
Δ vs Vulkan. Decision paragraph resolves the §15.7 open follow-up
either way (Vulkan stays / ROCm wins triggering a full-matrix rerun).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
git push origin claude/local-llm-bot-agents-O72i5
```

---

## Task 7: Close-out — record milestone in ninum-knowledge

**Files:** None in repo.

- [ ] **Step 1: Verify Heimdal state**

```bash
BAZPASS=$(security find-internet-password -s '192.168.1.3' -a 'brackin' -w)
sshpass -p "$BAZPASS" ssh -o StrictHostKeyChecking=no brackin@192.168.1.3 \
  "systemctl is-active llama-server.service; systemctl is-active llama-server-rocm.service; cat /sys/class/hwmon/hwmon2/power1_cap"
```

Expected: both services `inactive`, power_cap back to `264000000` (default).

- [ ] **Step 2: Create ninum-knowledge entry**

Use `mcp__ninum-knowledge__create_knowledge_entry` with project `proj_aeb8eaa4` ("AzerothCore (Heimdal)"), entry_type `project_milestone`, tags `["llm-agent", "phase-0.5", "rocm", "vulkan", "comparison", "milestone", "complete"]`.

Title: `Phase 0.5 ROCm Comparison Complete — <Vulkan stays|ROCm wins> (<YYYY-MM-DD>)`.

Body: brief recap citing §15.8 numbers, the decision, the resolved §15.7 follow-up, and any new open follow-ups (e.g. "ROCm wins → schedule full-matrix ROCm rerun" if applicable).

- [ ] **Step 3: Done.**

If applicable, surface next-step suggestion: either start Phase 1 implementation (using the validated backend + config), or schedule the full-matrix ROCm rerun if §15.8 said ROCm wins.

---

## Appendix A: Test inventory after this plan

| File | Tests | Notes |
| --- | --- | --- |
| Existing Phase 0.5 tests | 35 | All untouched by this plan |
| `tests/test_server.py` (new) | 7 | backend registry, dispatch, env-file preservation |
| **Total** | **42** | |

## Appendix B: Files created or modified

**New:**
- `infra/heimdal/llama-server-rocm.container`
- `infra/heimdal/llama-server-rocm.env`
- `tools/llm-bench/tests/test_server.py`
- `results/<YYYY-MM-DD>-rocm/results.csv` + `summary.md`

**Modified:**
- `infra/heimdal/sudoers-llama-server` (add 4 NOPASSWD lines)
- `infra/heimdal/README.md` (add ROCm deploy section)
- `tools/llm-bench/llmbench/server.py` (backend registry, dispatch, env preservation, longer health timeout)
- `tools/llm-bench/bench.py` (add `--rocm`, `--single-cell`, mutual exclusion, backend thread-through)
- `docs/llm_agent_design.md` (append §15.8, update §15.7)
