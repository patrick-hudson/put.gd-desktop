# Verification record

> **Historical record.** This is a dated run record, moved unchanged from [implementation evidence](../../testing/verification.md) on 2026-11-05 (#1222). It describes what that run found for the build it names, and its open or pending statements describe that time. "Above" and "below" refer to its neighbours on the old page, in the order of the [history index](../README.md#verification-run-records). For today, see the [current state](../../testing/verification.md#current-state). Do not use it as a work list.

## GNOME editor window refusal — 2026-10-19

The [GNOME editor refusal receipt](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-19-gnome-editor-refusal.md)
records #104 (issue #103), from pull request CI merge `3a9438c`. The Fedora 44
RPM (sha256 `ba4d1774…`) and the AppImage (sha256 `3a4cae53…`) both ran with
the RPM's Shell extension, version 2, loaded.

- On Fedora 44 GNOME 50 Wayland, window capture of the Edit & upload editor
  was refused (`capture_target`) from both builds. The editor was the only
  put.gd window. In both builds its `putgd-ui --edit-image` process was a
  direct child of the resident.
- Settings and History were still refused, and an external window was still
  captured.
- The GJS ownership contract adds the editor case. It fails against nightly's
  extension.
- Not covered:
  - recorder windows;
  - the X11 adapters, which still don't refuse the editor;
