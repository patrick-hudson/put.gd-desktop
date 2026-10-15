# Proton P2: runtime resources and optimization — 2026-10-15

P2 is complete for this recorded Cinnamon workload. The supported runtime recipe
is unchanged: a smaller-heap experiment increased active memory and CPU, so it
was reverted. [Machine receipt](2026-10-15-proton-p2-resources.json) retains the
identities, stage measurements, sampler limits, controls and cleanup results.
P3–P6 remain open; this is not final packaged three-desktop acceptance.

The user chose practical optimization without a hard total active-memory ceiling
on 2026-10-15. The pre-run **512 MiB** provisional envelope remains a diagnostic
reference, **not a release gate**. The frozen 40 MiB post-use idle limit, CPU below
0.5% of one core, no swap and no retained helpers remain unchanged.

## What ran

The baseline was clean Release `e558436e30c96d0e68fea14d9429e5ed2b811eaf`,
`0.1.1-dev.188+ge558436e30c9`, with runtime SHA-256
`93fc77e63913dd39075a7ede049289325fb7c16a3aaf20985bc62adbc25b0e99`.
The experiment was clean Release `7f42761d249c281ca708c2e614cfb44809584344`,
`0.1.1-dev.190+g7f42761d249c`, runtime
`843833ec8d98fe55340211158380a84975c7f5d4233678e959946930e14d58fd`.
Both used SDK 0.22.2, Crypto 2.0.0, Bun 1.4.2 and upstream
`8ffae007b0fbb8bcef53514719d6f093bb32dcf9`.

Tests ran in the visible Cinnamon/X11 KVM session (2 vCPU, 6 GiB, dynamic distro
Qt 6.4.2), using private Release executable deployments and independently signed-in
Proton sessions. Each run used a new owned demo folder, one admitted upload and
synthetic files only. **No S3 service access, tests, configuration or object
changes occurred.** Common preset builds compiled existing repository targets;
no S3 module or configuration was deployed into the test environment.

Each run exercised a local-only negative control, fresh-resident account
inspection, two 1 MiB files, two 128 MiB files with Activity visible briefly,
cancellation of two 64 MiB files after actual SDK byte progress, explicit retry
of that original operation, and three more two-file 1 MiB cycles. The small-file
deadline was 120 seconds; large/cancel/retry used 600 seconds. Helpers had to drain
within 15 seconds, followed by 30-second settled observations. Two separate
300-second post-use windows used the unchanged five-second idle sampler.

Both runs passed upload/share, native exact-link paste, cancellation without an
unconfirmed link, same-operation/publication retry and repeated-use checks.
Each process trace accounted for **39 SDK helpers and one UI helper**; every
child was observed after exec, sampled and reaped. The local-only action started
zero SDK helpers. No swap or escaped/retained product helper was observed.

## Baseline measurements

All values below cover the resident plus product descendants. Memory is MiB;
CPU seconds are sampled process time, not wall time.

| Workload | RSS peak | PSS peak | Private peak | Sampled CPU seconds |
| --- | ---: | ---: | ---: | ---: |
| Account inspection | 101.26 | 79.66 | 73.64 | 0.80 |
| 2 × 1 MiB | 173.26 | 151.55 | 145.53 | 6.98 |
| 2 × 128 MiB + brief Activity | 487.25 | 465.53 | 459.37 | 20.69 |
| Cancel 2 × 64 MiB after real bytes | 335.46 | 313.55 | 307.39 | 7.12 |
| Retry original operation | 330.36 | 308.07 | 302.21 | 12.80 |
| Repeat 1: 2 × 1 MiB | 177.15 | 155.22 | 149.06 | 8.68 |
| Repeat 2: 2 × 1 MiB | 179.23 | 157.40 | 151.24 | 7.56 |
| Repeat 3: 2 × 1 MiB | 178.32 | 156.54 | 150.37 | 8.21 |

At the family RSS peak, the helper used **457.82 MiB** and the resident
**29.44 MiB**. The largest observed helper VmHWM was **465.55 MiB**; this is a
per-process high-water observation, not another simultaneous family peak.
Settled RSS after the first small upload and three repeats was 28.215, 29.164,
29.836 and 30.883 MiB: **2.668 MiB** final-minus-first growth, below the frozen
4 MiB repeated-workload diagnostic. This finite run does not prove every
long-duration leak absent. Source inspection found request-scoped SDK state,
completed-operation observations/copy owners removed, and expired weak object
locks pruned; no resident retention defect was demonstrated.

## Optimization decision

