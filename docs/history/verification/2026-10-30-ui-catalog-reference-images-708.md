# Verification record

> **Historical record.** This is a dated run record, moved unchanged from [implementation evidence](../../testing/verification.md) on 2026-11-05 (#1222). It describes what that run found for the build it names, and its open or pending statements describe that time. "Above" and "below" refer to its neighbours on the old page, in the order of the [history index](../README.md#verification-run-records). For today, see the [current state](../../testing/verification.md#current-state). Do not use it as a work list.

## UI catalog reference images (#708) — 2026-10-30

Every `putgd-ui-catalog` state except the exempt ones is now compared with a reference image in CI (see
[the UI catalog](../../design/ui-catalog.md)). The references were rendered on k72 in an
`ubuntu:26.04` container built from the CI leg's package list (Qt 6.10.2).
Making renders reproducible took a stopped clock, fixed data paths, a fixed
build identity, waits for History's thumbnails, a fixed busy-indicator phase, a
steady cursor, scrolling from the top, and grabs repeated until two frames
match. Before that, two renders of one build differed in 13 to 54 states, and
in about 100 per scheme on CI. Validation ran four renders at once on k72's
four cores. After the last regeneration, every stressed round matched 218
states with 1 exempt, in both schemes. CI run 41007500724 then reproduced
them, with 117 of 117 tests passing first time.
`51a-settings-conflict-compact-update` is exempt from image comparison: the
General page header can keep a stale height after a resize, in 5 of 36
stressed light renders (#778). An earlier CI run passed `ui_catalog_light`
only on retry (`main-mixed-outputs`, #793) before its capture was made steady.
This is synthetic widget evidence, not desktop acceptance.
