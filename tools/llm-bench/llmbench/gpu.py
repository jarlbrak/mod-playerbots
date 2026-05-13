"""AMDGPU temperature sampling + power-cap control via sysfs."""

import math
import subprocess
from pathlib import Path


HWMON_DIR = "/sys/class/hwmon/hwmon2"
POWER_CAP_PATH = f"{HWMON_DIR}/power1_cap"
POWER_CAP_DEFAULT_PATH = f"{HWMON_DIR}/power1_cap_default"


def parse_temp_celsius(raw: str) -> float:
    """AMDGPU sysfs reports millidegrees C as int. Convert to float Celsius."""
    value = int(raw.strip())
    return value / 1000.0


def sample_gpu_temps(hwmon_dir: str = HWMON_DIR) -> dict[str, float]:
    """Read edge / junction / mem temps. Missing files return NaN.

    AMDGPU exposes temp1 (edge), temp2 (junction), temp3 (mem) on most
    discrete cards. The labels live in temp{N}_label; we assume the
    standard order without re-reading them every call.
    """
    base = Path(hwmon_dir)
    out: dict[str, float] = {}
    for label, fname in (("edge_c", "temp1_input"), ("junction_c", "temp2_input"), ("mem_c", "temp3_input")):
        path = base / fname
        if not path.exists():
            out[label] = float("nan")
            continue
        try:
            out[label] = parse_temp_celsius(path.read_text())
        except (ValueError, OSError):
            out[label] = float("nan")
    return out


def set_power_cap_watts(watts: int, hwmon_dir: str = HWMON_DIR) -> None:
    """Set GPU power cap in watts (converted to microwatts for sysfs).

    Requires the sudoers drop-in granting NOPASSWD on:
      /usr/bin/tee /sys/class/hwmon/hwmon2/power1_cap
    """
    microwatts = watts * 1_000_000
    payload = f"{microwatts}\n".encode()
    subprocess.run(
        ["sudo", "-n", "/usr/bin/tee", f"{hwmon_dir}/power1_cap"],
        input=payload,
        stdout=subprocess.DEVNULL,
        check=True,
    )


def restore_power_cap(hwmon_dir: str = HWMON_DIR) -> None:
    """Restore power_cap to its hardware default (read from power1_cap_default)."""
    default_microwatts = int(Path(f"{hwmon_dir}/power1_cap_default").read_text().strip())
    payload = f"{default_microwatts}\n".encode()
    subprocess.run(
        ["sudo", "-n", "/usr/bin/tee", f"{hwmon_dir}/power1_cap"],
        input=payload,
        stdout=subprocess.DEVNULL,
        check=True,
    )