The upload helper accounts for most active RSS. The inspected pinned SDK buffers
4 MiB encrypted blocks internally (15 buffered, five uploading); its public
configuration exposes no upload-queue memory control. A separate pinned-Bun
stream probe found only **128 KiB** read-ahead both with the current adapter and
an explicit byte strategy, so that hypothesis produced no change.

The tested alternative embedded Bun's supported
[`--smol` mode](https://bun.sh/docs/runtime#bun-run-smol) through
[`--compile-exec-argv`](https://bun.sh/docs/bundler/executables#embedding-runtime-arguments).
A compiled control verified the embedded flag. The full replay measured
**539.96 MiB** family RSS, **10.82% higher** than baseline. The helper itself
peaked at 499.62 MiB sampled RSS (506.26 MiB observed VmHWM), so the result was not
solely Activity overlap. The large-stage sampled CPU rose from **20.69 to
26.47 seconds**. This is one sequential instrumented comparison, not a general
throughput claim. It demonstrated no safe improvement, so commit `a063f36`
reverted the experiment. Offline provisioning restored the exact baseline helper
hash above. No speculative SDK patch, new dependency or forced-GC call was kept.

## Post-use idle and validation

Each row is a separate 300-second window of the same resident for that variant.
Every window passed: one process, no UI/SDK/escaped children, no exited retained
descendants, no swap, RSS below 40 MiB and CPU below 0.5% of one core.

| Variant/window | RSS peak MiB | CPU, one core | Samples |
| --- | ---: | ---: | ---: |
| Baseline, window 1 | 31.305 | 0.01000% | 61 |
| Baseline, window 2 | 31.305 | 0.00333% | 61 |
| Rejected `--smol`, window 1 | 31.270 | 0.01333% | 61 |
| Rejected `--smol`, window 2 | 31.277 | 0.00667% | 61 |

Candidate helper verification passed 41 tests / 253 assertions, 11 compiled
no-account stdio cases and the complete 1,048,337-byte stdout drain. Candidate and
restored dev builds each passed the two scoped Proton CTests. Full S3 suites were
excluded. Focused review of the build-flag change found no defects; its independent in-process read
covered the adversarial lens after the Anthropic route failed before egress with
npm `ETARGET`. The four known upstream TypeScript diagnostics remain unclaimed.

## Measurement limits and preserved attempts

The supplemental collector targeted 20 ms discovery / 200 ms memory sampling and
used process-only strace (no exec argv, environment, request or payload tracing).
For baseline/candidate respectively, maximum discovery gaps were 0.380/0.281
seconds and maximum memory gaps were 0.481/0.468 seconds. The collector itself
used 22.32%/25.40% of one core, excluded from product CPU. Passive strace overhead was not
separately quantified. CPU after each process's last sample is omitted. RSS can
double-count shared pages; the separate PSS/private maxima need not coincide.

The first collector failed on a legitimate fork-to-exec transition. Its correction
kept PID/start-time/UID ownership and expected executable hashes, and passed a
60 ms exec plus reparented-helper regression before replay. The collector's
`startedUtc` field is completion-time-labelled; monotonic time and trace epochs
provide chronology. Baseline host control later exited 143 during the final
repeat. A continuation verified and pasted that same completed operation without
another upload; that stage's wall/mean-CPU timing includes the interruption.
Candidate console focus required a visible correction after unlocking. Original
failures and corrected commands remain private; none was counted as an app pass.

Socket preflight covered all resolved paths (longest 53 UTF-8 bytes), and capacity
checks passed. The guest Secret Service was memory-only; persistent keyring boot
acceptance is unverified. Browser/SDK session establishment was outside the active
app observer: “first account use” here means `setup.inspectAccount` in the new
resident. Larger files, multiple accounts and other compositors/packages remain
outside this resource record. No physical cold-memory result is claimed.

## Cleanup and retention

Each scoped inventory found 12 confirmed nodes for 12 unique receipts, no duplicate
confirmed upload and no visible unfinished draft. Three earlier baseline-attempt
nodes were explicitly excluded and retained. All 24 current-workload test links
and two earlier-attempt links were revoked. Both independent SDK sessions were
revoked (HTTP 200), and their host/guest keychain entries were removed. Remote
objects and unknown private attempts were preserved; no remote object deletion ran.

Raw sources, measurements, native witnesses, original failures, exact identities
and cleanup receipts are retained privately at
`work/evidence/proton-p2-20261015` in the primary workspace, with a checksum index.
The public JSON contains no credentials, account identities, capability URLs,
passwords or payloads. Frozen idle limits are unchanged; notices, release assets,
compatibility controls and final packaged multi-desktop acceptance remain P3–P6.
