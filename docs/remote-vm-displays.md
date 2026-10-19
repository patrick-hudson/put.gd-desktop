# Remote QEMU displays on the workstation wall

The installed [display wall](http://127.0.0.1:8765/) discovers local QEMU processes.
Remote QEMU processes need an explicit SSH forward and a `name`/`port` entry.
The existing renderer supports this; remote discovery/tunnel creation is manual.
Use the pinned hosts in [remote builders](remote-builders.md).

1. Inspect the VM's run metadata and actual process on its builder. Confirm VNC
   listens on remote loopback. `--vnc-display 20` means TCP `5920`; use the actual
   run's port. Check existing processes/ports on the builder and workstation.
2. Pick a distinct free workstation port and service name per VM/run. Keep
   physical desktop ports `5970`, `5979` and `5972` reserved. This Fedora example maps
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

   For Debian substitute `.69` and the `d69/known_hosts` pin; for Kubuntu use
   `.72` and `k72/known_hosts`. Allocate a different unit/port for each guest.
   Local bind success does not establish remote VM readiness.
3. Inspect the running wall service's `Environment` and `ExecStart` for its
   actual configuration. The current workstation default is
   `/home/patrick/work/put.gd/tools/qemu-grid/remote-displays.json`. Back it up
   privately and append this object to its JSON array, preserving all host
   desktops and other VM tiles:

   ```json
   {"name": "Fedora VM RUN_ID", "port": 5981}
   ```

   Entries need a `name` and an integer `port`. They may also have `type`
   (`vm` or `physical`), `ip` and `hostIp`, and, for multi-head guests, `group`
   and `head` (below). Keep tunnel/run metadata in evidence separately.
   Coordinate edits and replace the file atomically. The wall rereads it every
   three seconds; no restart is required. Names keep only letters, digits,
   spaces, `.`, `_` and `-`; other characters show as `_`.
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

## Multi-head guests

A [multi-head guest](vm-lab.md#multi-head-guests) has one VNC listener per head, on
consecutive remote ports (`--vnc-display 60` with four heads: 5960-5963). Forward every
head, each with its own `-L` and workstation port. One SSH process per guest is enough:

```sh
systemd-run --user --unit=putgd-vm-fedora-RUN_ID --collect \
  --property=Type=exec --property=Restart=on-failure --property=RestartSec=3s \
  /usr/bin/ssh -NT -i /home/patrick/.ssh/awskey_110724 \
  -o BatchMode=yes -o IdentitiesOnly=yes -o StrictHostKeyChecking=yes \
  -o UserKnownHostsFile=/home/patrick/work/put.gd/work/remote-builders/f44/known_hosts \
  -o ExitOnForwardFailure=yes -o ServerAliveInterval=30 -o ServerAliveCountMax=3 \
  -L 127.0.0.1:5990:127.0.0.1:5960 -L 127.0.0.1:5991:127.0.0.1:5961 \
  -L 127.0.0.1:5992:127.0.0.1:5962 -L 127.0.0.1:5993:127.0.0.1:5963 patrick@192.168.68.70
```

Add one `remote-displays.json` entry per head. Entries with the same `group` are one
machine: the wall draws them as one card with the screens side by side, ordered by `head`
(0-based, as in the run's `heads` metadata). The card takes its title from the first head:

```json
{"name": "Cinnamon VM RUN_ID - 2 heads", "port": 5990, "group": "RUN_ID", "head": 0},
{"name": "Cinnamon VM RUN_ID - 2 heads", "port": 5991, "group": "RUN_ID", "head": 1}
```

Each screen keeps its own **Enable KB + Mouse** toggle, and enabling one head's input turns it
off on the card's other heads. **Expand** shows the whole card. Local QEMU processes need no
entries: the wall finds every `-vnc` listener of a process and groups them by its `head=`
option. Entries without `group` and the physical-desktop tiles are drawn as before.

The wall itself (`tools/qemu-grid`, run by `putgd-display-wall.service`) is local and not in
git. Back up its files privately before changing them, test a copy on another port
(`QEMU_GRID_PORT=8775 QEMU_GRID_REMOTES=COPY.json node server.mjs`), replace the live files
atomically, and restart the service once. Then check that every existing tile reconnects.

## Routing verification, 2026-10-16

A real diskless QEMU 10.2.2 process on Fedora displayed its firmware console on
remote `5930`, forwarded to workstation `5980`. The tile was **Fedora remote QEMU -
firmware routing check**. Both physical desktops remained visible. Saved screenshot
and exact process/tunnel receipts are in the private
`work/verification/2026-10-16-remote-vm-wall/` directory.

This proves remote QEMU display routing only. It is not an OS desktop or app
acceptance result. The run is diskless and uses TCG; neither server exposed
`/dev/kvm` or CPU virtualization flags at this check. VM acceptance still needs
its required acceleration/profile preflight.

The demo was retired at the owner's request on 2026-10-17: its recorded process
and QMP socket were already absent, its `putgd-vm-display-53.service` tunnel was
stopped, and only its matching tile was removed. Historical evidence is retained.
