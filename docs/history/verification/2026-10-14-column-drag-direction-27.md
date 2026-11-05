# Verification record

> **Historical record.** This is a dated run record, moved unchanged from [implementation evidence](../../testing/verification.md) on 2026-11-05 (#1222). It describes what that run found for the build it names, and its open or pending statements describe that time. "Above" and "below" refer to its neighbours on the old page, in the order of the [history index](../README.md#verification-run-records). For today, see the [current state](../../testing/verification.md#current-state). Do not use it as a work list.

## Column drag direction — issue #27 (2026-10-14)

Activity and History previously used a stretched first column. Growing another
column took the same width from that first column, moving the target's left edge
against the pointer; the first column itself was not interactive. Both views were changed to
use the then-named `VisualStyle::configureColumns` sizing on their existing Qt headers.
It fits the first column when the
viewport changes width, preserving the compact layout, but leaves column drags
to Qt without redistributing their width. Native minimum widths still apply.

On fresh `main` base `7e5a6a0`, the new pointer regression failed all 20 cases
before the implementation change. It then passed for both directions on all five
visible columns in each view, including the first and last columns and drags past
the minimum width. The final regression also checks two positions during a held
drag, then the released boundary. The existing compact History test covers
window resizing and the receipt inspector at 1024 and 1280 logical pixels.

The standard development configure and full build passed. `ctest --preset dev`
passed all 59 tests in 452.34 seconds, including the complete Settings, Activity,
and History GUI suites. `git diff --check` passed.

Local build and test logs are retained in `/tmp/p27v`; the initial configure and
regression build logs are `/tmp/p27-configure.log` and `/tmp/p27-build-red.log`.
Tests use synthetic local-only payloads, distro Qt 6.4.2,
`XDG_RUNTIME_DIR=/tmp/p27v/r`, and `TMPDIR=/tmp/p27v/t`. Socket preflight passed
393 fixed paths/equal-length randomized-path templates, with a maximum of 77
UTF-8 bytes. Widget tests use the offscreen platform; they do not establish
compositor or memory acceptance. Current-candidate Cinnamon functional/memory
acceptance and subsequent GNOME/KDE regressions remain pending. No VM base was
booted or changed for this slice.

Correctness review caught that replacing Activity's header disabled its native
column reordering. A failing native-defaults comparison is retained in
`/tmp/p27v/header-defaults.txt`. The final implementation configures the existing
header instead, preserving movable/clickable flags; the regression checks them.
The full rebuild and all six GUI suites passed after this correction
(`ctest --preset dev -R '^gui_'`, 68.71 seconds). The 59-test result above
precedes this correction; only the affected GUI suites were repeated.
