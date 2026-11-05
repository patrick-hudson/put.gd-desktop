# Verification record

> **Historical record.** This is a dated run record, moved unchanged from [implementation evidence](../../testing/verification.md) on 2026-11-05 (#1222). It describes what that run found for the build it names, and its open or pending statements describe that time. "Above" and "below" refer to its neighbours on the old page, in the order of the [history index](../README.md#verification-run-records). For today, see the [current state](../../testing/verification.md#current-state). Do not use it as a work list.

## Wayland recording regions and system audio — 2026-10-18

The [Wayland region receipt](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-18-wayland-recording-region.md)
covers the follow-up on `claude/wayland-recording-region`: region selection
before the stream (KDE layer-shell selector, GNOME Shell extension), ring,
dimming and bar placement with `overlayPlacement`, and the synthetic A/V-sync
check. Verified with a moving pattern and each clip's motion score on the
physical Debian Plasma 6.3 and Fedora GNOME 50 Wayland desktops, the
Ubuntu 26.04 GNOME, Kubuntu 26.04 Plasma 6.6 and Fedora 44 GNOME KVM guests,
and system audio through the speaker and headphone ports on the physical
Kubuntu, Debian and Fedora desktops (headphone route not measurable on
Fedora, where WirePlumber moves the default sink). Microphone
capture and listening tests with real headphones remain uncovered.
