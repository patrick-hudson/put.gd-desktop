# Disposable graphical lab

The host interface is `python3 tools/lab/lab.py`. It uses QEMU, QMP, SSH and ordinary
distro packages. It never shares the host clipboard. All control sockets are private;
VNC and SSH forwarding bind to 127.0.0.1. A QMP witness is independent of guest capture.
The user watches active machines at http://127.0.0.1:8765/. Keep each guest’s VNC
listener and graphical output active during interaction; use `run --keep` while
continuing a test session so subsequent actions remain visible on the display wall.
A locked or blanked session must be visibly restored before graphical acceptance.
Host protocol tests and offscreen fixture checks do not prove desktop acceptance.

For opt-in parallel package preparation on disposable cloud workers, use
[exact candidate builds on Boat](boat-candidates.md). Its artifact and snapshot
receipts are separate from the graphical acceptance evidence below.

## Budgie goldens

The [golden inventory](remote-golden-images.md#budgie-goldens-ec2-2026-11-08)
lists four immutable, EC2-built sources on f44, d69 and S3. Use the matching
profile from companion [code PR #1287](https://github.com/patrick-hudson/put.gd-desktop/pull/1287):

| Profile / golden directory | Native session | Package input |
| --- | --- | --- |
| `fedora44-budgie` | Budgie 10.10.2 / labwc 0.9.6 Wayland | AppImage |
| `ubuntu-budgie-2604` | Budgie 10.10.2 / labwc 0.9.3 Wayland | DEB (Ubuntu 26.04) |
| `fedora45-budgie` | Budgie 10.10.3 / labwc 0.20.2 Wayland | AppImage |
| `ubuntu-budgie-2404` | Budgie 10.9.1 X11 | DEB (Ubuntu 24.04) |

On d69, with no other owner using the example ports, run a fresh overlay:

```sh
python3 tools/lab/lab.py launch \
  --profile fedora45-budgie \
  --golden /home/patrick/work/put.gd/work/lab-images/fedora45-budgie/image.qcow2 \
  --run /home/patrick/work/put.gd/work/lab-runs/b1 \
  --bootstrap-key /home/patrick/work/put.gd/work/lab-bootstrap-d69/identity \
  --known-hosts /home/patrick/work/put.gd/work/lab-images/fedora45-budgie/known_hosts \
  --ssh-port 22442 --vnc-display 82 --timeout 300
```

Use f44's existing bootstrap key and pinned builder connection on f44. Keep
the guest's VNC listener on the [display wall](remote-vm-displays.md). The approved
login is `lab` / `putgd-lab`; SDDM autologin selects the real Budgie session on
the three Wayland images, and LightDM does so on Ubuntu 24.04 X11. Preserve the
native Qt platform environment: Fedora 45 sets `wayland;xcb`, while Fedora 44
and Ubuntu 26.04 set `wayland`; Ubuntu 24.04 leaves it unset and uses X11.

`install` accepts the published DEB or AppImage in the table, then `start`
requires live session/idle/output preflight and verifies the resident binary.
`run` also creates a fresh overlay and retires successful completed runs; follow
[package and run](vm-lab/package-and-run.md) and
[overlay retirement](vm-lab/retire-overlays.md) for full scenario commands and
custom/manual completion manifests. Profiles remain marked
`requires-graphical-acceptance`: step-1 bridge/tray/protocol checks do not replace
exact-build application acceptance. Never modify the golden for package updates.

Live uploads use `tools/lab/provision_live_credentials.py` as on the other
desktops; it accepts the `budgie-panel` session and keeps its Qt platform.
Fedora 45 Budgie's Secret Service is `oo7-daemon`, whose `login` collection stays
locked under autologin and ignores `SetAlias`, so `lab_keyring.py` can't make a
lab default there. Instead, install `gnome-keyring` in the overlay and start a
private `gnome-keyring-daemon --components=secrets` on `/dev/shm`, as
`nightly_desktop_guest.prepare_live_keyring` does for KWallet guests: find its
unique bus name, then run `lab_keyring.py` with `SECRET_SERVICE_BUS_NAME` set to
it, which creates the blank `putgd lab` collection and makes it that daemon's
default, and check `lab_keyring.py --status` reports it unlocked. Record the bus
name as the run's `secret_service`, so the bridge and a resident started with
`SECRET_SERVICE_BUS_NAME`, `QTKEYCHAIN_BACKEND=libsecret` both use it. oo7
keeps its name and collections.

## Topic pages

The lab guide is split by topic, with the text unchanged.

- [Hosts, bases and login inputs](vm-lab/hosts-and-bases.md): Host selection; Base protection and inventory; Protected login inputs.
- [Golden disks](vm-lab/golden-disks.md): First Cinnamon golden disk, including guest install and authenticated SSH port remapping; Additional distro base disks.
- [Package and run](vm-lab/package-and-run.md): building the lab package and the `install`, `run` and session commands.
- [Install staging and recovery](vm-lab/install-staging.md): how `install` stages the package in a guest and recovers an interrupted attempt.
- [Install launcher and process tracking](vm-lab/install-tracking.md): the launcher's probes, cgroup tracking and the limits of session-tracked jobs.
- [Multi-head and visible guests](vm-lab/guests-and-displays.md): Multi-head guests; Visible guest preflight.
- [Overlay retirement](vm-lab/retire-overlays.md): Retire completed overlays; Retire a pre-existing run outside the canonical root.
- [Input, editor drivers and evidence](vm-lab/input-and-evidence.md): Physical-desktop input agent; Editor acceptance drivers; Evidence and gates.

## First Cinnamon golden disk

The OS is installed once, and tests then boot independent copy-on-write overlays of that disk, so they never repeat the installer. The procedure is under [First Cinnamon golden disk](vm-lab/golden-disks.md#first-cinnamon-golden-disk).

## Retire completed overlays

Retire a finished run with the guarded `cleanup` command on the actual QEMU host. A passed, fully exported standard scenario needs no manifest; custom and manual campaigns need a completion evidence manifest. The guard refuses a run outside this checkout's `work/lab-runs`, which must be stopped and relocated instead. The procedure is under [Retire completed overlays](vm-lab/retire-overlays.md#retire-completed-overlays).
