# Verification record

> **Historical record.** This is a dated run record, moved unchanged from [implementation evidence](../../testing/verification.md) on 2026-11-05 (#1222). It describes what that run found for the build it names, and its open or pending statements describe that time. "Above" and "below" refer to its neighbours on the old page, in the order of the [history index](../README.md#verification-run-records). For today, see the [current state](../../testing/verification.md#current-state). Do not use it as a work list.

### Live selection and post-use cleanup (2026-10-08)

The region selector now leaves the desktop live while drawing its white outline.
Native Cinnamon proof changed the independent fixture color during dragging,
then verified both normal and rapid-release saved pixels and independent Qt paste
contained the final color without the outline. Escape and right-click cancellation
preserved clipboard text. The single-head record is in
[live-region evidence](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-08-live-region.json); fresh GNOME/KDE
graphical proof, mixed displays and the physical latency gate remain open.

After five local capture/clipboard cycles, production idle heap reclamation
finished at 66.473 MiB in the disposable Cinnamon lab, versus 75.648 MiB in the
earlier attribution run. It performs one glibc pass after work drains, retaining
live UI and native clipboard state; the longest observed call was 750 microseconds.
This comparison uses different binary identities and establishes neither cold
40 MiB acceptance nor post-S3 savings. A test-first cancellation fix also covers
acquisition that never enqueues a payload. Final coherent development tests pass
25/25 (81.39s); affected operation ASan/UBSan with leak detection passes 1/1
(24.70s), and instrumented-Qt TSan passes 1/1 (26.34s), with no new suppression.
[The cleanup record](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-08-heap-reclaim.json) retains source identities,
complete controlled samples, JUnit reports and the cancellation test delta.

A test-first lab-finalizer correction also verifies that input cleanup failures do
not skip restoring the original resident/configuration. Five actual-finalizer AST
fault cases pass, including restoration failure and startup that never completed.
Cleanup failures are saved as failed runs and still propagate. This is synthetic
orchestration proof; the prior native payload measurement remains separately labeled.
