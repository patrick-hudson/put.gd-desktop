# Preferred build and graphical test servers

Patrick's two physical Linux servers are the preferred project build, VM and
acceptance hosts. Choose between them by required tools/profile and available
capacity; use workstation-local VMs when the servers are unavailable or cannot
run the required profile. Record the reason for each fallback. Work under
`~/work/<project>` and preserve existing checkouts, builds, desktops and user work.

| Host | SSH address | Existing physical desktop | Workstation VNC port | Workstation user service |
| --- | --- | --- | --- | --- |
| Fedora 44 Workstation | `patrick@192.168.68.70` | GNOME on Wayland | `127.0.0.1:5970` | `putgd-fedora-vnc.service` |
| Debian 13 | `patrick@192.168.68.69` | KDE Plasma on Wayland | `127.0.0.1:5979` | `putgd-debian-vnc.service` |

The user authorizes full root access on these two machines through passwordless
sudo: install normal distro dependencies, configure build tools, compile and run
tests using `sudo -n COMMAND` (or `sudo -n -i`). `sudo -n id -u` must report `0`.
Do not apply this remote authorization to the workstation or unrelated machines.
If noninteractive sudo fails, report it rather than requesting or recording a
password. Keep distro Qt dynamically linked and use the project's pinned official
AWS SDK; see [build instructions](build.md).

The [Linux acceptance matrix](acceptance-matrix.md) covers Mint, Ubuntu, Kubuntu,
Fedora and Debian, GNOME/KDE, and native X11/Wayland. Combine host desktops and
builder-hosted VMs as needed; physical-hardware release gates require the
appropriate physical machine. Provision missing profiles and record local fallbacks.

## Graphical acceptance on the physical desktops

The build servers themselves are authorized graphical acceptance targets. Build,
install and run the candidate directly in Fedora's existing GNOME/Wayland session
or Debian's existing Plasma/Wayland session. Use the corresponding physical-host
tile on the workstation wall, preserve the user's other windows/projects, and
save screenshots or recordings of the actual app behavior and failures.
Record the host OS, compositor/session version, exact source and running build,
scenario and result. SSH output or an offscreen test alone is insufficient.

These host-desktop checks do not require a VM or `/dev/kvm`. Select direct host
testing when its actual desktop/version matches the claim being tested; use a
VM for other required guest profiles. Preserve Cinnamon-first ordering, the
approved KDE experiment exception and the release matrix's exact versions.

## Pinned SSH access

Connect from the main workstation on the LAN. Both hosts accept
`/home/patrick/.ssh/awskey_110724`. Keep that private key on the workstation:
never copy it into a server, VM, source export, image or evidence packet. Use
the existing host pins; do not replace them or disable host-key verification.

```sh
# Fedora
ssh -i /home/patrick/.ssh/awskey_110724 -o IdentitiesOnly=yes \
  -o StrictHostKeyChecking=yes \
  -o UserKnownHostsFile=/home/patrick/work/put.gd/work/remote-builders/f44/known_hosts \
  patrick@192.168.68.70

# Debian
ssh -i /home/patrick/.ssh/awskey_110724 -o IdentitiesOnly=yes \
  -o StrictHostKeyChecking=yes \
  -o UserKnownHostsFile=/home/patrick/work/put.gd/work/remote-builders/d69/known_hosts \
  patrick@192.168.68.69
```

For unattended commands, also use `-o BatchMode=yes` and a bounded connection
timeout. A missing pin or mismatch requires verified identity recovery, not
trust-on-first-use. Local bootstrap notes and tunnel units live privately under
`work/remote-builders/f44/` and `work/remote-builders/d69/`.

## Visual results are required

