import subprocess
from unittest import mock

import pytest

from llmbench.gpu import (
    parse_temp_celsius,
    sample_gpu_temps,
    set_power_cap_watts,
    restore_power_cap,
)


def test_parse_temp_celsius_basic():
    assert parse_temp_celsius("75000\n") == 75.0


def test_parse_temp_celsius_zero():
    assert parse_temp_celsius("0") == 0.0


def test_parse_temp_celsius_nonint_raises():
    with pytest.raises(ValueError):
        parse_temp_celsius("not a number")


def test_sample_gpu_temps_reads_three_sensors(tmp_path):
    (tmp_path / "temp1_input").write_text("48000\n")
    (tmp_path / "temp2_input").write_text("55000\n")
    (tmp_path / "temp3_input").write_text("52000\n")
    temps = sample_gpu_temps(str(tmp_path))
    assert temps == {"edge_c": 48.0, "junction_c": 55.0, "mem_c": 52.0}


def test_sample_gpu_temps_missing_file_returns_nan(tmp_path):
    # Only edge present; junction and mem missing.
    (tmp_path / "temp1_input").write_text("48000\n")
    temps = sample_gpu_temps(str(tmp_path))
    assert temps["edge_c"] == 48.0
    import math
    assert math.isnan(temps["junction_c"])
    assert math.isnan(temps["mem_c"])


def test_set_power_cap_watts_invokes_sudo_tee():
    with mock.patch("subprocess.run") as m:
        set_power_cap_watts(200, hwmon_dir="/sys/class/hwmon/hwmon2")
    assert m.called
    args, kwargs = m.call_args
    cmd = args[0]
    # First positional arg should be the command list starting with sudo, calling tee
    assert cmd[0] == "sudo"
    assert any("tee" in part for part in cmd)
    assert "/sys/class/hwmon/hwmon2/power1_cap" in cmd
    # The watts value (200 W → 200000000 uW) goes via stdin
    assert kwargs["input"] == b"200000000\n"


def test_restore_power_cap_reads_default_and_writes(tmp_path):
    (tmp_path / "power1_cap_default").write_text("264000000\n")
    with mock.patch("subprocess.run") as m:
        restore_power_cap(hwmon_dir=str(tmp_path))
    args, kwargs = m.call_args
    assert kwargs["input"] == b"264000000\n"
