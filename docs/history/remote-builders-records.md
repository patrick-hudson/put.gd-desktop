# Remote builder records

> **Historical record.** These are dated run records, moved unchanged from [remote builders](../lab/remote-builders.md) on 2026-11-05 (#1222). They describe what those runs found at the time, and their open or pending statements describe that time. For the current contract, see [remote builders](../lab/remote-builders.md). Do not use them as a work list.

## Access inventory checked 2026-10-16

Read-only pinned-SSH checks reached both hosts, identified Fedora 44 and Debian 13,
and confirmed `sudo -n id -u` returns `0`. Fedora had `qemu-system-x86_64` and
`qemu-img`; Debian had neither command on the tested PATH. `/dev/kvm` was not
present in either check. VM-host readiness therefore still requires preflight
and setup; these checks do not establish guest or graphical app acceptance.
Recheck capabilities and versions for every campaign rather than treating this
inventory as a permanent readiness receipt.

## Kubuntu host preflight, 2026-10-17

Pinned SSH and `sudo -n id -u` passed on `.72`. The machine has an Intel i5-6500T
(four logical CPUs), 15 GiB usable RAM and approximately 141 GiB free at preflight.
`/dev/kvm` is readable/writable by patrick; KVM API version 12 and VM creation
passed. This establishes KVM access, not guest boot or acceptance readiness.

The existing Plasma 6.6.6 X11 session renders at 1920x1080. Its forced HDMI-A-1
EDID arguments are active in the current boot, firmware is present in initramfs,
and GRUB/hook syntax checks passed. Existing automatic X11 login and the VNC
sleep/idle inhibitor were preserved. TigerVNC now listens only on loopback;
direct LAN VNC is refused. Its `.72` wall tile passed mouse/keyboard checks and
was returned to view-only. No sharing-portal approval is needed for this X11 session.

Native build bootstrap uses the committed `909f8ee` source archive, standard
distro packages and the verified official AWS SDK 1.11.907 dependency lock.
Exact source/tool versions, build outcome and access/display evidence are retained
privately in `work/remote-builders/k72/`. This setup did not perform a fresh reboot,
guest boot, graphical put.gd acceptance or a memory campaign.
