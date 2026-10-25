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
19:24:06Z ([receipt, sanitized projection](m74/release-receipt.sanitized.json)).

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
  19:52:03Z ([receipt, sanitized projection](m74-ci/release-receipt.sanitized.json)).
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

**f44 host deviation:** "verified-clean" does not mean the host was left
unmodified. The Fedora cleanup removed three pre-existing dependency packages,
although the simulation had not shown that. They were restored straight away with
the same NEVRAs and the same dependency install reasons, and the coordinator
verified this. Their install timestamps changed.

The frozen #555 driver requires Cinnamon/X11, so it cannot run on these desktops.
The coordinator approved two measurements in its place:

- the matrix item C idle check plus scoped functional checks;
- a separately identified native-Wayland burst **diagnostic**, with driver
  `wayland_burst_diag.py` (r1 `d40c77e4`, r2 `3bf3ba76`). It reimplements the burst
  orchestration with native Wayland UI and lifecycle adaptations, and borrows the
  pinned frozen `00ab777f` source, upload and verification functions. It is
  neither the unmodified frozen `burst()` nor the frozen acceptance gate.

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

The coordinator reviewed these safe frames. They show synthetic content and the put.gd
window, plus the Plasma panel on k72:

- [f44 History](gnome-kde/f44-gnome-history-98878cfe.png)
- [f44 Settings](gnome-kde/f44-gnome-settings-98878cfe.png)
- [k72 History](gnome-kde/k72-plasma-history-98878cfe.png)
- [k72 Settings](gnome-kde/k72-plasma-settings-98878cfe.png)

**d69 Debian 13, Plasma/KWin 6.3.6 Wayland.** Same protocols, using the candidate
debian13 DEB `83e4d2cc…` from the same dispatch run 40225160437.

| Check | Result |
| --- | --- |
| Wayland burst diagnostic db1 (r2 `3bf3ba76`) | settled RSS 39,211,008 B / PSS 14,910,464 B, both under the limits. 50/50 verified. Exit 1 only from the planned visual-attribution fail. |
| Matrix C dc1, 180 s idle | pass: peak RSS 32,456,704 B / PSS 9,501,696 B. No GTK, native theme, libssl or QtNetwork mapped. |
| Functional | passes, except the same in-flight cancellation mismatch as f44 and k72 |
| Queued-cancel control dq1 (r4) | pass within the scope below: PUT attempts [1,1,1,2], no target pixels published |

- **Clipboard:** "Link copied" means the d69 session clipboard was changed with
  loopback links. It was not inspected.
- **Pre-existing popups:** two stale put.gd popups were already on the desktop and
  were left untouched.
- **Matched trunk on d69** (`80ffa2e6`, debian13 DEB `c9ac8836…`):
  - dt1 failed after 30 s on the frozen sampler race
    (`ProcessFamily.walk` `descendant_executable_changed`, followed by
    `helper_gone`) and produced no score. It is retained.
  - dt2, the one identical retry, settled at RSS 46,112,768 B, **over 40 MiB**, and
    PSS 21,779,456 B, which passes. Its admission load was 0.70, the tail of dt1,
    so quiet admission was not independently proven.
  - Against candidate db1 (39,211,008 B / 14,910,464 B), the candidate is 6.58 MiB
    lower in RSS and 6.55 MiB lower in PSS: trunk fails and the candidate passes.
    This is a single pair, and quiet admission is not independently proven: the
    load was 0.70 at the tail of dt1, with no CPU sample. So it supports no strong
    causal or repeatability claim.
  - Visual attribution still fails (4) in both runs. This is an unscored Wayland
    diagnostic, not frozen acceptance.
  - Matrix C du1 passes on trunk too (32,694,272 B / 9,787,392 B). The in-flight
    cancellation outcome is the same as the candidate's, so it predates #576 here
    as well; the failed original expectation stays recorded.
  - The queued-cancel control dr1 passes within its scope: 4 objects, 5 PUTs
    (attempts [1,2,1,1]), no target pixels. The witness was a separate call from
    the cancel. The clipboard was changed, and persistence is unverified.
  - Frame: [trunk History](gnome-kde/d69-plasma-queued-cancel-history-80ffa2e6.png).
    "Link copied" means this trunk run also changed the d69 clipboard.
  - The d69 slot was released at 21:03:21Z, and the coordinator independently
    confirmed it free at 21:03:55Z. The pre-existing signer agent (PID 2008860)
    and collector (978298) were preserved, and all 9 cleanup receipts were normal.
  - Raw evidence: `i560a-d69-trunk1-raw.tar.gz`, SHA-256
    `119e925cf7284f253b10371a8b0f523810e21127b1c59042a7a5aced33dcd617`.

