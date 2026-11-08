# Verification record

> **Historical record.** This is a dated run record, moved unchanged from [implementation evidence](../../testing/verification.md) on 2026-11-05 (#1222). It describes what that run found for the build it names, and its open or pending statements describe that time. "Above" and "below" refer to its neighbours on the old page, in the order of the [history index](../README.md#verification-run-records). For today, see the [current state](../../testing/verification.md#current-state). Do not use it as a work list.

## Single-name configuration and data folders — 2026-10-18

The [issue 56 receipt](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-18-issue56-storage-locations.md) records
candidate `d37a975`:

- Workstation dev suite 73/73 on `d37a975` (671 s). The first candidate, `d7366b8`, failed
  `history_contract` (a null `previewPath` added to schema-1 records); `d37a975`
  fixes it. The new storage-locations suite covers a
  fresh install, an upgrade that keeps settings, history and failed or interrupted
  uploads, both roots in use, a stop before every rename, staged conflicts, linked
  and shared roots, and the real resident and CLI.
- Native Cinnamon/X11 on the workstation desktop: a profile written by the
  pre-fix build `625b7ff` moved on first start. Settings showed the new
  configuration path and `migrated` diagnostics. History kept both captures and
  the moved preview.

GNOME, KDE, build-server and package-upgrade runs were not part of this change;
package upgrade acceptance stays with issue #44.
