# Verification record

> **Historical record.** This is a dated run record, moved unchanged from [implementation evidence](../../testing/verification.md) on 2026-11-05 (#1222). It describes what that run found for the build it names, and its open or pending statements describe that time. "Above" and "below" refer to its neighbours on the old page, in the order of the [history index](../README.md#verification-run-records). For today, see the [current state](../../testing/verification.md#current-state). Do not use it as a work list.

### Memory continuation (2026-10-08)

The final reviewed SDK module completed the same-observer five-minute physical
Cinnamon cold workload at 54,407,168 bytes (51.887 MiB), compared with 59,002,880
bytes (56.270 MiB) for its reference repeat. CPU was 0.003331% of one core, with
zero operations and no observed mapped owned window. The 7.789% improvement is
retained at optimization revision `cadd9ed` and integrated into canonical source.
The original 51.996 MiB candidate result is preserved rather than replaced.
Native Cinnamon fonts and dialogs remain enabled; approximately 40 MiB is unmet.

The actual review's two P2 findings were fixed: package mismatch errors explain
restart recovery, and wrong-build/wrong-transport rejection fixtures fail when
the corresponding guards are removed. Eight affected SDK contracts pass in
Dev, Release, ASan and TSan; installed-package checks pass. The combined canonical
SDK/live-region/cleanup development suite passes 26/26 (112.90 seconds).
[The SDK record](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-08-sdk-memory.json) retains exact source, binaries,
review and raw measurements. Third-party uninstrumented library limits remain.

Three empty-view cycles on the reviewed binary finish at 79.551 MiB; no operations
or cloud requests occurred. This is a view-lifecycle diagnostic, not a permanent
plateau or leak proof. The module remains pinned after first S3 use, so cold gains
do not establish post-S3 memory savings. The [replay instructions](../testing-memory.md)
describe private inputs, timing, logs and the physical-machine/CI boundary.
