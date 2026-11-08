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
