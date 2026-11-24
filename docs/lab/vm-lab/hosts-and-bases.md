# VM lab: hosts, bases and login inputs

Part of [Disposable graphical lab](../vm-lab.md).

## Host selection

Plan host/guest placement with the [Linux acceptance matrix](../../testing/acceptance-matrix.md).
Cover Mint, Ubuntu, Kubuntu, Fedora and Debian plus GNOME/KDE X11/Wayland;
provision missing VM profiles while using physical machines for physical release gates.

Use the [preferred Fedora/Debian/Kubuntu build servers](../remote-builders.md) before local
workstation VMs. Select a host with the required capacity, KVM access and visible
guest display; record a local fallback reason if none can run the profile.
The runner executes on the actual QEMU host, via pinned SSH for remote hosts;
it does not currently provide a remote-host switch. Run its storage/process/port
inventory there, check resolved socket lengths there, and export receipts and
visual evidence to the workstation. Do not treat workstation capacity or process
absence as evidence about a remote host.

The existing Fedora GNOME and Debian Plasma display-wall tiles show physical
desktops. Add a distinct loopback-only SSH tunnel and tile for each remote guest
before graphical acceptance. Save screenshots or recordings of actions, results
and failures as well as the existing independent QMP/paste/artifact witnesses.
Host selection leaves the required guest OS/desktop versions
unchanged. Follow the remote-host runbook when copying goldens; never
boot or alter the source golden or reuse another run's writable overlay.

Linux source-build guests also require the standard cursor development package: `libxcursor-dev` on Ubuntu/Mint or `libXcursor-devel` on Fedora. Install it in disposable development guests using their normal package manager. The desktop profile files describe runtime sessions; they do not contain development package lists. Provision a new golden disk for persistent package updates, preserving sealed bases and active overlays.

## Base protection and inventory

Provisioned Linux golden disks under `work/lab-images/` are immutable inputs.
Never boot or modify them directly, or move, overwrite or delete a disk referenced
by overlays. Since 2026-10-17 the goldens live on the Fedora and Debian builders
and in S3, not on the workstation; see
[golden storage and restore](../remote-golden-images.md). Tests use independent
disks under `work/lab-runs/` created through `tools/lab/lab.py`; provision a new
golden path for updates. Preserve each run's
recorded backing path and base identity.

Before provisioning or resuming, inventory existing run/base metadata and the
actual host-visible QEMU processes and reserved VNC/SSH ports. A sandbox's missing
PID does not prove that the host VM stopped. Verify the exact process identity
and owned overlay before stopping a VM; preserve its diagnostics. `reset` retains
the overlay. `launch` requires a new run directory and creates a new overlay;
it does not resume an existing run. `--run` must name a symlink-free direct child
of this checkout's canonical `work/lab-runs`; an outside or nested path is rejected
before allocating an overlay or bootstrapping anything. Use a short name such as
`work/lab-runs/c1`. If that still exceeds the 107-byte resolved socket limit,
use a checkout at a shorter real path and keep runs inside its canonical root;
a symlink or `/tmp` run outside that root does not provide a cleanup-safe shortcut. An existing-overlay resume must separately
validate its recorded backing chain, host process/port absence and new QEMU identity,
then update the live run metadata without overwriting the prior receipt.

The current Linux runner records the golden SHA256 but does not enforce
sealed manifests before launch. Existing golden
files may still be writable. These workflow restrictions remain mandatory.

The provisioned GNOME/KDE goldens are the standalone images
`gnome-provisioned-20261016` and `kde-provisioned-20261016`. Their host paths
and hashes are in the [golden inventory](../remote-golden-images.md#current-inventory).
Use these for new provisioned runs. A workstation fallback first restores the
one it needs to `work/lab-images/<name>.qcow2`. Their private sidecar manifests
record the source chain, image SHA256, integrity check and successful guest-visible comparison. The migration receipts
live under `work/housekeeping/standalone-goldens-20261016/`. Historical run
metadata retains the old paths and hashes; do not rewrite it to name the new
goldens. Never resume a retired run directory.

To flatten another chain, create a **new** golden path with `qemu-img convert`
and no backing-file option. Before retiring any old image, require successful
`qemu-img check` and `qemu-img compare OLD NEW`, equal virtual sizes, no backing
file in the new image, and a saved SHA256/manifest. Inventory actual host
processes, open files and every surviving lab backing chain. Remove dependent
leaf images before their unreferenced parents; preserve other goldens and all
host evidence. Do not use an unsafe rebase, modify an existing golden in place,
or delete a parent merely because its directory is named `lab-runs`.

## Protected login inputs

New remote lab guests use the approved `lab` / `putgd-lab` default; follow
[golden discovery, transfer and login provisioning](../remote-golden-images.md).
Historical source goldens retain their existing logins until an independent
candidate is provisioned, visibly verified and saved at a new golden path.
The inventory below locates legacy inputs; never edit an immutable source in place.

Agents restore existing locked sessions. First inspect the run's profile and
bootstrap metadata, then locate the protected local input for that installation.
The three existing lab installations use these local files:

| Native profile | Existing protected input |
| --- | --- |
| Cinnamon/X11 | `work/lab-bootstrap/install/password` |
| Ubuntu GNOME/Wayland | `work/lab-bootstrap/ubuntu-install/password` |
| Kubuntu KDE/Wayland | `work/lab-bootstrap/kubuntu-install/password` |

These paths identify local inputs; they contain no login value in this document.
Verify with `lstat` that the selected file is regular, not a symlink, owned by the
current user and mode `0600`. Read its value only in memory after verifying the
visible console's focused, masked password field. Send keystrokes through that
VM's QMP/VNC console, keeping its VNC listener and display wall active. Never print,
hash, screenshot or put the value in arguments, configuration, logs or review
material. Never use `credentials.txt` for desktop login. Visually verify the
unlocked native desktop before accepting a graphical result. Ask the user only
if the protected input is unavailable or the console rejects it.
