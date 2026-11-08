# Verification record

> **Historical record.** This is a dated run record, moved unchanged from [implementation evidence](../../testing/verification.md) on 2026-11-05 (#1222). It describes what that run found for the build it names, and its open or pending statements describe that time. "Above" and "below" refer to its neighbours on the old page, in the order of the [history index](../README.md#verification-run-records). For today, see the [current state](../../testing/verification.md#current-state). Do not use it as a work list.

## AppImage Wayland window icons — 2026-10-19

The [Wayland window icon receipt](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-19-appimage-wayland-icons.md)
records #101 (issue #88): candidate CI AppImage `930d228`, sha256 `98bba655…`,
against nightly `9042776`. Both ran from the image with nothing installed.

- In a Wayland session AppRun writes `putgd-ui.desktop` and
  `putgd-record.desktop` while the image runs. On Debian 13 Plasma 6.3.6
  Wayland, KWin's title bar and task switcher then showed the put.gd icon. On
  Fedora 44 GNOME 50 Wayland, the overview and dash showed it. The baseline
  showed generic icons on both.
- Both desktops: no entry was left after a normal quit, and the next launch
  removed the entries of a SIGKILLed launch.
- With the put.gd Shell extension (temporary user copy) on GNOME, window
  capture of the native Settings and History windows was refused
  (`capture_target`), and an external window was captured.
- Not covered:
  - The editor's separate `putgd-ui --edit-image` process is not refused on
    GNOME. This is independent of native windows and tracked in #103.
  - Recorder windows were not opened.
  - KWin 6.6 Wayland was not run.
