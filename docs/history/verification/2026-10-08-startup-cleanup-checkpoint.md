# Verification record

> **Historical record.** This is a dated run record, moved unchanged from [implementation evidence](../../testing/verification.md) on 2026-11-05 (#1222). It describes what that run found for the build it names, and its open or pending statements describe that time. "Above" and "below" refer to its neighbours on the old page, in the order of the [history index](../README.md#verification-run-records). For today, see the [current state](../../testing/verification.md#current-state). Do not use it as a work list.

### Startup cleanup checkpoint — 2026-10-08

Successful startup now requests one pass through the existing two-second idle
cleanup gate on glibc. Failed initialization requests none; active workers,
uploads, selectors and shutdown retain their existing protections. Three new
success-path assertions failed before the change; the failed-startup test passed.
The reviewed change is kept as `68e5549` and integrated as `fc69d38`.

The unchanged five-minute physical Cinnamon workload fell from 46.1875 to
44.9648 MiB, a 1.2227 MiB saving. CPU was 0.0133% of one core. The predeclared
1 MiB threshold and existing idle budgets pass; the 40 MiB target does not.
Native GTK remains mapped. Three empty-view cycles reopened Settings, History
and Activity and ended at 73.8242 MiB hidden RSS; that is a distinct post-use
measurement, not cold idle or a long-session plateau.

Integrated development tests pass 28/28 (144.57s). The affected operations
contract passes Debug and Release, ASan/UBSan with leak checking (37.56s), and
instrumented-Qt TSan with unchanged suppressions (39.47s). These are bounded
application checks, not new desktop/cloud or whole-dependency certifications.
[Complete evidence](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-08-startup-reclaim.json) preserves exact
source/binary identities, tests, review and both physical workloads.
