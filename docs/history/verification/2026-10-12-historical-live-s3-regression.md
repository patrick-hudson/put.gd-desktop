# Verification record

> **Historical record.** This is a dated run record, moved unchanged from [implementation evidence](../../testing/verification.md) on 2026-11-05 (#1222). It describes what that run found for the build it names, and its open or pending statements describe that time. "Above" and "below" refer to its neighbours on the old page, in the order of the [history index](../README.md#verification-run-records). For today, see the [current state](../../testing/verification.md#current-state). Do not use it as a work list.

### Historical live-S3 regression — 2026-10-12

Runtime `0.1.1-dev.114+g0258600b752d` passed all three native/GUI/live-S3 regression
workloads and exact production cleanup. Five-minute RSS+swap peaks were 29.059 MiB
on Cinnamon, 36.078 MiB on GNOME and 31.285 MiB on KDE, with zero swap/remaining
helpers and idle CPU below 0.0201% of one core. All 105 synthetic objects in that campaign
(including the targeted KDE 12) were uploaded, independently verified and removed
through production. No SDK error, automatic retry or emergency cleanup occurred.

[The transport report](../testing-live-s3-transport.md) and
[historical live-S3 replay](../../evidence/live-s3-memory/source114/README.md) retain exact
identities, parameters, historical failures and qualifications. One current full
regression per desktop does not claim a fresh-repeat optimization decision or
full release/fresh-agent acceptance. Documentation commits do not rename the
measured binaries.
