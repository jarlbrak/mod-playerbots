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
