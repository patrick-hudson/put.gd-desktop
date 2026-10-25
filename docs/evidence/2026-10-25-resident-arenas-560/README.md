# Resident memory after a 50-upload burst: stranded malloc arenas (#560)

Code: [PR #576](https://github.com/patrick-hudson/put.gd-desktop/pull/576).
Candidate `98878cfe31c2506d25ea6cbec97af002210319d2` (commit count 1294).
Matched trunk `80ffa2e6c0d34223d2cc0f31254f2e60ac2bd247` (1293), its parent.

This record covers attribution, the change, headless controls and the physical
Mint Cinnamon run of the unchanged #555 `burst` scenario. It does not close #560:
the parallel Cinnamon/GNOME/KDE gates and published-asset gates are still pending
(see the end).

## Cause

The original failure was published `v26.1024.1185-nightly` (source `9dfac15c`) on
m74 under the frozen #555 driver `00ab777f`. That run settled at RSS 43,020,288 B
and PSS 31,269,888 B, over the 40 MiB / 29 MiB limits. Its separate post-score
diagnostic run `p560d2` kept `malloc_info` and `smaps` before the workload and
after the settle (archive `evidence-b1-d2.tar.gz`, SHA-256
`4665d84906e7057cde4dc132c793a03ea681e3317adf23fbc9bec571c0cf5715`).
[`lab/attribute_arenas.py`](lab/attribute_arenas.py) reproduces this arithmetic
from those files:

| | Before | After settle | After an extra manual `malloc_trim(0)` |
| --- | --- | --- | --- |
| Non-main arena heaps (64 MiB-aligned anonymous mappings) | 8 | 23 | 23 |
| Their RSS | 356,352 B | 12,783,616 B | 12,603,392 B |
| Largest four | — | 2.84 / 2.47 / 2.40 / 2.34 MB | 2.66 / 2.47 / 2.40 / 2.34 MB |

After the settle, arenas 6, 8, 9, 13, 14 and 16 each report `system` between
1.9 and 2.9 MB. Their `rest` is almost the same as `system`, and they have zero
or very few bin chunks. In glibc 2.39, `rest` includes the top chunk (malloc.c
5857–5860). So these arenas hold an almost entirely free **top chunk**.

`malloc_trim` never releases such a top:

- `mtrim()` (malloc.c 5149–5198) madvises only page-sized interiors of bin chunks.
- It calls `systrim()` only for `main_arena`.
- A non-main heap shrinks only through `heap_trim()`. That runs from `_int_free`
  when a consolidated free is at least `FASTBIN_CONSOLIDATION_THRESHOLD`, 64 KiB
  (malloc.c 4776–4795).
- When small frees merge into the top later, through `malloc_consolidate()`, no
  `heap_trim()` follows.

The resident's existing gated idle trim therefore ran (count 2), but the
post-score manual trim freed only 372,736 B.

Sources are the `glibc-2.39` tag, `malloc/malloc.c` (SHA-256
`5c2e41a83c1c4d1ddaaad761088b93469110a79067ba6485eb594a0423048663`) and
`malloc/arena.c` (`81b608d7eff41299a50f0b6e265d2bd6860f95f324fde89826ea2841062cf1bb`).
m74, d69, f44 and the workstation all run glibc 2.39 (`Ubuntu GLIBC 2.39-0ubuntu8.9`
on m74).

Where the arenas come from: `OperationService::background` starts a `QThread`
per job (journal saves, queries, upload runs), and each item transfer runs on its
own `std::thread`. A burst runs many of these at once. glibc gives contending
threads separate arenas, up to 8 × cores.

## Change

`applyResidentAllocatorPolicy()` now also calls `mallopt(M_ARENA_MAX, 1)`. Every
free top is then in the main arena, where the existing idle trim shrinks it.

- `M_ARENA_MAX` sets only `mp_.arena_max` (malloc.c 5614). The fixed 128 KiB mmap
  threshold and the trim threshold are unchanged, and no trim call is added.
- The limit is read lazily, when glibc creates its first extra arena (arena.c
  828–831). Arenas freed by exited threads are reused through `get_free_list`. So
  the policy must run before any worker thread. It does: the resident's `main()`
  applies it before application construction on every route, and
  `putgd-ui --edit-image` applies it at the same point.
- A standalone glibc 2.39 check, [`lab/arena-strand-microcheck.cpp`](lab/arena-strand-microcheck.cpp)
  (8 concurrent churning threads × 3 rounds, then trim), kept 1,316 KiB anonymous
  RSS with default arenas. It kept 260 KiB with a cap of 1, 396 KiB with 2 and
  660 KiB with 4. Only a cap of 1 removes the stranding; with 2 or more, a
  non-main arena can still strand its top.
- The header now includes `<cstdlib>` before it tests `__GLIBC__`. Before this, a
  translation unit that included it first compiled a silent no-op policy. Both
  production `main.cpp` files include Qt first, so they were not affected.

`allocator_policy_contract` reproduces the mechanism. It passes 3/3 on d69. With
only the cap line removed it fails 3/3, retaining 1,085,440 B over a 339,968 B
baseline ([candidate](d69/allocator-test-candidate.txt),
[negative control](d69/allocator-test-negative-nocap.txt)).

## Tests

- d69, Debian 13, Qt 6.8.2 Debug: 11 focused suites pass
  ([log](d69/ctest-focused-cand.txt)): allocator_policy, edit_operations,
  resident_retire, resident_display_loss, archive, upload_naming, history,
  operations, settings, diagnostics_upload and resident_session.
- f44 rootless container, Ubuntu 24.04, Qt 6.4.2, GCC 13.3, Release: the same 11
  suites pass.
  - The container image is `localhost/putgd-i560-u24-deps:v1`: image `d32d68be`
    plus `libxcb-xtest0-dev` 1.15-1ubuntu2, `xvfb` and `xauth`, which `BUILD_TESTING`
    needs.
  - As container root, `settings_contract` fails one case,
    `unwritableParentIsReportedWithoutChangingFile`, because root bypasses the
    unwritable directory. It passes 26/26 as an unprivileged user, which is how CI
    runs it.
- The full development suite and the CI sanitizer jobs are not claimed here.

## Physical Mint 22.3 Cinnamon/X11 (m74)

Packages were built in the same f44 container with BUILD_TESTING on, Proton not
bundled, for ubuntu24.04. Both are development versions with clean trees:

| Package | SHA-256 | `/usr/bin/putgd` |
| --- | --- | --- |
| candidate `putgd_26.1025.1294~dev+g98878cfe31c2_amd64.deb` | `bab4d09225f7ed620102b25ca0e7d812a28a53ff09e65f4a6b30b0fe498f103b` | `e6ea7836…` |
| trunk `putgd_26.1025.1293~dev+g80ffa2e6c0d3_amd64.deb` | `864062f35e57176688a80da3516dfdead493b1b8da73f2192fb98a87f09746a2` | `c8d59a0b…` |

Lock `t3-i560-opus-native1` was held from 19:01:50Z and released verified-clean at
19:24:06Z ([receipt](m74/release-receipt.json)).

Admission and setup:

- Load was 0.02 and CPU 97.6 % idle over a 2 s sample.
- The Windows QEMU (PID 308684) and the shared SSH processes 2067, 313646 and
  313651 were preserved.
- The apt simulation installed only `putgd` and `gir1.2-gtk-4.0`, with 0 upgrades.
- Host THP was `always [madvise] never`. There was no allocator environment and
  no prctl.
- The frozen driver, native driver and runner hashes are in
  [identities](m74/identities.sha256): `physical_campaign.py` `3cb511e0…`,
  `campaign_workloads.py` `179e837e…`, native driver `347934c2…`.
- The runner is [`lab/m74-run.sh`](lab/m74-run.sh). It is the earlier #560
  baseline runner with only paths, package and identity parameters changed, plus
  extra records outside scoring.

| Run | Status | Final family RSS | Final family PSS | Settle growth (driver: unassessed) | Gates |
| --- | --- | --- | --- | --- | --- |
| c1 candidate | complete | **36,057,088 B** | **24,264,704 B** | 5,327,872 B | settled RSS/PSS pass, 50/50 verified, visuals 4/4, cleanup pass |
| t1 trunk | failed at 72.3 s | — | — | — | harness `ProcessLookupError` (see below) |
| t2 trunk, identical retry | complete | 40,570,880 B | 28,607,488 B | 9,341,952 B | settled RSS/PSS pass (margins 1.37 / 1.80 MB), 50/50, 4/4, cleanup pass |

[Results](m74/frozen-burst-results.json) keep every gate, metric, limitation and
the full 61-sample settle series for c1 and t2. Both series stay at one process.
c1 runs from 36,155,392 B to 36,057,088 B, and t2 from 40,665,088 B to
40,570,880 B; neither rises during the settle.

- **t1 failure:** the frozen sampler hit a `/proc` race. A helper exited between
  discovery and its `stat` read, giving `ProcessLookupError` in
  `bench_ui_process.identity`. Resident cleanup still passed (exit 0, not forced,
  nothing remaining, socket absent). It is retained, and t2 reran the same command.
  t2 started at load 1.07, just after t1's teardown.
- **Trunk did not reproduce the failure.** In this single run, current trunk's
  development package passed, narrowly. The original failure was the published,
  Proton-bundled 1185 package from older source. Against matched trunk on the same
  host, the candidate settles 4,513,792 B lower in RSS and 4,342,784 B lower in
  PSS, and its settle growth is 5.33 MB instead of 9.34 MB. That is one run each,
  so no repeatability or variance is claimed.
- **Limits:** a finite settle does not prove the absence of all leaks. The
  driver's own limitations are kept in the results.

## Headless controls (d69, not desktop acceptance)

These use a private copy of the earlier #560 v7 harness, [`lab/resident_burst_o1.py`](lab/resident_burst_o1.py):

- It runs a private Xvfb and a private D-Bus with a simulated StatusNotifier peer,
  so the X11 Core resident route is selected.
- It reuses the frozen upload-source, preview and verification functions unchanged.
- It opens Activity before the burst, refreshes it at 5, 25 and 50 uploads and on
  completion, then closes it normally.
- It adds a per-call latency record and a post-score `smaps` capture.
- With `I560_THP_OFF`, it applies `PR_SET_THP_DISABLE` to the resident's process
  tree. This is a controlled isolation of THP effects. It is not the same as a
  host `madvise` policy.

The builds are Qt 6.8.2 Debug in one tree with the same flags
([base](d69/bin-base-SHA256SUMS), [candidate](d69/bin-cand-SHA256SUMS)). The S3
module is identical in both (`ae6e0172…`). Summaries:
[`d69/summary.jsonl`](d69/summary.jsonl), produced by
[`lab/summarize_o.py`](lab/summarize_o.py).

| 50 uploads, 10 s PUT, 300 s closed settle | THP | Final family RSS | PSS | Non-main arenas post-score (RSS) | AnonHugePages |
| --- | --- | --- | --- | --- | --- |
| o0 base | prctl off | 43,520,000 B (fail) | 19,456,000 B | 23 (7,331,840 B) | 0 |
| o1 candidate | prctl off | 37,736,448 B | 13,740,032 B | 0 | 0 |
| o2 base + env `MALLOC_ARENA_MAX=1` (diagnostic control only) | prctl off | 37,847,040 B | 13,812,736 B | 0 | 0 |
| o3 base | host `[always]` | 59,502,592 B (fail) | 35,449,856 B (fail) | 22 (23,248,896 B) | 20 MiB |
| o4 candidate | host `[always]` | 40,439,808 B | 16,409,600 B | 0 | 4 MiB |

- Every run had 50/50 independent verifications, a single process in all 60 settle
  samples, resident exit 0, and no tracked process or socket left.
- o2 matches o1 on the baseline binary, so the reduction comes from the arena cap
  alone. The environment variable is never an acceptance configuration.
- With host THP `always`, khugepaged collapses the stranded arena heaps into huge
  pages. That explains the late jump to 58.9 MB in the earlier n2 diagnostic.
- PSS here is lower than on m74 because more libraries are shared on d69.
- Session call latency was the same in all five runs: p50 about 1.5 ms, p95
  49.3 ms, max about 0.57 s.

**Cancellation** (oa base, ob candidate): eight uploads at concurrency 4. Two
transferring and two queued operations were cancelled, and the other four
completed and verified.

- Cancel-to-settled times were 0.52 / 1.35 / 8.87 / 1.30 s for base and
  0.57 / 1.30 / 8.88 / 1.30 s for the candidate.
- Both drained to a single process and exited normally.

**Multi-item throughput and main-loop responsiveness:** the production
`OperationService`, `S3Provider` and `putgd-s3-worker` ran against the loopback
fixture at concurrency 4. The workload was six 4 MiB files, two 96 MiB files
(multipart) and eight 1 MiB files, three rounds alternating base and candidate.
The probe had a private patch, [`lab/probe-control.patch`](lab/probe-control.patch),
applied identically at both commits. It applies the resident policy as `main()`
does, measures 10 ms timer lateness on the main thread, and reports heap after a
final trim.

| | Elapsed (ms) | Main-loop lag max (µs) | Heap arena bytes after trim |
| --- | --- | --- | --- |
| base | 8021 / 7956 / 7990 | 38,923 / 23,245 / 52,578 | 6.31 / 5.87 / 6.71 MB |
| candidate | 8039 / 7980 / 7956 | 35,960 / 17,727 / 28,628 | 2.27 / 2.29 / 2.03 MB |

All 96 files uploaded. Mean lag was 182–249 µs for both builds. No throughput or
responsiveness cost was observed at this scale; this is not a contention proof
for other workloads.

## Retained evidence

| Archive | SHA-256 | Where |
| --- | --- | --- |
| `evidence-m74-c1-t1-t2.tar` (1003 members, runtime sockets excluded) | `fd7cda2f062fa5eabb438eaa2304e82ab40c90af531604ce76d9b82f9b13b752` | m74 `~/work/i560o/`, workstation task `work/i560o/m74/` |
| `i560o-d69-evidence.tar` (1909 members) | `d3c77779d5bad10ecf0135b248716df0e3bdf2ca1219008dbb04a4b2f04f88e1` | d69 `~/work/`, workstation task `work/i560o/` |
| Earlier baseline/diagnostic `evidence-b1-d2.tar.gz` | `4665d849…` | m74 `~/work/i560/`, earlier task `work/i560/` |
| Earlier d69 handoff `handoff-d69-evidence.tar` | `b458c085…` | d69 `~/work/i560/`, earlier task `work/i560/` |

The m74 archive holds full results, events, the four burst visuals, runner logs
and the failed t1. The d69 archive holds every run's results, metadata and
smaps, plus the scripts. The random probe media were deleted after hashing; the
index is in the archive. Earlier rejected candidates (SQL projection, bounded
config read, query pool, archive buffer) and their evidence stay with the
earlier task.

## Still pending

- Parallel Cinnamon/GNOME/KDE functional + memory gates on the exact candidate.
  GNOME/KDE use the approved native matrix and scoped functional checks, plus a
  separately identified native-Wayland burst diagnostic. The #555 burst driver
  requires Cinnamon/X11.
- Published-asset gates, after a nightly containing #576 publishes.
- `captureBurst` and `historyOpenUpperBoundMs` remain unassessed by the driver.
- The candidate lacks the separate #558 and #254 fixes. This is not an
  integrated stable acceptance.
