# Verification record

> **Historical record.** This is a dated run record, moved unchanged from [implementation evidence](../../testing/verification.md) on 2026-11-05 (#1222). It describes what that run found for the build it names, and its open or pending statements describe that time. "Above" and "below" refer to its neighbours on the old page, in the order of the [history index](../README.md#verification-run-records). For today, see the [current state](../../testing/verification.md#current-state). Do not use it as a work list.

## History gallery blanking, scrolling and gap crash (#1151) — 2026-11-02

[Evidence](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-11-02-history-gallery/README.md): with `v26.1102.3796-nightly` on Mint
Cinnamon X11, a desktop recording blanked 15 or more of about 45 gallery tiles at its worst and resting the pointer
between tiles crashed `putgd-ui`. With PR #1151's DEB at `3f5d0b0da071`, no sampled frame (about 4 a
second) showed a blanked tile and the crash was gone. `putgd-ui` CPU per wheel notch fell from 144.5 ms to 59.2 ms. The scroll
paint test also passes on live GNOME Wayland; Plasma wasn't run. Synthetic history, not live storage.
