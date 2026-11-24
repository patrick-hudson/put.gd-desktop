# Verification record

> **Historical record.** This is a dated run record, moved unchanged from [implementation evidence](../../testing/verification.md) on 2026-11-05 (#1222). It describes what that run found for the build it names, and its open or pending statements describe that time. "Above" and "below" refer to its neighbours on the old page, in the order of the [history index](../README.md#verification-run-records). For today, see the [current state](../../testing/verification.md#current-state). Do not use it as a work list.

## AppImage window icon on Plasma (#523) — 2026-10-23

Lab code: [#530](https://github.com/patrick-hudson/put.gd-desktop/pull/530).
Evidence: [2026-10-23-appimage-icon-523](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-23-appimage-icon-523/README.md).

- **Cause.** The neon smoke runs' "W" icon was a lab artifact. `lab.py`'s
  private `XDG_DATA_HOME` hid AppRun's `putgd-ui.desktop` from KWin, and the
  1058 AppImage's Qt 6.8.3 can't send the icon itself.
- **Before and after.** On k72 (Plasma 6.6.6), d69 (Plasma 6.3.6) and a neon
  User 6.8.1 VM: 1058 showed "W" in the dashboard's and the editor's title bars.
  The Qt 6.12.0 AppImage (`26.1023.1101-dev+gaa047473ad2e`) showed the put.gd
  icon in the same setup, sending it over `xdg-toplevel-icon-v1`. 1058 with the
  real data home showed the put.gd icon too.
- **Lab fix.** For AppImage profiles, the runner links the private data home's
  `applications/` to the session's. In a fresh neon overlay, 1058 run through
  `lab.py` showed the put.gd icon and captured the desktop through KWin
  ScreenShot2.
- **Capture risk.** On k72 (KWin 6.6.6), 1058 with the private data home failed
  desktop capture with `capture_permission`, and passed with the real one.

Not covered: neon Unstable, and f44/m74 (no app change). Follow-up: launches
whose `XDG_DATA_HOME` differs from the compositor's lose KDE capture
([#531](https://github.com/patrick-hudson/put.gd-desktop/issues/531)).
