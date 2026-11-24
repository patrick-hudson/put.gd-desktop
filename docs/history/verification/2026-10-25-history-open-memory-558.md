# Verification record

> **Historical record.** This is a dated run record, moved unchanged from [implementation evidence](../../testing/verification.md) on 2026-11-05 (#1222). It describes what that run found for the build it names, and its open or pending statements describe that time. "Above" and "below" refer to its neighbours on the old page, in the order of the [history index](../README.md#verification-run-records). For today, see the [current state](../../testing/verification.md#current-state). Do not use it as a work list.

## History-open memory (#558) — 2026-10-25

The History-only helper now defers hidden Settings/destination construction,
while retaining active drafts and accepted setup inspection. Controlled builder
measurements and lifecycle tests are recorded in the
[History memory evidence](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-25-history-open-558/README.md).
The exact local development DEB passes physical Cinnamon 300/30/300 memory
(46,641,152 B History-open family PSS) and separate targeted native UI checks.
All 107 configured dev tests pass across two invocations; four minimum-Qt suites
also pass. Parallel Cinnamon/GNOME/KDE acceptance, paint-response timing and
published assets remain pending; this evidence does not close #558.
