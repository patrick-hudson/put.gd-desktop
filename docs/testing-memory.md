# Physical memory measurements

Use the production Release build and dynamically linked distribution Qt described
in [build.md](build.md). The separate instrumented dependencies in
[thread-sanitizer.md](thread-sanitizer.md) establish thread-checking evidence;
their memory footprint is not a production baseline. These commands require the
actual physical Cinnamon X11 session, Linux `/proc`, Python 3, `systemd-detect-virt`,
`xprop` and `xwininfo`; the view diagnostic also uses `wmctrl`.

Run physical measurements serially. Keep the private test resident untouched
during cold idle: do not open its views, invoke capture/clipboard actions, or run
another physical benchmark alongside it. Use a new short `/tmp` directory for
each attempt so its local socket fits the Unix path limit. The harnesses isolate
configuration, data, cache and runtime roots; measurements contain no screenshots,
clipboard/source payloads, credentials, or cloud requests.

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release
python3 tests/performance/observe_idle.py \
  --app build/release/putgd --cli build/release/putgd-cli \
  --output /tmp/putgd-idle-replay-unique --seconds 300
```

`observe_idle.py` launches the unchanged
[`bench_idle.py`](../tests/performance/bench_idle.py), with the same arguments,
warmup and sampling. The frozen harness creates a private validated local-only
configuration with no profiles, disabled shortcuts, notifications and login
startup. It records RSS from `/proc/<pid>/statm`, resident CPU ticks over elapsed
time, binary hash, hardware/session identity and initial/final `smaps` attribution.
Footprint maps contain library basenames or anonymous labels, never full mapped
paths. Its legacy budget requires at least 300 seconds, sampled RSS at most
100 MiB and CPU below 0.5% of one core. That budget is distinct from the requested
approximately 40 MiB optimization target; a legacy pass does not meet that target.

The observer stores `/tmp/putgd-idle-replay-unique-windows.json` beside the frozen
results directory. Every second it finds the benchmark's resident children and
queries EWMH client IDs, then checks `_NET_WM_PID` before reading map state. It
exports only owned IDs and visibility, not titles or unrelated window metadata.
On completion it reads only operation counts from the isolated journal. Missing
observation/journal evidence, mapped owned windows, nonzero operations or a failed
benchmark prevent a successful uninterrupted-idle result. Visibility is sampled:
a window can appear and disappear between polls, and override-redirect windows
need not be in the EWMH client list. Zero recorded operations and the original
workload constraints remain necessary. The observer itself adds host work;
compare using the same observer when evaluating candidates.

## Empty view lifecycle

Use a successful full idle run as the fixture after that run has stopped:

```sh
python3 tests/performance/bench_views.py \
  --app build/release/putgd --cli build/release/putgd-cli \
  --fixture /tmp/putgd-idle-replay-unique \
  --output /tmp/putgd-views-replay-unique \
  --cycles 3 --idle-seconds 30
