"""AMDGPU VRAM sampling via kernel sysfs."""

from pathlib import Path


def parse_vram_bytes(raw: str) -> int:
    value = int(raw.strip())
    if value < 0:
        raise ValueError(f"negative VRAM bytes: {value}")
    return value


def sample_vram(card_device_dir: str) -> tuple[int, int]:
    """Return (used_mb, total_mb) for the given /sys/class/drm/cardN/device dir.

    The caller picks which card. On Heimdal, render node /dev/dri/renderD128
    corresponds to the AMD discrete GPU under /sys/class/drm/card1/device
    (card0 is typically the iGPU or fallback). Resolution happens in the
    orchestrator, not here.
    """
    base = Path(card_device_dir)
    used = parse_vram_bytes((base / "mem_info_vram_used").read_text())
    total = parse_vram_bytes((base / "mem_info_vram_total").read_text())
    return used // (1024 * 1024), total // (1024 * 1024)
