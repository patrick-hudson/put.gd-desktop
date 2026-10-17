# Remote QEMU displays on the workstation wall

The installed [display wall](http://127.0.0.1:8765/) discovers local QEMU processes.
Remote QEMU processes need an explicit SSH forward and a `name`/`port` entry.
The existing renderer supports this; remote discovery/tunnel creation is manual.
Use the pinned hosts in [remote builders](remote-builders.md).

1. Inspect the VM's run metadata and actual process on its builder. Confirm VNC
   listens on remote loopback. `--vnc-display 20` means TCP `5920`; use the actual
   run's port. Check existing processes/ports on the builder and workstation.
2. Pick a distinct free workstation port and service name per VM/run. Keep
   physical desktop ports `5970` and `5979` reserved. This Fedora example maps
   workstation `5981` to remote `5920`; replace `RUN_ID` and ports before use:

   ```sh
   systemd-run --user --unit=putgd-vm-fedora-RUN_ID --collect \
     --property=Type=exec --property=Restart=on-failure --property=RestartSec=3s \
     /usr/bin/ssh -NT -i /home/patrick/.ssh/awskey_110724 \
     -o BatchMode=yes -o IdentitiesOnly=yes -o StrictHostKeyChecking=yes \
     -o UserKnownHostsFile=/home/patrick/work/put.gd/work/remote-builders/f44/known_hosts \
     -o ExitOnForwardFailure=yes -o ServerAliveInterval=30 -o ServerAliveCountMax=3 \
     -L 127.0.0.1:5981:127.0.0.1:5920 patrick@192.168.68.70
   systemctl --user status putgd-vm-fedora-RUN_ID.service
   ```

   For Debian substitute `.69`, the `d69/known_hosts` pin, and a different
   unit/port. Local bind success does not establish remote VM readiness.
3. Inspect the running wall service's `Environment` and `ExecStart` for its
   actual configuration. The current workstation default is
   `/home/patrick/work/put.gd/tools/qemu-grid/remote-displays.json`. Back it up
   privately and append this object to its JSON array, preserving both host
   desktops and other VM tiles:

   ```json
   {"name": "Fedora VM RUN_ID", "port": 5981}
   ```

   Entries accept exactly `name` and integer `port`. Keep tunnel/run metadata
   in evidence separately. Coordinate edits and replace the file atomically.
   The wall rereads it every three seconds; no restart is required.
4. Check the forwarded RFB connection, then visibly confirm **VNC connected**
   and the correct guest on the wall. A configured tile or RFB header alone is
   not graphical evidence. Enable keyboard/mouse only on that tile while
   interacting, then return to view-only. Capture decisive actions and results
   with host, run, QEMU version and source/build identities.
5. At retirement remove only that run's object and stop its owned tunnel:

   ```sh
   systemctl --user stop putgd-vm-fedora-RUN_ID.service
   ```

   Preserve entries added since your backup. Retire overlays separately through
   [guarded lab cleanup](vm-lab.md), after exporting verified evidence.

## Routing verification, 2026-10-16

A real diskless QEMU 10.2.2 process on Fedora displayed its firmware console on
remote `5930`, forwarded to workstation `5980`. The tile is **Fedora remote QEMU -
firmware routing check**. Both physical desktops remained visible. Saved screenshot
and exact process/tunnel receipts are in the private
`work/verification/2026-10-16-remote-vm-wall/` directory.

This proves remote QEMU display routing only. It is not an OS desktop or app
acceptance result. The run is diskless and uses TCG; neither server exposed
`/dev/kvm` or CPU virtualization flags at this check. VM acceptance still needs
its required acceleration/profile preflight. The demo tunnel is
`putgd-vm-display-53.service`; stop the demo after the user's inspection using
its recorded QMP/process identity and remove only its matching tile.
