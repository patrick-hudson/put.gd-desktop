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

Line numbers above are from the `glibc-2.39` tag: `malloc/malloc.c` (SHA-256
`5c2e41a83c1c4d1ddaaad761088b93469110a79067ba6485eb594a0423048663`) and
`malloc/arena.c` (`81b608d7eff41299a50f0b6e265d2bd6860f95f324fde89826ea2841062cf1bb`).
That is the version of every physical m74 run and of the Ubuntu 24.04 package
container. Versions differ by host (`getconf GNU_LIBC_VERSION`):

| Where | glibc | Used for |
| --- | --- | --- |
| m74 (Mint 22.3, `Ubuntu GLIBC 2.39-0ubuntu8.9`) | 2.39 | original failure, all physical runs |
| f44 rootless Ubuntu 24.04 container | 2.39 | package builds, Qt 6.4.2 tests |
| workstation | 2.39 | standalone microcheck |
| d69 (Debian 13) | 2.41 | headless controls, Qt 6.8 tests |
| f44 host (Fedora 44) | 2.43 | not used for this evidence |

The 2.39 source proof applies to the 2.39 rows. For the other versions the same
behaviour was checked in their own sources, without extending the 2.39 proof:

- **2.41** (`malloc.c` SHA-256 `7d931418…`): `mtrim()` is byte-identical to 2.39
  (lines 5206–5255), and `heap_trim()` is reached at line 4852.
- **2.43** (`c0cb1540…`): `mtrim()` no longer calls `malloc_consolidate()`, but it
  still calls `systrim()` only for `main_arena` (line 4818), and `heap_trim()` is
  reached at line 4565.

The d69 controls exercised 2.41.

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

