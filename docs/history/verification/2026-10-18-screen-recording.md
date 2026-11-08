# Verification record

> **Historical record.** This is a dated run record, moved unchanged from [implementation evidence](../../testing/verification.md) on 2026-11-05 (#1222). It describes what that run found for the build it names, and its open or pending statements describe that time. "Above" and "below" refer to its neighbours on the old page, in the order of the [history index](../README.md#verification-run-records). For today, see the [current state](../../testing/verification.md#current-state). Do not use it as a work list.

## Screen recording — 2026-10-18

The [issue 50 receipt](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-18-issue50-screen-recording.md) records
candidates `8252f03` (X11 capture, review, export, tray) and `625b7ff` (Wayland
acquisition) on pull request #68:

- Full suites on the Kubuntu, Fedora and Debian builders and the workstation
  (70–73 tests each, all green), plus pull request CI on the head.
- Capture scenarios on the Cinnamon guest, the GNOME and KDE Wayland guests and
  the physical Kubuntu, Fedora and Debian desktops; the recording flow with a
  moving pattern on the Cinnamon guest, the workstation and the physical
  Kubuntu desktop (motion 0.48–0.63 against a 0.012 static control).
- Wayland: the portal with one picker click, then restore tokens and the
  KWin/Mutter native streams reaching `ready` with no input on the physical
  Debian Plasma and Fedora GNOME desktops, clips carrying the pattern.

Microphone capture and AMD GPU export were uncovered in this recording checkpoint; the Wayland
guests were rerun by the follow-up above.