**Native candidate vs matched trunk, single pairs** (Wayland burst diagnostic,
settled family RSS against the 40 MiB limit):

| Desktop | Trunk `80ffa2e6` | Candidate `98878cfe` | RSS change |
| --- | --- | --- | --- |
| f44 GNOME 50.6 | 44,077,056 B, fail | 41,304,064 B, pass | −2.64 MiB |
| d69 Plasma 6.3.6 | 46,112,768 B, fail | 39,211,008 B, pass | −6.58 MiB |
| k72 Plasma 6.6.6 | 38,260,736 B, pass | 38,014,976 B, pass | −0.23 MiB |

One run each. The k72 and d69 trunk retries started at an elevated load after a
failed first attempt. These are diagnostics, not the frozen gate.
- **Frames:** [History](gnome-kde/d69-plasma-queued-cancel-history-98878cfe.png)
  and [Settings](gnome-kde/d69-plasma-settings-98878cfe.png). They show synthetic
  content, the put.gd window and the Plasma panel.
- **Raw evidence:** `i560a-d69-cand1-raw.tar.gz`, SHA-256
  `4780e44bfdf9a47aff3a0f1c7ffed2594ea26f5dd2dbe7f6ad54db8eba2c7ebf`, in the
  acceptance task.

**Native queued-cancel control, r4** (`queued_cancel.py` `b5447229…`, unscored): runs
gq2 on f44 and kq2 on k72, with the candidate CI packages. Both **pass**. The scope
is limited to queued-publication absence plus survivor attribution.

- **Witness and cancel:** both targets were witnessed `queued` with 0 items, then
  cancelled 0.020–0.031 s later. Both ended `cancelled` with no items.
- **Publication:** decoded pixels put no target in any stored object or PUT body.
- **Survivors:** all 4 were stored once each. PUT attempts were [1,1,2,2] on f44
  and [1,1,2,1] on k72, within the up-to-3 attempts that `docs/s3.md` allows.
- **Cleanup:** helper close and normal quit were normal, with cleanup receipts.
  The slots were released verified-clean at 20:30:14Z and 20:30:15Z.
- **Lanes:** at the recorded cancellation witnesses, two blockers were uploading
  on f44, and the other blockers were queued with stored items. k72 also showed a
  third blocker uploading. These snapshots do not establish an effective
  concurrency limit or the cause of the r2 precondition failures.
- **Not proven:** atomicity (inspect and cancel are separate calls), and byte-level
  retry, key or condition correctness. The earlier native in-flight cancellation
  failures remain failures.
- **Earlier r2 attempts** gq1/kq1 failed the harness precondition before any target
  was cancelled, so they give no verdict. Their raw status `running` is stale.
- **Headless smoke** qs1/qs2 is not native evidence.
- **Not a functional or clipboard pass:**
  - The f44 History rows say "Could not copy link", which is a separate functional
    limitation this pass does not waive.
  - The k72 rows say "Link copied", so that run changed the k72 session clipboard
    with a loopback fixture link. Persistence was not checked, and the clipboard
    was not inspected or restored.

Both frames show 4 synthetic captures, 4 stored files and 2 cancelled captures
hidden. The k72 frame also shows the Plasma panel:

- [f44](gnome-kde/f44-gnome-queued-cancel-history-98878cfe.png)
- [k72](gnome-kde/k72-plasma-queued-cancel-history-98878cfe.png)

Raw evidence is private in the acceptance task (`i560a-f44-qc2-raw.tar.gz` and
`i560a-k72-qc2-raw.tar.gz`).

**Matched trunk (`80ffa2e6`), same protocols.** The trunk packages come from nightly CI run 40212427980.
One diagnostic pair per desktop, using the same driver on each:

