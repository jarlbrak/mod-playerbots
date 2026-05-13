# Heimdal infra for Phase 0.5

Deploy from the Mac after authoring locally:

```bash
BAZPASS=$(security find-internet-password -s '192.168.1.3' -a 'brackin' -w)
ALIAS="sshpass -p $BAZPASS ssh -o StrictHostKeyChecking=no brackin@192.168.1.3"

# Copy artifacts to a staging dir on Heimdal
sshpass -p "$BAZPASS" scp -o StrictHostKeyChecking=no \
  infra/heimdal/llama-server.container \
  infra/heimdal/llama-server.env \
  infra/heimdal/sudoers-llama-server \
  infra/heimdal/gpu-fan-max.service \
  brackin@192.168.1.3:/tmp/

# Install on Heimdal
$ALIAS 'sudo install -m 0644 /tmp/llama-server.container /etc/containers/systemd/llama-server.container'
$ALIAS 'sudo install -m 0644 /tmp/llama-server.env       /etc/llama-server.env'
$ALIAS 'sudo install -m 0440 /tmp/sudoers-llama-server   /etc/sudoers.d/llama-server'
$ALIAS 'sudo install -m 0644 /tmp/gpu-fan-max.service    /etc/systemd/system/gpu-fan-max.service'
$ALIAS 'sudo visudo -c -f /etc/sudoers.d/llama-server'   # validate
$ALIAS 'sudo systemctl daemon-reload && sudo systemctl enable --now gpu-fan-max.service && sudo systemctl start llama-server.service'
$ALIAS 'curl -s http://127.0.0.1:8080/health'
```

Stop the service after the run:

```bash
$ALIAS 'sudo systemctl stop llama-server.service'
```

Bazzite/SELinux notes:
- `/var/lib/llama-models` must be labeled `container_file_t` (see Task 3 step 1).
- The `:Z` flag on the volume mount triggers the relabel on first start.
- AMDGPU passthrough requires `brackin` in groups `render` and `video`.

Quadlet quirks (learned in Task 4):
- `GroupAdd=` uses numeric host GIDs (render=105, video=39). Named
  groups would fail because the llama.cpp container image lacks
  `render`/`video` entries in its `/etc/group`. Verify with
  `getent group render video` on the host before redeploy if GIDs ever
  change.
- `EnvironmentFile=` appears in BOTH `[Container]` and `[Service]`.
  `[Container]` -> `podman --env-file` (vars visible inside container).
  `[Service]`   -> systemd unit env so `${LLAMA_MODEL}` / `${LLAMA_SLOTS}`
                   in `Exec=` get substituted before podman is invoked.
  Both are required.

Thermal management (learned in Task 19 — Heimdal thermal-tripped under
sustained 1-hour GPU load and rebooted, losing the run):

- **GPU fan**: `gpu-fan-max.service` forces `pwm1_enable=1` and
  `pwm1=255` on `hwmon2` at boot. Fan stays at 0 RPM until firmware
  zero-RPM mode releases at ~50°C edge / 55°C junction, then ramps
  to ~4100-4200 RPM and stays there. `ExecStop` returns to auto curve
  (`pwm1_enable=2`).
- **GPU power**: `power1_cap_max = power1_cap_default = 264 W` —
  hardware-pinned upper bound. We can only REDUCE the cap; capping to
  e.g. 200 W trades ~24% peak compute for materially less heat output.
  Apply with `echo 200000000 | sudo tee /sys/class/hwmon/hwmon2/power1_cap`.
- **Case fans**: Gigabyte motherboard, EC-controlled. Linux only sees
  `gigabyte_wmi` (temps only — no PWM headers exposed). To max case
  fans you must enter BIOS and set Smart Fan profiles to "Full Speed"
  (or custom-max). This is a manual physical step. No `it87` or
  `nct67xx` module is currently loaded; force-loading them risks EC
  conflict on this board.
- **Thermal limits the driver enforces**: edge crit 100°C / emergency
  105°C; junction crit 110°C / emergency 115°C; mem crit 100°C /
  emergency 105°C. Any one of these will trigger hardware-level
  shutdown.
