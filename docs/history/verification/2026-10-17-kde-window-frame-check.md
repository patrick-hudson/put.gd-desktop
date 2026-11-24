# Verification record

> **Historical record.** This is a dated run record, moved unchanged from [implementation evidence](../../testing/verification.md) on 2026-11-05 (#1222). It describes what that run found for the build it names, and its open or pending statements describe that time. "Above" and "below" refer to its neighbours on the old page, in the order of the [history index](../README.md#verification-run-records). For today, see the [current state](../../testing/verification.md#current-state). Do not use it as a work list.

## KDE window frame check — 2026-10-17

The [KDE window frame receipt](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-17-kde-window-frame.md) records
the decision to keep frame-inclusive window capture on Plasma/Wayland, and the
lab fix. The runner now asks KWin for the frame on any observed KDE Wayland
session, not only a profile named `kde-wayland`. Window runs pass 2/2 on
physical Debian Plasma 6.3 (642×449) and 2/2 on the Kubuntu Plasma 6.6 VM
(640×448), each matching KWin's own frame geometry.
