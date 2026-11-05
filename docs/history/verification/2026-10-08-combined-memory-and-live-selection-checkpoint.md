# Verification record

> **Historical record.** This is a dated run record, moved unchanged from [implementation evidence](../../testing/verification.md) on 2026-11-05 (#1222). It describes what that run found for the build it names, and its open or pending statements describe that time. "Above" and "below" refer to its neighbours on the old page, in the order of the [history index](../README.md#verification-run-records). For today, see the [current state](../../testing/verification.md#current-state). Do not use it as a work list.

### Combined memory and live-selection checkpoint — 2026-10-08

The Linux native IPC implementation is committed as `1fdfff0`; live transparent
selection and one-pass image-heap reclamation are committed as `2e789b8`. Source
closure checks match the earlier full/follow-up reviews and the separate IPC
review, including the successful Claude retry and reproduced disconnect fix.
The combined Debug suite passed 28/28 in 127.35 seconds; the four affected
Release contracts passed in 46.79 seconds. Separate IPC ASan/UBSan and
instrumented-Qt TSan runs each passed 4/4; those are not a new whole-tree
sanitizer certification.

The isolated fixed IPC source measured 46.020 MiB against a paired 52.012 MiB
reference over five minutes. The committed combined
source measured 46.188 MiB over another 300 seconds at 0.00666% of one core,
without operations or owned window activation. Full identities are recorded
in [the IPC evidence](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-08-local-ipc.json). Keep the
40 MiB target, post-S3/populated-history memory and full desktop acceptance open.
The temporary region trace still fails the unchanged 150 ms warm p95 gate at
160.004 ms; it points to journal initialization overlapping a prior cancellation
commit, with individual permission/PRAGMA waits not yet resolved.