- d69, Debian 13, Qt 6.8.2 Debug: 11 focused suites pass:
  allocator_policy, edit_operations,
  resident_retire, resident_display_loss, archive, upload_naming, history,
  operations, settings, diagnostics_upload and resident_session.
  - [`d69/ctest-focused-cand.txt`](d69/ctest-focused-cand.txt) is an **excerpt**:
    only the last 18 lines of the original run were kept, so it starts at 6/11.
  - The same command was rerun at the same clean `98878cfe` tree, with its full
    output: [`d69/ctest-focused-cand-full.txt`](d69/ctest-focused-cand-full.txt),
    100 % of 11 passed in 74.72 s.
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
- **Runner exit status:** [`lab/m74-run.sh`](lab/m74-run.sh) wrote the driver
  status to `driver-exit.txt` but always exited 0 itself (#579 review). Every pass
  and fail above comes from the raw driver `results.json`, gates and cleanup
  receipts, never from the runner's exit status.
  [`lab/m74-run-v2.sh`](lab/m74-run-v2.sh) returns the driver status; the exact-CI
  runs below used scripts that record it the same way.

## Physical m74, exact CI package

The CI package is the ubuntu24.04 DEB from workflow_dispatch run 40225160437 at
`98878cfe`. It is not the pull request's merge-ref run 40223937660, whose binaries
embed `4e686c59`.

- Package SHA-256 `5c77da8d6e25228061024afd9adcf55c61c25e87283920e39152eee4412a6300`;
  `/usr/bin/putgd` is `eccfe8c7…`.
- Installed identity: `26.1025.1294-dev+g98878cfe31c2`, not dirty
  ([identity](m74-ci/installed-version.json), [hashes](m74-ci/identities.sha256)).
- Lock `t3-i560-opus-ci1` was held from 19:31:03Z and released verified-clean at
  19:52:03Z ([receipt](m74-ci/release-receipt.json)).
- The same admission and cleanup rules applied: load 0.06 at start, apt simulation
  of 2 new packages with 0 upgrades, only `putgd` and `gir1.2-gtk-4.0` purged.
- Host THP was `[madvise]`, with no allocator environment and no prctl.

Runner scripts: [`lab/m74-run-ci.sh`](lab/m74-run-ci.sh) differs from `m74-run.sh`
only in its lock session. Also used were
[`lab/m74-memory-ci.sh`](lab/m74-memory-ci.sh),
[`lab/functional_ci.py`](lab/functional_ci.py) and [`lab/f-run.sh`](lab/f-run.sh).
Results are in [`m74-ci/frozen-results.json`](m74-ci/frozen-results.json) and
[`m74-ci/functional-results.json`](m74-ci/functional-results.json).

| Run | Result |
| --- | --- |
| e1 frozen `burst`, settle 300 | complete. Final family RSS **35,995,648 B**, PSS **24,103,936 B**: settled RSS/PSS pass. 50/50 verified, visuals 4/4, cleanup pass. Settle growth 5,271,552 B (unassessed). The 61-sample settle series stays at one process, from 36,089,856 B down to 35,995,648 B. |
| m1 frozen `memory`, cold 300 / History 30 / closed 300 | complete, driver exit 1 because one gate fails. Cold and closed CPU/RSS/PSS pass (peak family 29,978,624 / 18,788,352 B and 30,179,328 / 18,966,528 B). **`historyOpenPss` fails**: 64,642,048 B over 30.044 s against 62,914,560 B. This is the known #558 failure; that fix is not in `98878cfe`. No waiver. `historyPaintResponse` unassessed; cleanup pass. |
| f1 scoped, unscored functional pass | pass (details below). |

f1 ran against a private profile on the physical `:0` desktop, at concurrency 1
with a 6 s PUT delay:

- Two editor uploads completed and were verified with the frozen `verify_uploaded`.
- The third operation's fresh witness said `queued` (state `acquiring`, no items),
  and the operation was then cancelled and settled as cancelled. f1 did not
  inventory the fixture, so it does not prove that nothing was published.
- History showed 2 captures and "1 cancelled capture hidden"
  ([frame](m74-ci/f1-history.png)). Settings showed the private archive path
  ([frame](m74-ci/f1-settings.png)).
- The window closed through the window manager and the UI reached `stopped`.
  `ui.quit` gave exit 0, the socket was gone, and no owned process remained.
- **Deviation:** the default "Copy each link as it finishes" put two synthetic
  loopback links on the desktop clipboard while the resident ran. Whether the
  selection persisted, for example in a clipboard manager, was not checked, and the
  clipboard was not inspected or cleared afterwards. No profile setting disables
  automatic copying, only its format, so a future unscored run on a shared desktop
  needs a nested display or an explicit clipboard scope.

## Physical GNOME and Plasma, exact CI packages

Recorded by the separate acceptance thread. The packages come from the same exact
dispatch run 40225160437 at `98878cfe`: the f44 Fedora 44 RPM `ad172a3e…` and the
k72 Kubuntu 26.04 DEB `50564f8f…`. The f44 and k72 locks were held from 19:26:54Z
and 19:26:55Z and released verified-clean at 19:49:09Z and 19:49:10Z.

The frozen #555 driver requires Cinnamon/X11, so it cannot run on these desktops.
The coordinator approved two measurements in its place:

- the matrix item C idle check plus scoped functional checks;
- a separately identified native-Wayland burst **diagnostic**. Its driver,
  `wayland_burst_diag.py` (r1 `d40c77e4`, r2 `3bf3ba76`), imports the frozen
  `00ab777f` workload unchanged. It is not the frozen gate.

| Desktop | Wayland burst diagnostic (50 uploads, settled) | Matrix C, 180 s idle (`native_c_functional` r2 `109d29ff`) |
| --- | --- | --- |
| f44 GNOME 50.6 | gb1: RSS 41,304,064 B / PSS 26,366,976 B, both under the 40 / 29 MiB limits | gc3: peak RSS 37,154,816 B / PSS 22,383,616 B, pass |
| k72 Plasma 6.6.6 | kb2: RSS 38,014,976 B / PSS 13,446,144 B, both under the limits | kc1: peak RSS 33,239,040 B / PSS 8,889,344 B, pass |

- **Burst runs:** both had 50/50 verified uploads, a normal helper exit and normal
  cleanup. Both exited 1 because of the planned `burstVisualAttribution` fail:
  automated visual attribution is unassessed on Wayland.
- **k72 kb1:** it hit the frozen sampler's `/proc` race before scoring and is
  retained.
- **f44 gc1 and gc2:** a stale put.gd banner blocked them before measurement. One
  authorised dismiss click was made, then gc3 ran.
- **Matrix C mappings:** neither desktop mapped GTK, the native theme, libssl or
  QtNetwork.
- **Functional checks:** editor upload, the views, helper close and normal quit pass
  on both desktops.
- **Functional cancellation:** recorded as a **fail** on both. The cancel came after
  the whole 716-byte body had been sent, so the outcome matches the in-flight
  contract in `docs/s3.md`. The coordinator classified it as an assertion mismatch,
  neither a regression nor a pass.

Reviewed safe frames show synthetic content and the put.gd window only:

- [f44 History](gnome-kde/f44-gnome-history-98878cfe.png)
- [f44 Settings](gnome-kde/f44-gnome-settings-98878cfe.png)
- [k72 History](gnome-kde/k72-plasma-history-98878cfe.png)
- [k72 Settings](gnome-kde/k72-plasma-settings-98878cfe.png)

Raw evidence stays private in the acceptance task: `work/i560a/evidence/`, with
`SHA256SUMS` `73726b516606ce875864d206436b37ffd5aa9e3dcc03c1b35c32ad62d624b201` and
`RESTORE-INDEX.md`. Admission thumbnails and banner crops show the hosts' own
desktops, so they stay private.

## Headless controls (d69, not desktop acceptance)

These use a private copy of the earlier #560 v7 harness, [`lab/resident_burst_o1.py`](lab/resident_burst_o1.py).
`resident_burst_o1.py`, [`run_o1.sh`](lab/run_o1.sh), [`campaign_o.sh`](lab/campaign_o.sh),
[`phase2_o.sh`](lab/phase2_o.sh), [`summarize_o.py`](lab/summarize_o.py) and
[`m74-run.sh`](lab/m74-run.sh) are kept byte-for-byte as the historical scripts that
produced these results. They hard-code the d69/m74 working directories. To replay,
use the revised scripts listed under [Revised scripts](#revised-scripts) instead.

The o1 harness:

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
module is identical in both (`ae6e0172…`).

The summaries are [`d69/summary-v2.jsonl`](d69/summary-v2.jsonl), from
[`lab/summarize_o2.py`](lab/summarize_o2.py). The original
[`d69/summary.jsonl`](d69/summary.jsonl) is kept, but its `last270MaxFamily` and
`AllLast270` fields are **wrong for oa and ob**: those runs settled for only 31 s, so
the "last 270 s" was a single sample. v2 takes the window from sample timestamps
and reports it only after at least 300 s of observed settle. v2 reproduces the
values for o0–o4, which settled for about 312 s.

The `exit=` values in [`d69/campaign.log`](d69/campaign.log) are also wrong: they
record `date`'s status, so a failed run would still log 0. Every run's actual
status is in its own `wrapper-cleanup.json`: runExit 0 for o0–o4, oa and ob.

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

**Cancellation, original control** (oa base, ob candidate): eight uploads at
concurrency 4. Four targets were cancelled and the other four completed and
verified. Cancel-to-settled times were 0.52 / 1.35 / 8.87 / 1.30 s for base and
0.57 / 1.30 / 8.88 / 1.30 s for the candidate, and both drained and exited
normally.

**Correction (#579 review):** this does not show cancellation of *queued*
operations. The four targets were chosen from one earlier snapshot. The two
"transferring" targets were cancelled, and waited on, first. By the time the two
"queued" targets were cancelled, `pump()` may already have started them; the
8.87 s settle suggests one had. The raw `queuedCancelled` labels are kept; read them
as "targets selected as queued in an earlier snapshot".

**Cancellation, corrected control** (qa base, qb candidate):
[`lab/resident_burst_o2.py`](lab/resident_burst_o2.py) `I560_MODE=cancel2`, run by
[`lab/campaign_o2.sh`](lab/campaign_o2.sh), with controlled THP-disabled as above.
Results: [`d69/cancel2-results.json`](d69/cancel2-results.json),
[log](d69/campaign2.log).

- Setup: six uploads at concurrency 2, with a 30 s PUT delay holding the two
  transfers.
- Queued targets go first. Each gets a fresh `operation.inspect` immediately before
  its cancel; the cancel follows 0.4 ms later. Every witness reported `queued`
  (state `acquiring`, no items). Then the two transfers were cancelled, each
  witnessed as `uploading`.
- Settle times:

  | Target | Base | Candidate |
  | --- | --- | --- |
  | queued | 0.053 / 0.053 s | 0.005 / 0.005 s |
  | transferring | 22.8 / 1.25 s | 22.9 / 1.25 s |

- The fixture inventory afterwards shows **0 PUT requests and 0 objects** for each
  cancelled queued target. For the transfer targets it reports 0 objects, recorded
  but not asserted, since an in-flight PUT may take effect.
- The two remaining uploads completed and verified. Both runs drained to one
  process, exited 0, and left nothing.
- The witness and the cancel are separate requests. The held transfers make a
  promotion in between implausible, not impossible; atomicity is not claimed.

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

All 96 files uploaded, and mean lag was 182–249 µs for both builds. No material
throughput or responsiveness change was observed in this one workload over three
rounds. This is not a contention proof for other workloads.

**Probe identity audit (#579 review):** [`lab/phase2_o.sh`](lab/phase2_o.sh) would
have copied stale binaries after a failed checkout, patch or build. So the
historical probe directories were audited before relying on them:

- The build log ([`d69/phase2.log`](d69/phase2.log)) shows `operations_upload_probe`
  and `putgd-s3-worker` linking in both builds.
- Each worker embeds its own revision string (`g80ffa2e6c0d3` and `g98878cfe31c2`).
- The probe binaries differ (`e73eda01…` and `0ab83a3a…`).
- Disassembly shows one `mallopt(-8 = M_ARENA_MAX, …)` call in the candidate probe
  and none in the base probe.

The historical comparison therefore stands, and it was not rerun.
[`lab/phase2_o2.sh`](lab/phase2_o2.sh) replaces the build stage:

- Every stage fails fast, and binaries are copied only into new directories.
- Copying happens only after the checkout matches, the patch is present, the
  outputs are newer than the build start, and the worker embeds the intended
  revision.
- A trap restores the source tree.

[`lab/fault-checks/`](lab/fault-checks/results.txt) drives it with fake `git` and
`cmake`:

| Injected fault | Exit | Directories copied |
| --- | --- | --- |
| failing checkout | 1 | none |
| failing apply | 1 | none |
| no-op apply | 5 | none |
| failing build | 1 | none |
| stale outputs | 6 | none |
| wrong worker revision | 7 | none |
| existing output root | 2 | none |
| clean run | 0 | both |

The source patch was restored in every case. The same results file shows
`campaign_o2.sh` logging `exit=37` for a runner that exits 37, and
[`lab/test_summarize_o2.py`](lab/test_summarize_o2.py) passing 6 cases: 30 s,
300 s, a peak before and a peak inside the window, partial data and missing data.

## Revised scripts

| Historical | Revision | Change |
| --- | --- | --- |
| `resident_burst_o1.py`, `run_o1.sh` | [`resident_burst_o2.py`](lab/resident_burst_o2.py), [`run_o2.sh`](lab/run_o2.sh) | Explicit, validated `I560_SRC`/`I560_HARNESS` roots; `run_o2.sh SRC HARNESS OUT APP` rejects a bad root or an existing output and returns the harness status; corrected `cancel2` mode |
| `campaign_o.sh` | [`campaign_o2.sh`](lab/campaign_o2.sh) | Captures the run status before logging; returns failure |
| `phase2_o.sh` (probe stage) | [`phase2_o2.sh`](lab/phase2_o2.sh) | Fail-fast, verified build identity, new output directories, source restore |
| `summarize_o.py` | [`summarize_o2.py`](lab/summarize_o2.py) | Timestamp-based final-270 s window, only after a full settle |
| `m74-run.sh` | [`m74-run-v2.sh`](lab/m74-run-v2.sh) | Returns the driver status; lock session is a parameter |

## Retained evidence

| Archive | SHA-256 | Where |
| --- | --- | --- |
| `evidence-m74-c1-t1-t2.tar` (1003 members, runtime sockets excluded) | `fd7cda2f062fa5eabb438eaa2304e82ab40c90af531604ce76d9b82f9b13b752` | m74 `~/work/i560o/`, workstation task `work/i560o/m74/` |
| `i560o-d69-evidence.tar` (1909 members) | `d3c77779d5bad10ecf0135b248716df0e3bdf2ca1219008dbb04a4b2f04f88e1` | d69 `~/work/`, workstation task `work/i560o/` |
| `evidence-m74-ci-e1-m1-f1.tar` (431 members) | `2ff4726efd80a682f03efc6272faa71ffe99ce72156a7c9ea9e376a2ff8bbd71` | m74 `~/work/i560o/`, workstation task `work/i560o/m74ci/` |
| `i560o-h2-evidence.tar` (136 members, corrected cancel control, summary v2, full ctest log) | `b42ec747efdacc8c7e03ea53d634a91dff5f05431708c7d569b6cc9b4d31ceb5` | d69 `~/work/`, workstation task `work/i560o/` |
| Earlier baseline/diagnostic `evidence-b1-d2.tar.gz` | `4665d849…` | m74 `~/work/i560/`, earlier task `work/i560/` |
| Earlier d69 handoff `handoff-d69-evidence.tar` | `b458c085…` | d69 `~/work/i560/`, earlier task `work/i560/` |

The m74 archive holds full results, events, the four burst visuals, runner logs
and the failed t1. The d69 archive holds every run's results, metadata and
smaps, plus the scripts. The random probe media were deleted after hashing; the
index is in the archive. Earlier rejected candidates (SQL projection, bounded
config read, query pool, archive buffer) and their evidence stay with the
earlier task.

## Still pending

- The parallel Cinnamon/GNOME/KDE functional + memory set on the exact candidate.
  The m74 exact-CI runs above are its Cinnamon part. GNOME and KDE use the
  approved native matrix and scoped functional checks, plus a separately
  identified native-Wayland burst diagnostic, because the #555 burst driver
  requires Cinnamon/X11. The candidate GNOME and Plasma rows are recorded above.
  Still pending: matched-trunk `80ffa2e6` rows for GNOME and Plasma, d69, and
  resolving the functional-cancellation assertion.
- `historyOpenPss` fails on the exact candidate (m1) until #558's fix is included.
- Published-asset gates, after a nightly containing #576 publishes.
- `captureBurst` and `historyOpenUpperBoundMs` remain unassessed by the driver.
- The candidate lacks the separate #558 and #254 fixes. This is not an
  integrated stable acceptance.
