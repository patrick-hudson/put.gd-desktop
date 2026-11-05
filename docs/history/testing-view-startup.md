# Replaying on-demand view startup measurements

> **Historical record.** This page describes an app version, experiment or plan that no longer exists and is kept for context only. The current design is in [overview](../overview.md); see [memory-strategy](../memory-strategy.md). Do not use it as a work list.

Keep the on-demand process split and resident tray. The original source-41
comparison reduced closed whole-family RSS by about 20 MiB, but its cold
five-minute peak was 44.390625 MiB. The 40 MiB target remains open. This document
covers Settings reopen timing; it does not replace capture, populated-history,
post-upload or cold-idle acceptance.

## Frozen workload

Use `tests/performance/bench_ui_process.py` without changing its observer.
The approved reference hash is
`a28c94b075036ea65f8eca8e068d480cff46ee959d4680593f5c4d55cce3fc40`.
Each process run uses split mode, Settings/History/Activity in that order,
three cycles, a 5-second resident warmup, 2 seconds visible after readiness,
30 seconds quiet after closing, and a 5-second sampling interval. Confirm a
candidate with two independent serial runs, giving four Settings reopen samples
(cycles 2 and 3). Compare their median with the original 332.6767 ms reference.
A saving must exceed 40 ms; the target is approximately 200 ms.

The same validated private fixture must have local actions, empty history,
no shortcuts, notifications or autostart, and no credentials. The harness
copies validated configuration into its own isolated state directory. Never
point it at the production configuration, history, captures or credential file.
Do not retain or prewarm an invisible helper. Normal last-window close must
checkpoint safely and exit the helper; resident shortcuts, clipboard, tray and
operations remain alive.

Freeze the exact commit and archive before building. Record archive/source and
app/CLI/helper/module hashes, the compiled manifest, and PID/start/hash identities.
An instrumented or uncommitted overlay needs its complete patch and changed-file
hashes; its base commit label does not identify its executable bytes. Preserve
failed attempts under new output names. Never relabel an earlier binary after a
documentation commit.

Check resolved Unix socket paths before launching the harness. It appends
`runtime/putgd/session.sock` to each run's output directory; Linux accepts at
most 107 UTF-8 path bytes, excluding the terminating NUL. Count the complete
absolute path, including run labels. Use short private output names, then
export the safe results into descriptive evidence directories after measurement.
Run `python3 tools/diagnostics/check_socket_paths.py --ui-output RUN_1 RUN_2`
for the complete planned output paths before launching. The command reads no
credentials, creates no directories and exits 2 for an overlong socket path.
A 115-byte socket path prevented the first source45 reference from starting;
that failed attempt remains saved, with zero accepted samples. The repeat uses
a 94-byte path and the identical frozen harness and workload.

## Native correctness and quiet timing

Start with one Cinnamon build and its required functional and memory checks;
stop and fix or reject a failed gate before building/testing elsewhere. Acceptance
still requires current-source native evidence on Cinnamon/X11, GNOME/Wayland and
KDE/Wayland.
After Cinnamon passes, GNOME and KDE checks can run in parallel. Keep guest
output on each existing localhost VNC listener
and visible through the display wall. Stop verified old app residents before
starting replacements; preserve immutable VM bases and historical evidence.

Complete real region/window/desktop and Qt/GTK clipboard tests, region
cancellation, view reopen, draft/Cancel behavior, full-ID routing, independent
result-copy paste, normal helper exit and rendered tray evidence. Offscreen
widget tests establish contracts, not compositor acceptance. Report a missing
native case or uncertain compact geometry as an open gate.

Drain all compilation and active desktop testing before physical timing. Run
only one measured family at a time on the same Cinnamon session. Keep native
fonts and folder dialogs enabled. Preserve per-run command, environment summary,
fixture identity, structured events/samples, CLI acknowledgement and independently
observed mapped readiness. The observer includes process/window/diagnostic work;
neither a `show()` return nor CLI acknowledgement proves painted pixels or p95.

Reject a candidate exceeding 46.5 MiB closed RSS, 29 MiB closed PSS or 60 MiB
maximum sampled open PSS. These whole-family gates account for resident and helper;
summed RSS counts shared pages twice. The workload's success flags establish only
this empty three-view exercise. Verify native receipts separately.

## Locating measurements