```

The diagnostic verifies that the recorded fixture passed the physical idle
budget and has zero operations, empty profiles, local-only actions, disabled
shortcuts and disabled notifications/login startup. It copies only its validated
configuration into fresh private XDG roots and updates the archive root to the
new private directory. It does not copy the journal or any payloads.

The workload warms up for five seconds, samples cold memory, then opens Settings,
History and Activity through production `ui.show` calls. Each activation settles
for two seconds. It expects one, two and three mapped PID-owned EWMH windows,
respectively. It closes only those owned windows with `wmctrl`, waits up to five
seconds for them to hide, waits the requested hidden interval, and samples again.
Three cycles establish whether the empty views can be reopened and show the
sampled retained footprint. Native IDs may change across hide/show; the harness
checks owned window counts rather than requiring stable IDs.

`progress.json` preserves partial samples and `results.json` records completion
or failure, parameters, selected package/Python versions, binary/CLI/harness
hashes, command arguments and the fixture's source hashes. Output/configuration
files are private. Failures retain their output directory; do not reuse it for a
retry. This view workload is a post-use diagnostic, not a five-minute cold idle
measurement, a leak proof, a permanent plateau proof, populated-history/upload
acceptance, or the approximately 40 MiB gate.

## Retained continuation evidence

[The continuation attribution export](evidence/2026-10-08-memory-continuation-attribution.json)
contains the source journal hash, protocol, measurement hashes, all failed
attempts, sample summaries and a manifest of complete sanitized raw result and
partial-progress JSON under `evidence/2026-10-08-memory-continuation-raw/`.
The earlier comparison remains in
[the resident-memory record](evidence/2026-10-08-resident-memory.json), with its
[review receipt](evidence/2026-10-08-memory-review.json).

The completed reference at revision `1d4f2ef14c6dbf88e91e4744e3c6bea5b73b5827`
used frozen harness SHA256
`47e65e026ebd3a2a596224036ef594cf30770a49f4ff1dbc462dbf0f6c95e652` and Release
binary SHA256
`29c3906f7884a823ba880c1618e85883a1b0fcb22d77d414dd9a0462429b5430`.
Its uninterrupted five-minute peak was 59,027,456 bytes (56.293 MiB), with CPU
0.01666% of one core. It passed the legacy idle budget and missed the 40 MiB target.
The first continuation attempt was invalid idle evidence: the private journal
contained a user-invoked cancelled region operation, peak RSS was 102,387,712
bytes and CPU was 0.54967%. Its failed result is retained, including the mismatch
between its generic raw scope string and the journal evidence.

The successful empty-view run reached 88,088,576 bytes (84.008 MiB) at final hidden
idle after three cycles. Earlier observer attempts failed because a transient
window disappeared during lookup, then because the observer incorrectly required
stable native IDs. Neither is recorded as an application failure or a passing
measurement. The stable harness uses EWMH IDs, PID ownership and window counts.

Temporary allocator instrumentation reported glibc `mallinfo2` counters. The
post-view probe sampled at 4, 15, 35, 75 and 110 seconds and called `malloc_trim(0)`
once at 35 seconds in the temporary build. Before trimming, allocated arena bytes
were 15,612,944 and free arena bytes were 1,364,976; the arena shrank by only
126,976 bytes. This result does not support production trimming for that workload.
The separate cold probe paused after a five-second warmup and around one trim,
using SIGSTOP/SIGCONT for external `smaps` sampling. RSS fell by 1,216,512 bytes;
that is a locating result, not a scored implementation or an idle acceptance run.

The allocator record retains a sandbox display-startup failure, a whole-tree X11
observer race, and the first cold observer's buffered-stderr timeout. The corrected
cold observer used an unbuffered pipe (`bufsize=0`). Startup/theme probes and
allocator instrumentation were temporary, and their application source was
restored. Their environment flags are **not production switches**. The user chose
to preserve Cinnamon-native fonts and dialogs; the temporary public Qt theme
selection was excluded from implementation.

Historical scratch harnesses did not record full argv or all package versions.
The export labels reconstructed commands and export-time package inventory
explicitly. Raw measurement hashes remain authoritative; the canonical observer
and view harnesses have new hashes after validation/metadata improvements. The
observer completed a fresh 300-second reference run at 56.270 MiB with 294
PID-owned visibility polls, no observed mapped window and zero operations. The
review-fixed candidate completed the same observer workload at 54,407,168 bytes
(51.887 MiB), CPU 0.003331% of one core, with 292 visibility polls and zero
operations. The reduction from the repeat reference is 7.789%. The actual decision
script retained it on the optimization branch at `cadd9ed`; canonical integration
passes all 26 development tests. The original 51.996 MiB candidate sample remains
in [the SDK memory record](evidence/2026-10-08-sdk-memory.json), alongside the final
source, review, binary identities and comparison. Native Cinnamon fonts and dialogs
were retained. The approximately 40 MiB target remains open. The module is pinned
after first use, so cold savings do not establish post-S3 savings.

Both replay scripts have now run. The final reviewed binary completed three cycles
of Settings, History and Activity with zero operations and a final hidden RSS of
83,415,040 bytes (79.551 MiB). This validates the empty-view command and gives a
post-use sample; it is not a leak or permanent plateau proof. The complete raw
result is retained under `2026-10-08-sdk-memory-raw/review-fixed-empty-views.json`.

A same-binary 15-second locating comparison skipped only custom palette/stylesheet
initialization, leaving native GTK integration enabled. The difference was 753,664
bytes (0.719 MiB), below the declared noise and improvement thresholds. No delayed
styling change was adopted. Source was restored, and both complete short results
are retained in the continuation attribution export. Short runs intentionally fail
the legacy 300-second gate and are not performance acceptance.

## Synthetic post-use allocator attribution

[The payload allocator record](evidence/2026-10-08-payload-allocator.json) retains
the exact protocol, ten operation IDs, binary/source fingerprints, native package
versions, complete sanitized results, controller and temporary source patch. Five
1280×800 desktop captures paired with 96×64 synthetic clipboard images reached
79,323,136 bytes (75.648 MiB) after completion and worker drain. One glibc
`malloc_trim(0)` call reduced RSS to 69,775,360 bytes (66.543 MiB); the five-second
follow-up was 69,791,744 bytes. Allocated arena bytes stayed at 3,366,480, so this
recovered freed pages rather than removing live payloads. Native fonts/dialogs
and the original lab resident/configuration were preserved.

This is a single disposable Cinnamon VM locating measurement. It is separate from
the physical cold-idle gate and uses no S3 requests. Its temporary allocator
control flag and polling timer are absent from production.

## Automatic cleanup after image work

The production service requests one glibc reclamation pass after accepted capture
or clipboard work, including cancellation, and after decoding an existing artifact
for an image/text copy. A two-second single-shot timer waits until tracked operation
workers, active actions, the selector, upload queue and decoded-image leases drain.
New background work stops that timer. The pass returns allocator-owned free pages;
live widgets, native fonts/dialogs and clipboard ownership remain intact. Other
allocators report reclamation unsupported. See `diagnostics.memory` in the CLI
reference; its count records attempts, which may reclaim no pages.

[The production record](evidence/2026-10-08-heap-reclaim.json) includes a replayable
five-cycle local capture/clipboard test, source and binary identities, all ten
operation IDs and complete sanitized process samples. Final drained RSS was
69,701,632 bytes (66.473 MiB), versus 79,323,136 bytes (75.648 MiB) in the earlier
attribution run. Each cycle requested one completed pass; the longest recorded
allocator call was 750 microseconds. These runs have different binary identities,
and do not establish a permanent plateau, post-S3 memory use, cold-idle savings or
end-to-end UI latency.

Replay with the production Release build and independent synthetic fixture:

```sh
python3 tests/performance/bench_payload_lab.py --help
python3 tests/performance/bench_payload_lab.py --run work/lab-runs/<disposable-cinnamon-run> --app build/release/putgd --cli build/release/putgd-cli --fixture build/release/putgd-desktop-fixture --output /tmp/putgd-payload-replay-unique --cycles 5 --settle-seconds 5
```

Use an existing disposable Cinnamon lab with its VNC connection available. The
command refuses a reused output path, creates private synthetic local-only
configuration, and restores the original lab configuration and resident afterward.
It does not invoke S3. A production image-release test verifies cleanup cannot run
while a slow upload is blocked; another holds a selector and an unrelated tracked
worker open. A test-first cancellation regression covers acquisition without an
enqueued payload. The final source passed all 25 development contracts and the
affected ASan/UBSan and instrumented-Qt TSan operation contracts.

## Native IPC memory reduction

The Linux resident uses native local sockets to remove its eager Qt Network
dependency while retaining Cinnamon-native fonts and dialogs. The reviewed fix
measured 46.020 MiB over 300 seconds, versus 52.012 MiB in the paired reference
(11.52% lower). CPU was 0.0133% of one core; no operations or owned window
activation were observed. Native GTK remains mapped and Qt Network is absent
from the cold resident. The 40 MiB target remains unmet.

The isolated source is retained as `783df0d` on `optimize/resident-local-ipc` and
integrated as `1fdfff0` on the implementation branch. Development contracts pass
28/28; affected Release, ASan/UBSan and instrumented-Qt TSan contracts each pass
4/4. The prior MCP/CLI suite passed 3/3 with synthetic peers. The combined committed app, including live selection and image cleanup, passed
28 development and four affected Release tests and measured 46.188 MiB over
another 300 seconds, with CPU 0.00666% of one core. This is a separately identified
whole-product measurement, not another isolated IPC contribution.

The first IPv6 differential test found a loopback-classification mismatch. Claude
review then prompted a reproduced pending-reply disconnect regression: the native
transport stopped observing clients that had quit. Both were fixed. The new
disconnect test fails before the fix and passes afterward; half-close behavior
matches original Qt on both Linux event loops. The original 45.625 MiB pre-fix
sample, failing tests, sandbox listener failures, reviewed fix, and fresh fixed
measurement remain distinct.

[Complete IPC evidence and replay parameters](evidence/2026-10-08-local-ipc.json)
include source/binary hashes, raw tests, review receipts, original/repeated
baselines and the fixed comparison decisions. Each run uses a fresh output path.
The two-hour/three-variant IPC scope adds no dependency or cloud resource; its
remaining product/desktop and post-S3 gates are not inferred from cold idle.

The approved startup experiment requests the existing idle-gated cleanup once
after successful recovery. Its unchanged five-minute physical workload fell from
46.1875 MiB at `2e789b8` to 44.9648 MiB: a 1.2227 MiB (2.65%) saving, above the
predeclared 1 MiB threshold. CPU was 0.0133% of one core. Native Cinnamon fonts
and dialogs remain enabled, and no new dependency was added. The change is kept
as `68e5549` and integrated as `fc69d38`; the 40 MiB target remains unmet.

The standalone Qt locating probe recovered about 1.77 MiB; the product saving
was smaller. These are one-sample stable-mode comparisons, not a statistical
confidence interval. Three native empty-view cycles reopened Settings, History
and Activity successfully, ending at 73.8242 MiB hidden RSS. That post-use figure
is separate from cold idle and does not establish a permanent plateau.
[Startup evidence, frozen parameters and raw reports](evidence/2026-10-08-startup-reclaim.json)
preserve both workloads and the source review. The local resume specification is
`.context/compound-engineering/ce-optimize/resident-startup-reclaim/spec.yaml`.

The hidden-view result contains 8,404,992 bytes of Xorg shared mappings, matching
three page-rounded 1000×700 RGBA drawing buffers. Local Qt 6.4.2 source explains
why retained widgets keep those backing stores after close. This is a strong
source-and-size inference, not object-level runtime proof. The additional
11.86 MiB heap RSS remains unclassified between live state and free allocator
pages. A separate candidate can test deferred release of hidden native backing
resources while preserving logical widgets and drafts. It must check complete
repainting, rapid reopen, minimize/restore, dialogs and selection state, and
measure RSS/PSS and reopen latency. It would address post-use retention, not
lower the never-opened cold baseline. [The source attribution and bounded test
proposal](evidence/2026-10-08-startup-reclaim-raw/hidden-views-attribution.json)
record the exact inspected sources and limitations; no such product change or
saving has been measured yet.

## SQLite journal locating evidence

The standalone rollback/FULL probe completed twelve fresh-process cases: two
matched repeats of connection/statement reuse and 2000/512 KiB page-cache caps.
Explicit preparation cost was small, with medians of 0.062–0.202 ms across three
durable states. Connection/statement reuse produced no consistent whole-iteration
winner; durable commit tails dominated. A controlled exclusive writer caused
`user_version` reads to wait, supporting lock-contention attribution without
proving the cause of every millisecond in the earlier product initialization
stall. No product SQLite change was made from this probe, and it does not pass
the 150 ms capture gate.

The 512 KiB cap lowered synthetic first-scan RSS growth by about 1.510 MiB;
actual paginated history and cold-idle savings remain unmeasured. Preserve the
probe limitation: a later write overwrites reader `beginNs`, so the absolute
reader-start timestamp is unavailable; individual phase durations and writer
timestamps remain usable. [The structured evidence](evidence/2026-10-08-sqlite-locating.json)
and [report/replay](evidence/2026-10-08-sqlite-locating-raw/report.md) retain all
420 raw samples, source/build/library identities, commands and limitations.
Databases and payload bytes are excluded; replay regenerates private synthetic
data. No WAL test or production recovery/concurrency acceptance is inferred.

## Integer widths and object layout

Use the standalone [layout inspection tool](../tools/diagnostics/layout/README.md)
before proposing narrower fields or member reordering. It uses normal CMake/Ninja
and the existing Qt headers, prints JSON metadata, and touches no application
state. Its instructions cover fresh build directories, consumed header hashes,
compiler/ABI identity, numeric bounds, live counts and the required workload
measurement after a candidate change.

The [recorded GCC 13.3 / Qt 6.4.2 inspection](evidence/2026-10-08-layout-inspection.json)
found `int`, `qint32` and `std::int32_t` all occupy 4 bytes; `qint64` and
`std::int64_t` occupy 8. Merely changing aliases saves nothing. A synthetic
narrowing example retained its 16-byte stride because padding grew; a separate
reordering example fell from 24 to 16 bytes. Those toy results are not product
savings. `sizeof` excludes allocations behind strings, JSON, images and widgets.
The empty cold workload has no operation/history population to shrink, while
the default 2 GiB file limit already exceeds signed 32-bit range. No production
integer change was selected. Reconsider only a measured dense population with
safe bounds and a meaningful `count × stride reduction`, followed by actual
RSS/latency and regression checks.

## Current-build desktop matrix

Start each memory candidate with one current-source Cinnamon build, its native
functional cases and its memory checks. Fix or reject failures there before
expanding to GNOME/Wayland or KDE/Wayland. Once Cinnamon passes, independent
distro builds and native guest scenarios may run in parallel; serialize actions
within each guest. Wait for compilation and active GUI testing to drain before collecting
quiet samples. Physical Cinnamon retains its serial, unchanged workload. VM data
must be labelled VM data and cannot certify the physical budget.

Pin a source revision/archive and save each distro package hash, installed app,
CLI, S3 module/helper hashes and the actual running app PID/start/executable hash.
The same version string is insufficient. Cover region/window/desktop, native
clipboard text/image/files, three region cancellations, and settings/history/activity
reopen before post-use measurement. Preserve separate cold, post-view and post-action
workloads; record missing cloud/publication or other acceptance gates explicitly.
The lab now verifies the running app against its install receipt before actions
and again before accepting success, including expected history restarts.

The [2026-10-08 VM audit](evidence/2026-10-08-vm-memory-audit.json) found that the
8767 dashboard was correct: KDE 179.22 MiB, GNOME 111.27 MiB and Cinnamon 56.11 MiB
RSS. These were old direct-AWS/QtNetwork builds, not the current cold candidate.
KDE PSS was 66.67 MiB; shared LLVM/Gallium accounted for 58.40 MiB RSS but 7.37 MiB
PSS. KWin reported llvmpipe. This explains substantial shared graphics accounting,
not the app's exact GL context, all retained heap or a leak. [Replay and limits](testing-vm-memory-audit.md)
retain the measurements and the corrected testing policy. Retire superseded test
VMs after saving evidence; keep necessary build caches powered off.

The [current-source follow-up](evidence/2026-10-08-current-desktop-matrix.json)
passed 12 native cases per desktop and repeated view opens before a common
two-minute observation. Post-use RSS was 83.30 MiB Cinnamon, 114.36 MiB GNOME and
186.17 MiB KDE; corresponding PSS was 54.51, 71.58 and 64.35 MiB. KDE's high RSS
therefore persists on current code, with 148.77 MiB shared resident pages and
37.40 MiB private. These are VM observations, not a cold physical optimization
comparison. The 40 MiB target remains open. Exact samples, workload differences,
failure/retry receipts and stopped obsolete VM identities are retained with the
report and [replay instructions](testing-vm-memory-audit.md).


## Native listener candidate

The experimental source49 Qt Core/native-tray resident passes the 40 MiB physical
Cinnamon cold and closed-view target: 24.676/24.555 MiB cold peak and 25.229 MiB
closed family median RSS. Source45 reference was 45.414 MiB cold and 45.600 MiB
closed. The change is not integrated because selector startup regresses; capture
timing and Wayland fallback footprints remain separate. The
[replay guide](testing-native-listener.md) and
[structured results](evidence/2026-10-09-native-listener.json) contain frozen
parameters, source/binary identities, raw samples and failed attempts. Do not
replace the 150 ms capture guard with the memory-only evaluator decision.
