# Linux acceptance coverage and execution placement

The required Linux test distributions are **Linux Mint, Ubuntu, Kubuntu, Fedora
and Debian**. Cover Cinnamon plus GNOME and KDE/Plasma, and real X11 and Wayland
sessions. Use a combination of the physical build servers and VMs that provides
this coverage on the exact candidate being accepted. This document sets required
coverage; it does not establish that any row has passed.

## Distribution coverage

| Distribution | Desktop/session baseline | Preferred execution placement |
| --- | --- | --- |
| Linux Mint | Cinnamon/X11 | Fresh VM on a capable build server; physical Mint for gates requiring physical Cinnamon; local VM fallback when needed |
| Ubuntu | GNOME/Wayland, with GNOME/X11 coverage on a release that supplies it | Builder-hosted VM of the required Ubuntu version, or an appropriate physical Ubuntu machine |
| Kubuntu | KDE/Plasma Wayland, with KDE/X11 coverage on a release that supplies it | Builder-hosted VM of the required Kubuntu version, or an appropriate physical Kubuntu machine |
| Fedora | GNOME/Wayland on the existing Fedora 44 server; other declared variants need their matching session/profile | Direct Fedora server desktop when its version matches the gate; VM for missing variants/versions |
| Debian | KDE/Plasma Wayland on the existing Debian 13 server; other declared variants need their matching session/profile | Direct Debian server desktop when its version matches the gate; VM for missing variants/versions |

Across that distribution coverage, require GNOME/Wayland, GNOME/X11,
KDE/Wayland and KDE/X11 results, as well as Mint Cinnamon/X11. Use native distro
sessions and supported packages. This is coverage across supported combinations;
it does not assume that every distro version ships every desktop/session pair.
Choose and record the exact distro release for each row before a campaign.

The current checked-in VM profiles are `cinnamon-x11`, `gnome-wayland`,
`kde-wayland` and `fedora-gnome-wayland` under `tools/lab/profiles/`. They do not
provide GNOME/X11, KDE/X11 or a Debian-specific automated profile. Provision the
missing VM/profile or use an appropriate native machine and retain manual
acceptance evidence. Do not relabel an existing profile as another distro or
session. An unavailable required combination remains a recorded gate.

XWayland or `QT_QPA_PLATFORM=xcb` inside a Wayland session does not establish a
native GNOME/X11 or KDE/X11 result. Verify the actual login session/compositor
and Qt platform, along with the OS and desktop versions, in the evidence.

## Choose physical hardware or a VM for each gate

Prefer Patrick's [Fedora and Debian build servers](remote-builders.md), including
their own visible GNOME/Plasma desktops. Build and test directly there when the
host matches the required release/desktop/session. Provision a fresh VM on a
capable builder for missing distributions, versions or sessions. Discover/copy
[immutable golden inputs](remote-golden-images.md) and expose each remote guest
on the [workstation display wall](remote-vm-displays.md). Use local VMs when the
builders cannot run the profile, and record the fallback reason.

**A release gate that requires physical/native hardware must run on the
appropriate physical machine.** A VM result cannot close that gate. Mark the
required form factor before testing; record physical gates as blocked until the
appropriate machine is available. Do not downgrade hardware, display topology,
acceleration or compositor requirements to fit an available VM. VM functional
coverage and physical release evidence remain separately identifiable.

Check KVM, capacity and visible session access on the actual VM host before
launch. A server can still run direct desktop acceptance while its VM readiness
is blocked. Preserve the user's existing host projects/windows; use isolated
payloads, settings and test destinations.

## Ordering, evidence and status

Keep the existing Cinnamon-first functional/memory gate for a new candidate and
the explicit KDE resident-memory experiment exception in [AGENTS.md](../AGENTS.md).
Expand to the other rows after the required first gate passes. A scoped user
waiver applies only to its named task/gate and does not certify the full matrix.

Use the same exact accepted source/build candidate across the declared matrix.
For every result retain distro/release, desktop/compositor version, X11/Wayland
session, Qt platform, physical machine or VM identity, acceleration, installed
and running build identities, scenario parameters, outcome and blocked gates.
Keep graphical interactions visible on the wall and save screenshots/recordings,
including failures, alongside independent artifact/paste witnesses as required.
SSH logs, offscreen tests, a live tile or historical acceptance cannot certify a
new row/candidate. Existing performance, geometry and fresh-agent release gates
remain in [verification](verification.md); preserve their historical evidence.