The first BASE41 diagnostic overlay retains source and binary provenance and
fixed stage-only stderr markers. It exports no settings, widget text, secrets or
payloads. Native Qt initialization, control/bootstrap, snapshot validation,
Settings construction and showing are timed separately. Its observed reopened
constructor/show cost was 116–129 ms, versus under 7 ms for the handshake.
Those values guide experiments; diagnostic overhead prevents treating them as a
product saving. Hidden-page cost attribution precedes any lazy-page refactor.

The [attribution receipt](../evidence/2026-10-08-view-startup-attribution.json)
exports both complete diagnostic patches, source/binary provenance, normal build
commands, frozen workload results, safe numeric trace records and the parser.
The fine probe located 30.0–32.6 ms constructing hidden Capture, Shortcuts and
Preferences pages, plus 11.3–11.5 ms of their exclusive Polish dispatch during
`show()`. These intervals are disjoint; their aggregate is 41.3–44.1 ms. This is
located gross cost, not a measured saving. Other layout work is untracked and
lazy-state handling has its own cost. The selected variant defers those three
pages until expansion. Its later correctness and timing results are recorded below.

To reproduce a locating probe in a new checkout:

1. Export the receipt's exact base commit (`97ca3a521714e2717cedc1139976130627bfb8a8`)
   with `git archive`, preserving the archive SHA and source hashes. Do not copy
   working-tree files or the local credential file. Extract into a new scratch
   directory; never reuse a previous measurement output.
2. Apply the chosen complete `instrumentation.patch` from the receipt's raw
   directory with normal `patch -p1`. The fine patch already includes the first
   probe's instrumentation; do not apply both patches. Verify every resulting
   changed-source hash against `source-provenance.json`. This is a diagnostic
   overlay, even when its compiled version label names the base commit.
3. Replay `build-commands.json` using the ordinary release CMake preset and Ninja,
   adjusting only checkout/SDK paths to existing documented installations. Keep
   the recorded base commit/tree/count overrides for archive builds, dynamically
   linked distro Qt, the official SDK, lab fixture flag and jobs=2. Record actual
   toolchain/dependency/source/binary hashes; do not claim byte equivalence after
   changing compiler or libraries.
4. Validate the private empty local-only fixture with the production
   `bench_views.fixture_config`; the fixture procedure is in
   [memory testing](testing-memory.md). Drain compilation and graphical acceptance
   work. Run the frozen `bench_ui_process.py` command recorded in `results.json`,
   using new output and rebuilt app/client/helper paths, the same three-view
   parameters, original archive, and `--allow-dirty` for the declared overlay.
5. Run the exported `parse_trace.py` on the new `resident.log` and `results.json`.
   Require zero rejected trace records, complete stages and monotonic counters.
   Preserve the actual failure if these checks fail. Export only the fixed numeric
   stage/Polish grammar; do not copy general application stderr into review packets.
6. Record workload, fixture and source/binary identities beside the parsed results.
   Compare constructor, show and mapped observer separately. Do not use these
   diagnostic runs as a production candidate score or native acceptance receipt.

The JSON evaluator is Python; guest fixtures use existing C++/Qt, Python/GTK and
GNOME GJS runtimes. Do not introduce Node to a guest or host test requirement.

The local optimization ledger is
`.context/compound-engineering/ce-optimize/view-startup-latency/`.
It retains the approved specification, opportunity estimates, source digest,
raw attempt paths and decisions. Durable evidence must include the final hashes,
commands, patches, structured results and unresolved gates before scratch cleanup.

## CI and agent use

The existing GitHub workflow preserves JUnit, CTest logs, lab-contract reports
and build identity for 30 days. Its containers run compiler/core/widget and host
protocol checks; they do not establish native desktop or physical memory budgets.
A future native performance job needs a dedicated graphical runner, the same
fixture and workload, current-source native acceptance, exclusive timing and
artifact upload even after failure. Do not substitute a hosted container's timing
for this physical comparison. Each agent should inspect the actual installed
build/capabilities before replay and record the exact effect and operation IDs.