| Desktop | Trunk burst diagnostic | Candidate burst diagnostic |
| --- | --- | --- |
| f44 GNOME | RSS 44,077,056 B (**over 40 MiB**) / PSS 29,126,656 B | RSS 41,304,064 B / PSS 26,366,976 B |
| k72 Plasma | RSS 38,260,736 B / PSS 13,630,464 B | RSS 38,014,976 B / PSS 13,446,144 B |

- **f44:** RSS goes from fail to pass. The candidate is 2.64 MiB lower in RSS and
  2.63 MiB lower in PSS. This is a single pair, with no repeatability claim.
- **k72:** both builds pass. The candidate is 0.23 MiB lower in RSS and 0.18 MiB
  lower in PSS. Quiet admission was **not proven** for the trunk run kt2: load was
  1.38, the tail of the failed kt1 (frozen-sampler `/proc` race, retained), and no
  CPU sample was taken. No efficacy claim
  rests on the small k72 delta.
- **Matrix C:** passes on both desktops for trunk as well: f44 peak RSS 37,249,024 B /
  PSS 22,436,864 B, k72 33,665,024 B / 9,133,056 B.
- **Functional cancellation:** the trunk runs reproduce the same finalizing-body
  cancellation, with a stored object and receipt, on both desktops. So that
  behaviour predates the candidate. The assertion mismatch is still recorded as a
  mismatch, not as a pass.

Raw evidence stays private in the acceptance task: `work/i560a/evidence/`, with
`SHA256SUMS` `f19dc51abc10ba31c7477aadcaef9f862d86d317dda53ad2ba9652b50d2e3240`,
which includes d69 candidate and trunk (earlier manifests: `b1c2c599…` with the d69
candidate, `877b4b9d…` with the queued-cancel control,
`66f3d971…` with trunk, `73726b51…` candidate only), and `RESTORE-INDEX.md`. Admission thumbnails and banner crops show the hosts' own
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

- ~~The fixture inventory shows 0 PUT requests and 0 objects for each cancelled
  queued target, and 0 objects for the transfer targets.~~ **Void.** That check
  matched source file names, but the editor exports artifacts under generated names
  (`Screen-Capture-…`). The survivors' own objects did not match either, so zero
  matches proves nothing about publication. The raw check is kept in
  `cancel2-results.json`, labelled flawed. The fresh queued witnesses and the
  `cancelled` terminal states still stand.
- The two remaining uploads completed and verified. Both runs drained to one
  process, exited 0, and left nothing.
- The witness and the cancel are separate requests. The held transfers make a
  promotion in between implausible, not impossible; atomicity is not claimed.

**Cancellation, payload-attributed control** (sa base, sb candidate):
[`lab/resident_burst_o3.py`](lab/resident_burst_o3.py) `I560_MODE=cancel3`, run by
[`lab/campaign_o3.sh`](lab/campaign_o3.sh) with the same setup as cancel2. Results:
[`d69/cancel3-results.json`](d69/cancel3-results.json),
[log](d69/campaign3.log).

The check decodes every stored object version and every PUT body in the fixture's
`/__state` to pixels, and matches its SHA-256 to each source's `pixelSha256`. That
is the same equality the frozen `verify_uploaded` checks. Names are never used.

| Check | Required | sa base | sb candidate |
| --- | --- | --- | --- |
| Each survivor attributed (objects / PUTs) | exactly 1 / at least 1 | 1/1, 1/1 | 1/1, 1/1 |
| Unattributed objects or PUTs | 0 | 0 of 4 + 4 | 0 of 4 + 4 |
| Queued targets (objects / PUTs) | 0 / 0 | 0/0, 0/0 | 0/0, 0/0 |
| Injection self-test (detected objects / PUTs) | 1 / 1 | 1/1 | 1/1 |
| Transfer targets (objects / PUTs), reported only | — | 1/1, 1/1 | 1/1, 1/1 |

- **Queued witnesses:** each said `queued` with 0 items. Settle times were
  0.005–0.053 s for queued targets, and 22.8–22.9 s and 1.25 s for transfers.
  Both runs exited 0 and left nothing.
- **Injection self-test:** run after the real inventory. It PUTs one queued
  target's own source PNG under a `Screen-Capture-…` key and must find it.
