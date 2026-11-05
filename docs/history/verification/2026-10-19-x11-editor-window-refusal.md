# Verification record

> **Historical record.** This is a dated run record, moved unchanged from [implementation evidence](../../testing/verification.md) on 2026-11-05 (#1222). It describes what that run found for the build it names, and its open or pending statements describe that time. "Above" and "below" refer to its neighbours on the old page, in the order of the [history index](../README.md#verification-run-records). For today, see the [current state](../../testing/verification.md#current-state). Do not use it as a work list.

## X11 editor window refusal — 2026-10-19

The [X11 editor refusal receipt](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-19-x11-editor-refusal.md)
records #106 (issue #103, the X11 half). The candidate was `85d04f0`, packaged by
manual CI run 39266644046: the Ubuntu 26.04 DEB (sha256 `0b8aa778…`) and the
AppImage (sha256 `d14dc770…`).

- On Kubuntu 26.04 Plasma X11 (k72), window capture of the Edit & upload editor
  was refused (`capture_target`) from both builds. The editor was the only
  put.gd window, and its `putgd-ui --edit-image` process was a direct child of
  the resident.
- Settings and History were still refused, and an external `kdialog` window was
  still captured.
- `x11_isolated` and `x11_native_snapshot` add an unregistered resident-child
  case. Both fail against nightly's X11 sources and pass with the change, on d69.
- Not covered:
  - a Cinnamon desktop (the core route ran on Xvfb only);
  - recorder windows;