Both physical desktops appear at [the display wall](http://127.0.0.1:8765/).
Tiles start view-only; select **Enable KB + Mouse** to interact, then return to
view-only when finished. VNC has no password and must remain restricted to
loopback, reached through authenticated SSH tunnels. Never expose it to the LAN.
The services in the inventory table run on the workstation. Inspect/recover the
existing service before starting a duplicate tunnel or taking its port.

Keep graphical tests visible there throughout the run. Capture screenshots or
short recordings of the initial desktop, decisive actions, results, failures and
completion. Save the visual files with the host, session/compositor/version, exact
source commit/tree, installed and running build identities, scenario parameters
and pass/fail result. Preserve failed attempts. Exclude revealed credentials and
protected login values from screenshots, recordings, logs and review material.
Follow [evidence retention](evidence-retention.md).

A host tile does not automatically display its VMs. Before remote VM acceptance,
bind the guest's VNC listener to remote loopback, allocate a distinct free
workstation port and forward it over pinned SSH. Register a separate guest tile
in the existing wall configuration, preserving both host tiles. The current wall
reads its `QEMU_GRID_REMOTES` JSON file, or
`tools/qemu-grid/remote-displays.json` by default: entries have exactly `name` and
`port`, with the port naming a workstation-loopback tunnel. Verify the live guest
visually; a listed tile or an RFB handshake alone does not establish visibility.

SSH builds/logs and offscreen tests do not establish graphical acceptance. Test
the actual GNOME or Plasma session for physical-host claims. Required release
profiles retain their exact OS/desktop versions; Fedora GNOME or Debian Plasma
does not waive a different version in that matrix. Cinnamon-first ordering and
the approved KDE memory-experiment exception remain in force. For VM scenarios,
retain the existing independent QMP, saved-artifact and native-paste witnesses.

## Remote VM operating procedures

Use [remote VM display tunnels](remote-vm-displays.md) to add each builder-hosted
QEMU guest to the existing wall. Use [golden image discovery and copying](remote-golden-images.md)
for exact workstation paths, verified transfers and the approved shared guest login.

## VM preflight and golden-image copies

Before provisioning or launching a guest, inspect the actual remote host's CPU
virtualization support, accessible `/dev/kvm`, QEMU tools, RAM, storage/inodes,
running VMs, backing chains and reserved SSH/VNC ports. Retain per-host
[storage preflight](lab-storage.md) receipts. Provision missing distro packages
with the authorized noninteractive sudo. Do not count TCG as KVM acceptance or
start a hidden guest because display setup is incomplete. If the host cannot
meet the profile, record the blocked gate and use a suitable local VM fallback.

Golden-image copying is authorized; it is not a request to move or retire the
originals. For each transfer:

1. Inventory the source and its complete backing chain. Never copy a running
   guest's writable disk as a golden. Prefer the existing standalone provisioned
   goldens documented in [VM lab](vm-lab.md). If flattening is needed, produce a
   new standalone image and verify it; never rebase or alter the original chain.
2. Record the source image SHA-256, virtual size and sidecar/provisioning identity.
   Check remote capacity for the copy, staging and future overlays before transfer.
3. Copy to a fresh private directory under
   `~/work/put.gd/work/lab-images/` on the selected host, using a temporary filename
   and sparse transfer where supported. Never overwrite an existing golden or a
   disk referenced by overlays. Do not copy workstation private keys or secrets.
4. Verify the destination checksum, `qemu-img info` backing-file state and virtual
   size, and `qemu-img check`. Retain source/destination paths and verification
   receipts before promoting the complete image to its new immutable golden path.
   Never rewrite historic run metadata to point at the copy.
5. Create fresh disposable overlays under that host's
   `~/work/put.gd/work/lab-runs/` through the existing runner. Never boot either
   golden directly. Bootstrap separate guest identities on the host/run through
   the documented visible console; do not distribute the workstation key.
6. Run the lab controller on the actual QEMU host using the same committed source
   and required native dependencies. Preflight all resolved Unix socket paths
   there (107 UTF-8 bytes maximum), and keep guest displays on the workstation
   wall. The runner currently has no remote-host command-line switch.
7. Export source/build identities, functional artifacts, visual evidence,
   failures and completion/retirement receipts to the workstation before guarded
   cleanup of completed disposable overlays. Preserve both golden copies and
   existing projects. Follow [VM lifecycle](vm-lab.md) and
   [retention rules](evidence-retention.md).

Windows bases retain their stricter disk/UEFI/TPM/identity bundle and independent
copy rules; see [Windows lab](windows-vm-lab.md). Copying a Linux golden does not
authorize partial Windows-base transfers.

## Access inventory checked 2026-10-16

Read-only pinned-SSH checks reached both hosts, identified Fedora 44 and Debian 13,
and confirmed `sudo -n id -u` returns `0`. Fedora had `qemu-system-x86_64` and
`qemu-img`; Debian had neither command on the tested PATH. `/dev/kvm` was not
present in either check. VM-host readiness therefore still requires preflight
and setup; these checks do not establish guest or graphical app acceptance.
Recheck capabilities and versions for every campaign rather than treating this
inventory as a permanent readiness receipt.