The GNOME extension ownership fixture runs with the desktop's existing GJS
interpreter (`gjs -m tests/desktop/test_gnome_ownership.mjs
integrations/gnome/extension.js`). It uses mocked Shell dependencies and provides
no graphical acceptance. Node is not a host/guest test requirement. Where GJS is
absent, run the shared C++ contracts and obtain this extension-specific receipt
from the GNOME lab; do not install another JavaScript engine for it. File loading
uses the [GJS/GIO file API](https://gjs.guide/guides/gio/file-operations.html).


Source43 native replay is saved in `docs/evidence/2026-10-08-ui-refinement-native.json` and its raw directory. It includes twelve capture/clipboard/cancellation cases per desktop and bounded Settings/History/Activity checks. GNOME locked-start tray rendering and KDE destination-button continuity during resize remain explicit source43 gates; neither is dismissed as a VNC issue. These post-suite RSS/PSS/private/shared snapshots are attribution, not quiet budgets.

The late-tray correction is commit45, independently reviewed with a red/green
visibility-intent test; see `docs/evidence/2026-10-08-tray-late-availability.json`.
The source45-plus-patch native correctness receipts remain explicitly dirty in
`docs/evidence/2026-10-09-lazy-settings-native.json`. They are not scored timing
results. The isolated experiment is now frozen as clean commit
`6f6372e79d8bd81346924aacec7eac842db98725`, version
`0.1.1-dev.46+g6f6372e79d8b`, with those same four reviewed settings/test files.
It is an experimental checkpoint, not a keep decision or main-branch integration.

Clean46 has passed twelve native capture/clipboard/cancellation cases on each
of Cinnamon, GNOME and KDE. All three also passed lazy-page keyboard traversal,
Settings/History/Activity close/reopen, normal helper exit and visible tray checks.
The clean46 package and live binary identities are recorded separately from the
older dirty cohort. The first Cinnamon setup attempt reached a locked session and
aborted before any matrix case; its failure is retained, followed by protected
visible-console unlocking and an explicitly repeated successful matrix.

Quiet post-native-use guest observations contain seven samples over thirty seconds,
one resident and no helpers: Cinnamon 53.63 MiB RSS / 33.55 MiB PSS / 32.19 MiB
private; GNOME 60.51 / 33.87 / 30.34 MiB; KDE 140.43 / 40.69 / 28.41 MiB.
KDE's LLVM mapping contributes 48.28 MiB RSS, 5.98 MiB PSS and 2.14 MiB private.
These observations explain some dashboard differences; they establish neither a
cold physical result nor an optimization saving. The approximately 40 MiB cold
RSS target remains unmet. Serial physical startup confirmation follows the native
checks, comparing clean46 both with the original source41 contract and a fresh
matched clean45 reference to separate the lazy-page contribution from other changes.

## Confirmed startup result

The [clean46 confirmation receipt](../evidence/2026-10-09-lazy-settings-confirmation.json)
contains the native checks, commands, per-process samples, exact build identities
and both decision inputs/outputs. Two alternating serial runs per build produced
four reopen samples each. The matched clean45 median was 332.5025 ms; clean46 was
295.4320 ms, a 37.0705 ms (11.15%) improvement. Against the original source41
332.6767 ms reference, the difference was 37.2447 ms (11.20%). Neither exceeds
the approved 40 ms comparison threshold or reaches the 200 ms target.

The installed decision tool returned **inconclusive** for both comparisons.
The trial is not integrated into the product branch. Preserve its clean checkpoint
and referenced worktree for replay; a small observed gain is not proof that the
approved keep criterion passed. All correctness and memory gates passed: candidate
closed family RSS 45.58 MiB, closed PSS 25.835 MiB and maximum sampled open PSS
50.68 MiB. These are the empty-view workload's observations, not cold-idle results.
The reference/candidate sample ranges overlap; four samples do not certify p95.

The locating forecast was 41–44 ms of gross hidden-page work, with net benefit
explicitly unknown. The measured net mapped gain is smaller. The approved loop
stopped with one inconclusive variant and no remaining runnable hypothesis;
protocol coalescing was not selected because its located upper bound was under
7 ms. One overlong-socket setup attempt had zero accepted samples and remains
preserved separately. Native receipts and post-use guest memory do not establish
full compositor coverage, U9, cloud acceptance or the cold 40 MiB target.

The subsequent [cold source45 reference](../evidence/2026-10-09-cold-reference45.json)
measured 45.414 MiB peak/final RSS over five minutes, with no views or operations.
It is separate from the empty-view and post-native guest observations. Native
initialization attribution is recorded in
[its own receipt](../evidence/2026-10-09-native-initialization.json); a Core-only
constructor without a working tray and capture pipeline is not a product win.
Compatible memory changes may be combined, but require a measured integrated
whole-family candidate rather than summing independent savings.