- **Transfer targets:** both cancelled transfers did store their object. Their PUT
  was already in flight, which is consistent with the in-flight contract in
  `docs/s3.md` and the native finalizing-body observation.
- **Attempts r1** (ra/rb, [log](d69/campaign3-r1.log),
  [script](lab/attempts/resident_burst_o3-r1.py)): both failed with `TimeoutError`.
  The run's 30 s PUT-delay rule also delayed the self-test PUT past its 10 s client
  timeout. The failure was in the self-test PUT, after the real assertions had run
  without raising, but no result event was recorded. r2 clears the rules before the
  self-test. Both failed runs are retained.
- **Same assertion:** the acceptance thread's native `queued_cancel.py` uses the
  same pixel-attribution semantics. The differences are a pre/post state diff, and
  calling an undecodable payload inconclusive where this check fails it; neither
  outcome ever passes. The two remain separate runs.

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

- **S3 module provenance (#579 Greptile 4372226982).** The probe and its worker
  resolve the S3 module relative to their own directory (see the configured
  module path in `CMakeLists.txt`). That path resolves to
  `~/work/i560o-h/lib/x86_64-linux-gnu/putgd/libputgd-s3.so`, not to the copy beside
  them.
  - That file is `ae6e0172…`, unchanged since 18:39Z, before the 19:23Z probe runs.
    It is byte-identical to the module built at both revisions.
  - No runtime mapping was recorded for those runs. Provenance is therefore
    inferred from the configured lookup, the single file at that path and its hash;
    it was not observed.
  - The adjacent copy made by `phase2_o.sh` was not on the lookup path.
- **Rebuild check:** [`lab/phase2_o4.sh`](lab/phase2_o4.sh) rebuilt both revisions
  into separate install-relative layouts
  ([base](d69/phase2_o4-rev-80ffa2e6-SHA256SUMS),
  [candidate](d69/phase2_o4-rev-98878cfe-SHA256SUMS)). The rebuilt probe and worker
  are byte-identical to the historical ones (`e73eda01…`/`d7529771…` and
  `0ab83a3a…`/`1156f233…`).
- **Relocated-bundle smoke:** [`lab/module_smoke.sh`](lab/module_smoke.sh) uploaded
  one file per revision. While the PUT was held, each worker's `/proc/PID/maps`
  showed its own revision's module
  ([output](lab/fault-checks/module-smoke-real.txt)).

The historical comparison therefore stands, and it was not rerun.
[`lab/phase2_o2.sh`](lab/phase2_o2.sh) was the first replacement for the build stage:

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

`phase2_o2.sh` had a real-binary bug that the short fake outputs could not expose
(#579 Greptile 4372226977). It checked `strings worker | grep -q` under
`pipefail`. On the real workers the identity string is line 1,746 of 38,771, so
`grep -q` exits early, `strings` gets SIGPIPE, and the pipeline returns 141 in
20 of 20 runs. A valid build would have been rejected
([check](lab/fault-checks/identity-check-real-binaries.txt)). It was never run on
real builds.

- [`phase2_o3.sh`](lab/phase2_o3.sh) writes the strings to an owned file first; the
  file form passes 20 of 20 runs.
- [`phase2_o4.sh`](lab/phase2_o4.sh) adds the module and its layout. It exits 8 if
  the module is missing and 9 if the embedded lookup path is wrong.
- [`phase2_o5.sh`](lab/phase2_o5.sh) adds argument preflight: at least one full
  40-hex revision, and canonical roots before any change or `cd`.

The fake `git`/`cmake` now quote every path; the earlier unquoted ones are under
[`fault-checks/r1/`](lab/fault-checks/r1/). Fault suites:

- [o3](lab/fault-checks/fault-results-o3.txt) and
  [o4](lab/fault-checks/fault-results-o4.txt);
- [o5](lab/fault-checks/fault-results-o5.txt), run from relative roots in a
  directory whose name contains spaces: the 9 fault cases above, plus no revision,
  a short revision, a missing output parent and a missing source. Each rejection
  exits non-zero, nothing is copied, and the patch is restored.

The source patch was restored in every case. The same results file shows
`campaign_o2.sh` logging `exit=37` for a runner that exits 37, and
[`lab/test_summarize_o2.py`](lab/test_summarize_o2.py) passing 6 cases: 30 s,
300 s, a peak before and a peak inside the window, partial data and missing data.

## Revised scripts

Historical scripts stay unchanged. The current scripts for any replay:

| Historical | Current | Change |
| --- | --- | --- |
| `resident_burst_o1.py`, `run_o1.sh`; `resident_burst_o2.py`, `run_o2.sh`, `run_o3.sh` | [`resident_burst_o3.py`](lab/resident_burst_o3.py) (r3), [`run_o4.sh`](lab/run_o4.sh) | Explicit roots and payload-attributed `cancel3` (see below) |
| `campaign_o.sh` … `campaign_o4.sh` | [`campaign_o5.sh`](lab/campaign_o5.sh) | Status captured before logging; failure returned; runs `run_o4.sh` through `bash` |
| `phase2_o.sh` … `phase2_o5.sh` | [`phase2_o6.sh`](lab/phase2_o6.sh) | Fail-fast, verified identity and module layout, preflight, source restore; clean-tree check before OUTROOT exists |
| `summarize_o.py` | [`summarize_o2.py`](lab/summarize_o2.py) | Timestamp-based final-270 s window, only after a full settle |
| `m74-run.sh`, `m74-run-ci.sh` | [`m74-run-v2.sh`](lab/m74-run-v2.sh) | Returns the driver status; lock session is a parameter |
| `functional_ci.py`, `f-run.sh`, `f-run-v2.sh` | [`f-run-v3.sh`](lab/f-run-v3.sh) | Refuses to launch without an explicit dedicated clipboard scope; `f-run.sh` alone owns the work directory |
| `module_smoke.sh` … `module_smoke_v3.sh` | [`module_smoke_v4.sh`](lab/module_smoke_v4.sh) | Arguments and canonical roots checked first; maps parsing handles spaces and empty polls; probe status kept; raw probe output kept byte-for-byte, with any classification in a separate file |
| `identity_check.sh` | [`identity_check_v2.sh`](lab/identity_check_v2.sh) | Requires a full expected revision and a present worker; an absent identity fails |
| `test_wrappers.sh` (**do not run**) | [`test_wrappers_v2.sh`](lab/test_wrappers_v2.sh) | All output inside its own `mktemp` directory; never creates or deletes fixed campaign paths |

- **`resident_burst_o3.py` r3** (#579 Cubic 4372241070): the script now exits non-zero
  when the resident exits abnormally, the forced fallback was used, or tracked
  processes remain. A workload failure stays the primary error, and the cleanup
  evidence is written first. [`test_harness_status.py`](lab/test_harness_status.py)
  checks this with fake runs: 6 classification cases and 5 propagation cases
  ([output](lab/fault-checks/harness-status.txt)). Earlier runs' exit-0 product
  evidence stays with those runs.
- **`run_o4.sh`** (Cubic 4372241065): canonicalizes SRC, HARNESS and APP before
  `cd`. Its preflight-only path test passes relative arguments containing spaces,
  and rejects a missing APP, a relative OUT, a missing HARNESS and too few
  arguments ([output](lab/fault-checks/run_o4-paths.txt)).
- **`f-run-v2.sh`** (Cubic 4372241074): f1's clipboard side effect is a
  **historical deviation**. It is documented above and not fixed after the fact.
  The replay runner refuses to launch unless
  `I560_CLIPBOARD_SCOPE=dedicated:<approval>` names a clipboard dedicated to the
  test. It never reads, saves or restores clipboard contents. Without a scope it
  exits 3 and creates nothing; with an invalid scope it also exits 3; with no label
  it exits 2 ([output](lab/fault-checks/f-run-v2-guard.txt)). A nested display
  would be a separately labelled diagnostic, never physical evidence.
- **Second review round** (#579 Greptile 4372270472, 4372270477, 4372270482). Three
  bugs, each fixed in a new script; the old bytes are kept.
  - `f-run-v2.sh` created the work directory that `f-run.sh` creates itself under
    `set -e`, so an approved run always failed. `f-run-v3.sh` leaves the directory
    to `f-run.sh` and writes the scope receipt as a sibling file before dispatch.
  - `campaign_o3.sh` and `campaign_o4.sh` executed `run_o3.sh`/`run_o4.sh`
    directly, which fails when a checkout keeps the committed 100644 mode.
    `campaign_o5.sh` runs it through `bash`.
  - `module_smoke.sh` used SRC and OUTROOT after `cd`, so relative roots broke.
    `module_smoke_v2.sh` checks its arguments and canonicalizes the roots first.
    Testing v2 exposed two more bugs that v1 shares:
    - its maps pattern `/[^ ]*libputgd-s3\.so` cannot match a path with spaces;
    - an empty poll, before the module is mapped, ended the script under
      `set -e`/`pipefail`.

    The retained real smoke was unaffected: its paths had no spaces, and the
    module was mapped at the first poll.
  - The other current launch paths call their scripts explicitly: `run_o4.sh` and
    `m74-run-v2.sh` call `python3`, the harness uses `sys.executable`, and
    `phase2_o5.sh` launches nothing.
  - [`test_wrappers.sh`](lab/test_wrappers.sh) ran from a clean `git archive`
    export, where the scripts have non-executable modes: 12 of 12 pass
    ([output](lab/fault-checks/wrapper-tests-clean-archive.txt)). It uses stubs
    only: no product, no display, and only a stub loopback server.
    - `f-run-v3.sh`: refusal without a scope or with an invalid one, permitted
      dispatch to a stub runner, an existing output, and a missing label.
    - `campaign_o5.sh`: a mode-644 stub runner passing, exiting 37 (logged, and the
      campaign returns 1), and missing arguments.
    - `module_smoke_v2.sh`: relative roots with spaces, using a stub fixture, a stub
      probe and a compiled stub worker that loads its module the way the real one
      does; plus a missing TAG, bundle or SRC.
- **Third review round** (#579 Cubic review 5645176997). The historical bytes and
  logs are kept; each fix is a new file.
  - **4372333033, 4372333035, 4372333038:** `campaign_o3.sh`,
    `attempts/campaign_o3-ra-rb.sh` and `campaign_o4.sh` run their runner directly,
    which needs an executable mode. They are historical: the ra/rb and sa/sb runs
    used copies made executable on d69, and `campaign_o4.sh` was never run. The
    replay route is `campaign_o5.sh`, which runs the runner through `bash`. No
    rewrite.
  - **4372333039:** `phase2_o5.sh` created OUTROOT before rejecting a dirty tree,
    which blocked a retry. `phase2_o6.sh` checks first. Stubs: a dirty tree exits 3
    and creates no OUTROOT; an existing OUTROOT exits 2 and its content is kept.
  - **4372333040:** the no-scope case in `test_wrappers.sh` inherited any exported
    `I560_CLIPBOARD_SCOPE`. `test_wrappers_v2.sh` exports a hostile scope and runs
    the case under `env -u`: it still refuses with exit 3 and creates nothing.
  - **4372333045:** `identity_check.sh` took the identity from the worker itself,
    so a missing identity became `grep -qF ''` and passed. `identity_check_v2.sh`
    requires a full expected revision and a present worker; a wrong worker exits 1,
    and a missing worker or short or empty revision exits 2. The retained
    real-worker log stays valid for its scope: it printed a non-empty identity
    (`g80ffa2e6c0d3` and `g98878cfe31c2`) for each real worker.
  - **4372333048:** in `module_smoke_v2.sh`, a non-zero `wait` under `set -e`
    skipped the status and failure record. `module_smoke_v3.sh` captures the status,
    writes a failure result if the probe produced none, always stops the fixture,
    and returns non-zero. Stubs: pass; probe exit 7 recorded; missing result
    recorded as `missing_or_invalid_result`; no stub fixture left running.
  - **Follow-up on 4372333048:** v3 replaced a malformed `result.json` with
    fallback JSON, destroying the probe output that explains the failure.
    `module_smoke_v4.sh` keeps `result.json` byte-for-byte and writes the
    classification, with the raw byte count, to `result.classified.json`, which the
    checker reads instead. `probeExit` is kept. Stub run from a clean
    `git archive` of `821fafbf`, 5 of 5 checks
    ([output](lab/fault-checks/module-smoke-v4-clean-archive.txt)): a normal pass
    uses the raw file; malformed output from a probe exiting 5 survives
    byte-identical; the run fails with exit 1, `probeExit` 5 and
    `missing_or_invalid_result`; and the stub fixture stops.
  - **4372333052:** a real ownership defect. `test_wrappers.sh` checked that the
    fixed paths `/tmp/p560ua`/`ub` were absent and then later ran `rm -rf` on them,
    so a real campaign starting in between could lose its evidence. It is kept
    only as history and **must not be run**. In `test_wrappers_v2.sh` the stub
    runner writes only inside the test's `mktemp` directory, and the test removes
    only that directory and its own sentinel. A final check confirms that the state
    of `/tmp/p560ua`, `uab`, `ub`, `ubb` and a foreign sentinel is identical before
    and after.
  - **4372333053:** the `head` line in
    [`fault-results-o5.txt`](lab/fault-checks/fault-results-o5.txt) is the output
    of `ls "st none"`, run to show that the space-containing fake state directory
    held its state file. The raw file is kept;
    [`fault-results-o5.annotated.txt`](lab/fault-checks/fault-results-o5.annotated.txt)
    is a derived copy that says so.
  - **4372333054:** checked rather than accepted. The scripts are tracked as
    `100644`, not executable. `git archive` writes `0666 & ~tar.umask`, and the
    default `tar.umask` is `0002` (unset here), so the archive's tar headers really
    are `0664`. The logged 664 is that header and extracted mode, not a typo. It
    means non-executable, with group write coming from git archive's default.
  - [`test_wrappers_v2.sh`](lab/test_wrappers_v2.sh) ran from a clean
    `git archive` of `f728a98a` through `bash`: 22 of 22 checks pass
    ([output](lab/fault-checks/wrapper-tests-v2-clean-archive.txt)). It covers
    f-run-v3, campaign_o5, phase2_o6, identity_check_v2 and module_smoke_v3, plus
    the ownership check.
- **Receipts** (Cubic 4372241047): the committed m74 release receipts are
  **sanitized projections**, with the private coordination-notes path removed from
  the lock purpose. The originals (SHA-256 `b235b55b…` for m74 and `63d87a9b…` for
  m74-ci) stay unchanged in the m74 archives below and on m74.

## Retained evidence

| Archive | SHA-256 | Where |
| --- | --- | --- |
| `evidence-m74-c1-t1-t2.tar` (1003 members, runtime sockets excluded) | `fd7cda2f062fa5eabb438eaa2304e82ab40c90af531604ce76d9b82f9b13b752` | m74 `~/work/i560o/`, workstation task `work/i560o/m74/` |
| `i560o-d69-evidence.tar` (1909 members) | `d3c77779d5bad10ecf0135b248716df0e3bdf2ca1219008dbb04a4b2f04f88e1` | d69 `~/work/`, workstation task `work/i560o/` |
| `evidence-m74-ci-e1-m1-f1.tar` (431 members) | `2ff4726efd80a682f03efc6272faa71ffe99ce72156a7c9ea9e376a2ff8bbd71` | m74 `~/work/i560o/`, workstation task `work/i560o/m74ci/` |
| `i560o-h2-evidence.tar` (136 members, corrected cancel control, summary v2, full ctest log) | `b42ec747efdacc8c7e03ea53d634a91dff5f05431708c7d569b6cc9b4d31ceb5` | d69 `~/work/`, workstation task `work/i560o/` |
| `i560o-h34-evidence.tar` (323 members: cancel3 r1/r2, real-binary identity check, `phase2_o4` layouts, module smoke) | `3699d08a3c8fe00cf46b8e7508386d80bfc01c14eb300f1beaef9efc0c2a0338` | d69 `~/work/`, workstation task `work/i560o/` |
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
  Matched-trunk GNOME and Plasma rows are recorded too. The d69 candidate and
  trunk rows are recorded too. Still pending: the native in-flight cancellation
  assertion, which predates #576 on every desktop. The queued-cancel
  control passes; that assertion remains a recorded mismatch.
- `historyOpenPss` fails on the exact candidate (m1) until #558's fix is included.
- Published-asset gates, after a nightly containing #576 publishes.
- `captureBurst` and `historyOpenUpperBoundMs` remain unassessed by the driver.
- The candidate lacks the separate #558 and #254 fixes. This is not an
  integrated stable acceptance.
