# SQLite rollback/FULL locating results — 2026-10-08

Keep IPC work independent. This standalone experiment found no consistent end-to-end latency improvement from retaining a connection or reusing one prepared statement. FULL-sync commit tails dominate. A smaller page-cache cap merits a separate populated-history memory experiment, but these measurements do not establish product savings, 40 MiB or the 150 ms selector gate.

All twelve fresh-process cases completed: two repeats of reopen/connection/statements at 2000/512 KiB, forward then reversed order, five warmups plus thirty scored iterations each (420 raw, 360 scored). Each iteration committed synthetic accepted/acquiring/cancelled states. Every case reported DELETE journal mode, synchronous=2 (FULL), valid final states and a passing integrity check; stderr was empty. No failed timing attempt was omitted. Total controller wall interval was about 12.54 seconds, far below the ten-minute cap. No WAL, product data, configuration, desktop/VM or clipboard work occurred.

| Cache KiB / case | Median iteration ms, repeats 1 / 2 | p95 iteration ms, repeats 1 / 2 |
| --- | --- | --- |
| 2000 / reopen | 6.408 / 6.787 | 268.261 / 99.060 |
| 2000 / connection | 7.669 / 6.791 | 11.035 / 9.171 |
| 2000 / statements | 8.366 / 7.336 | 94.604 / 174.884 |
| 512 / reopen | 6.318 / 9.431 | 9.268 / 97.742 |
| 512 / connection | 6.407 / 7.795 | 92.401 / 96.061 |
| 512 / statements | 8.292 / 6.234 | 178.171 / 94.225 |

These are three durable journal writes in a simplified synthetic schema, not hotkey/paint timings. Tail rankings vary between matched repeats; selecting the single lowest p95 would be unsound. Explicit prepare median summed over three states was 0.062–0.202 ms in non-reuse cases, versus zero during scored statement-reuse iterations. The largest scored per-iteration commit sum was 347.065 ms. These results locate parsing as a small cost in this workload; large item/deletion loops remain unmeasured.

Each case also ran a separate controlled writer on its own Qt SQL connection. It held BEGIN EXCLUSIVE for an injected 100 ms before committing. The following connection's user_version read waited 103.892–229.410 ms, while open peaked at 0.102 ms and chmod at 0.017 ms. Writer hold-through-commit ranged 103.422–182.924 ms. This reproduces the lock-wait mechanism specifically at user_version, and is consistent with the earlier product cancellation-commit overlap. It does not prove that every millisecond in the product's 729.438 ms permission/initialization boundary has that cause. The following product accepted/acquiring write could still wait even if the initialization read did not.

In six fresh processes per cache setting, normalized first-scan RSS growth for a separate 10,000-row/256-byte synthetic scan was 2064–2104 KiB (median2098) at cache2000 and 548–552 KiB (median552) at cache512: median difference1546 KiB, about1.510 MiB. That is consistent enough to justify measuring a capped cache in the actual paginated history workload. It is not cold idle savings or SQLite-owned allocation accounting; Qt query/result allocations and glibc retention can contribute. Scan times were noisy, and repeating after shrink raised RSS again.

shrink_memory reduced sampled RSS by2020 KiB in one case and zero in eleven. Do not add global cache purges on that evidence. The actual compile-options list lacks ENABLE_MEMORY_MANAGEMENT, so the optional global sqlite3_release_memory route is unsupported by the documented build condition; connection-local shrink remains available. No glibc trim was performed here.

Cheapest next work: keep startup free-page reclamation separate; add the transient permission/individual-PRAGMA product markers before a journal behavior change. Bounded statement reuse can still help genuine batch loops, but will not remove the measured lock/fsync tail. A persistent journal-only owner needs its own lifecycle/concurrency evidence and must not execute upload/deletion waits which depend on checkpoint callbacks. Compare a small page-cache cap only with real bounded history paging, retained thumbnails and repeated reads. Keep WAL conditional on verifying the distro correction and private sidecar/recovery behavior; it was not executed.

Build/source remained unchanged: probe SHA256 2e6ebb66aaf667018027e5c87f411d952b8d748bea14a478b477d00ce1150c2a; binary d4419f7c36fee1b21fd6435f72ff1d36946ec4b86432880e7bcf0605c89051fb. Actual loaded Qt6.4.2, SQLite3.45.1 source ID2024-01-30 16:01:20 e876e51a0ed5c5b3126f52e532044363a014bc594cfefa87ffb5b82257ccalt1. Per-process loaded-library hashes, exact order/arguments/PIDs and parameters are in raw/manifest.json; every complete sample, raw result and empty stdout/stderr log is preserved under raw/, with hashes in raw-hashes.json. Synthetic databases remain private at /tmp/putgd-sqlite-latency-rollback-20261008T193548Z-ee5c51ea and were not copied into this evidence packet.

Known probe limitation, preserved without repair/rerun: reader beginNs is reused by the following write's begin duration, so an absolute reader-start timestamp is unavailable. Individual phase durations and writer timestamps remain valid. The source is not a migration/recovery/concurrent-publication implementation or a power-loss test. SQL/Qt process RSS cannot certify native app budgets. No production changes, new dependencies or threshold changes; all jobs are terminal and root owns the physical slot.

[Structured summary](summary.json), [build receipt](build/build-identity.json), [Official SQLite sources](official-sources.json).
