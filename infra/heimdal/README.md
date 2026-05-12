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
  brackin@192.168.1.3:/tmp/

# Install on Heimdal
$ALIAS 'sudo install -m 0644 /tmp/llama-server.container /etc/containers/systemd/llama-server.container'
$ALIAS 'sudo install -m 0644 /tmp/llama-server.env       /etc/llama-server.env'
$ALIAS 'sudo install -m 0440 /tmp/sudoers-llama-server   /etc/sudoers.d/llama-server'
$ALIAS 'sudo visudo -c -f /etc/sudoers.d/llama-server'   # validate
$ALIAS 'sudo systemctl daemon-reload && sudo systemctl start llama-server.service'
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
