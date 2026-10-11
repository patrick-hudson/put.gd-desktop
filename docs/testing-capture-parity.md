# Replaying the capture regression investigation

This is a diagnosis archive from October 9, 2026. Feature parity is **not established** by it. Source49 met Cinnamon's memory target in the separate native-listener experiment, but capture startup regressed. The two-stage helper preparation change was still under development when this packet was captured; it has no acceptance claim here.

The [structured packet](evidence/2026-10-09-capture-parity-debug.json) contains 34 capture runs, exact binary/harness fingerprints, commands, exit receipts, all numerical samples and synthetic operation outcomes. Its manifest identifies every exported file, transformation and SHA256. Dirty and instrumented locator builds are explicitly labeled. A base Git hash with `.dirty` is not a clean source version; complete transient source snapshots were not saved for every variant. Binary fingerprints still identify what actually ran.

Run `python3 docs/evidence/capture-parity-debug/verify_export.py` to verify the archive and recompute every saved budget. Success means the archive is internally consistent, including its failed gates; it does not mean the application passed.

## What the evidence establishes

| Run | Samples | p95 outline ms | Outcome |
| --- | ---: | ---: | --- |
| Clean source49 baseline |30|285.69|Fails 150 ms |
| Dirty theme + GL-none control A |30|488.98|No GL win |
| Dirty theme-only control B |30|308.54|Fails 150 ms |
| Clean source45 contemporary control |18|Unavailable|Incomplete map/paint attempt retained |
| Dirty native selector A |0|Unavailable|Startup failed; retained |
| Dirty native selector B |30|214.96|35 cancellations; fails 150 ms |
| Dirty native selector C |30|215.45|35 cancellations; median 138.17 ms still does not pass p95 |
| Dirty PERSIST/FULL journal variant |30|251.43|Rejected; original journal policy restored |
| Dirty two-stage helper preparation A |30|195.37|35 cancellations; still fails 150 ms |
| Instrumented two-stage phase run |5|Unavailable|11 operations: one completion, ten cancellations; failed/incomplete |
| Instrumented input/screen diagnostic |30|189.11|35 recognized Escapes and cancellations; unscored |
| Dirty default-screen prewarm A |30|168.62|35 cancellations; fails 150 ms |
| Instrumented default-screen phases |30|266.13|Fails; unscored attribution only |
| Dirty server cursor cache A |30|289.33|35 cancellations; fails 150 ms |
| Unchanged cursor cache B repeat |30|190.16|Same binary hashes; 35 cancellations; fails 150 ms |
| Instrumented cache phase diagnostic |30|194.55|35 cancellations; unscored journal/phase attribution |
| Cursor-cache syscall diagnostic |30|167.23|Aggregate synchronization locator; unscored |
| Dirty cache + TRUNCATE/FULL A |30|296.55|Fails; later rejected |
| Unchanged TRUNCATE/FULL B |30|197.73|Fails; later rejected |
| Dirty cache + PERSIST/FULL A |30|305.64|Fails; original policy restored |
| Completed source45 contemporary repeat |30|703.85|Complete failing control; host-pressure context retained |
| Instrumented TRUNCATE phase diagnostic |30|117.17|Raw score passes; unscored rejected-policy diagnostic |
| Original policy with named old VMs stopped |30|228.08|Still fails; 35 cancellations |
| Narrow finalizer/user-version/sync diagnostic |30|356.64|Unscored; long same-thread synchronization |

The original helper's Qt/theme initialization was a material cost. Phase traces locate the subsequent native carrier/show and managed exposure waits. They are locating data, with instrumentation overhead, rather than clean release timing proof. Independent source reasoning and uninstrumented GL controls did not establish a GL initialization cause; the override was removed.

The preferred native-phase summary is `phases/native-phases/summary-nearest-rank.json`. It uses the 29th sorted observation for 30 samples. The earlier `summary.json` used index 27 rather than 28: accepted-worker p95 changes from 75.039338 to 75.375867 ms. Both originals are retained. Earlier first-paint summaries also retained the last point rather than first point for a repeated phase; first-point corrections are saved separately. Do not silently replace the raw samples or cite superseded summaries as corrected values.

For journal synchronization, `phases/journal-sync/commit-syscalls.json` correlates calls with the **same thread's** accepted commit interval. Three slow commits took 78.40, 81.63 and 77.45 ms, with 76.913, 78.867 and 75.662 ms in synchronization. They had zero failed lock attempts and approximately 0.067, 0.172 and 0.103 ms in locking calls. These intervals do not support statement preparation or lock retry as the main cause of those tails. Other constructor/schema waits in the same archive remain present; the finding must not erase them. Numeric FDs were recorded without paths, so the archive cannot identify a slow FD as the main database, rollback journal or directory.

PERSIST reuses rollback-journal storage, but still synchronizes its invalidated header and does not remove FULL synchronization. The measured variant did not improve the required gate, so it was rejected. Its provisional tests are preserved as `rejected/test-history.patch`; they were unbuilt, unrun and restored from the working source. They are not passing recovery evidence. [SQLite atomic commit](https://sqlite.org/atomiccommit.html), [journal and synchronous settings](https://sqlite.org/pragma.html).

## Two-stage preparation remains under investigation

The untraced early preparation locator improved some timings but still failed: nearest-rank p95 was 195.369348 ms over 30 samples, with 35 cancelled operations. The instrumented follow-up ended after 11 invocations and 5 measured samples with a map/paint error, p95−1, one completed operation and ten cancellations. Its generic parameter scope says all selections were cancelled; actual terminal states contradict that label. Preserve the contradiction and the failed attempt. Do not score it, discard its completion or treat it as parity evidence.

The phase analyzer has six groups after excluding five warmups; the sixth is the failed invocation, so its six-group summaries do not equal six valid timing samples. Its cursor preparation median was 45.515 ms, and acquiring writes often completed before readiness. These are incomplete diagnostic spans. They do not prove a clean candidate passes 150 ms. At this snapshot the temporary source markers were restored, while the release executables still carried instrumentation pending a standard rebuild.

Five affected development suites passed, then the full development suite passed 48/48 before the final buffered-frame guard. The actual helper test subsequently passed; removing only the fatal-frame decoder stop/settled guards made it fail with one unexpected post-ready error. Both begin frames were sent in one pipe write, and the desktop row’s one-pixel budget made unintended acquisition admission observable. The guard was restored, followed by a final affected 3/3 pass. At that stage no full 48/48 rerun after the guard was recorded; the later full pass under rejected TRUNCATE is separately scoped below. These isolated Xvfb tests cover protocol admission, invisible preparation, input ownership and cancellation; they do not establish Cinnamon compositor acceptance. Zero encoded bytes alone would not prove that no screen read occurred.

Bounded structured JUnit projections under `tests/` preserve case names, counts, exits, original XML fingerprints and the negative assertion. Arbitrary suite stdout is omitted. The successful socket preflight receipts also show that the wrapper was exercised on both attempts before application launch, with 54-byte socket paths; this validates preflight execution, not capture performance.

## Cursor lifetime and screen-cache diagnosis

The input/screen diagnostic recorded 35 Escape events and 35 cancellations. In all 35 invocations the prewarm screen differed from the foreign-wrapper parent screen. The user confirmed physical input during the earlier incomplete phase attempt; that attempt stays archived as failed/incomplete rather than being repaired retrospectively.

Warming the default QWindow screen reduced the untraced locator to 168.621035 ms p95 over 30 measured samples, with 35 cancellations. It still fails the frozen 150 ms gate. The instrumented follow-up had 266.130854 ms p95; its matched-screen cursor preparation took 39.056021 ms median and 58.337369 ms p95, while native show fell to 0.0612425 ms median. These diagnostics locate the initial themed-cursor load; they do not establish performance acceptance.

An XFixesChangeCursor lifetime experiment was rejected. After the producer client exited, pointer grab status remained 0 (success), but XFixesGetCursorImage returned BadAccess 10. A successful grab alone therefore missed a cursor-inspection regression relevant to VNC. The corrected primitive creates a new cursor from owned ARGB data; its isolated cursor comparison passed after producer exit. The packet exports numerical error observations, source hashes and bounded JUnit projections, with cursor pixels and expected image/hash values omitted.

The first untraced server-cache run completed 30 samples and 35 cancellations with an empty error field, but failed at 289.333194 ms p95. Median map/paint were 52.373946/102.871638 ms; two late map observations were 276.667098/372.041627 ms. The unchanged repeat used identical binary and harness hashes and failed at 190.160250 ms p95, with medians 78.426436/155.8631315 ms. A faster typical path does not satisfy the tail gate, and a clean error field does not imply performance acceptance. Both attempts remain available; further attribution, Cinnamon memory and compositor parity are pending.

The new cursor-cache protocol integration is still being tested. Passing primitive/integration suites do not establish compositor, memory or physical timing parity. The user installed the normal host `libxcursor-dev` package; Fedora’s development package is `libXcursor-devel`. Use the existing CMake/Ninja builds and distro packages. The resident must preserve its lightweight boundary while helper/bootstrap ownership and cleanup remain tested.

## Cache review and latest diagnostic limits

The cache-phase diagnostic retained a 532.81 ms history-open maximum and 532.61 ms initialization maximum, despite sub-millisecond medians. History commit p95 was 79.731108 ms; accepted-worker p95 was 94.056241 ms. Constructor/schema outliers and commit tails both remain in the archive. The aggregate syscall summary contains 414 fdatasync calls: 20 exceeded 30 ms, maximum 74.22 ms. No fcntl call exceeded 30 ms. This summary is unscored and does not correlate a given call to a same-thread commit or identify its file. Its raw syscall log is deliberately excluded. Its saved launcher identity records the exact `strace -D -f -qq -ttt -T -e trace=fsync,fdatasync,fcntl` flags, resident/descendant scope and direct forwarding of version probes. Reproduce tracing only as a separate unscored diagnostic with private logs; do not add FD paths, payload tracing or arbitrary logs to the packet. The aggregate cannot be rederived from this export because its raw calls are intentionally absent.

The bounded external review receipt verified Claude family independence and the actual requested model; requested effort was high, but actual effort remains unverified. The final review verdict is **Not ready**, with three historical findings statically addressed: use the full frozen layout in the cache key, bound and clean up the source-disconnect probe, and exercise the real seed/protocol path. Static resolutions do not establish runtime acceptance. Original residual risks, including file-only theme changes, remain recorded. The source provenance verifies installed libXcursor 1.2.1 behavior without claiming runtime incidence on a supported desktop.

The first actual-helper seed/Qt proof failed because the test probe did not negotiate XFixes; its corrected retry passed. Controller protocol/layout cases passed. These passes preceded deterministic-theme fixture changes. The latest final-dev run passed 3/4 suites and failed when the generated static theme returned an empty ARGB seed (zero image count). The failed run stays part of the acceptance record. A subsequent image-count correction passed the preparation suite; the full dev suite then passed 48/48 under TRUNCATE, a policy later rejected. These passes do not establish an original-policy full rerun or physical/memory parity.

An earlier snapshot proposed TRUNCATE/FULL. Subsequent TRUNCATE A/B and cache PERSIST/FULL attempts failed and were rejected. Both durable checkpoints remain required, and the original journal/test files were restored to HEAD. WAL has not been selected while the installed SQLite 3.45.1 reset-race concern requires version/fix verification. Do not substitute a journal-mode theory or static review for the unchanged capture, feature and memory gates.

## Host visibility correction and rejected journal variants

Earlier sandbox-only checks suggested the old VMs were absent. That claim was invalid as evidence of host absence and remains recorded as superseded. Root’s host-escalated checks verified the exact old QEMU processes 2205651, 2215650 and 2204498 using start ticks, executable hash and owned overlays. The archived `lab.py reset` commands stopped those three user-requested old sessions. Afterwards those same process identities were absent and the overlays were preserved. This establishes only the named process shutdowns, not global VM absence. The saved I/O pressure counters remained high immediately afterwards; shutdown alone does not prove the source of every latency tail.

The completed source45 control had 30 samples, no harness error, and p95 703.845909 ms despite median map 30.580909 ms and median paint 144.8651065 ms. Its executable/CLI fingerprints match the saved source45 archive provenance; dirty state is unknown (`null`) in that supplied build metadata. The original resident therefore also exhibited large tails in this host environment. This failing control limits claims that the process split alone caused shared tails; it does not pass any gate.

TRUNCATE/FULL cache attempts A/B failed at 296.548848/197.731752 ms p95; PERSIST/FULL cache A failed at 305.644234 ms. A full 48/48 development pass ran under TRUNCATE before that policy was rejected. The generated-theme image-count correction also passed. Narrow read-only hashes now confirm `HistoryStore.cpp` and `test_history.cpp` match WT HEAD; this does not imply that the entire candidate is clean or that an original-policy build was accepted. At this snapshot root was rebuilding the original-policy candidate for another isolated replay. Those results are outside this archive; current functional parity and memory gates remain unaccepted.

## Corrected history pairing and narrower trace

The original cache/TRUNCATE analyzers merged the first marker for each phase in an invocation without retaining thread identity. The worst cache group combined accepted TID 3282792’s commit start with prior TID 3282748’s end, inventing a −89.639230 ms duration. The original summaries remain intact. Use `summary-thread-paired.json` for history statistics: each begin/end uses the same PID/TID, worker ownership is exact, additional/overlapping workers are listed, and 35 total invocations remain separate from 30 measured invocations after five warmups.

Corrected cache accepted commit median/p95 is 6.822337/79.731108 ms; acquiring commit is 2.716992/8.578159 ms. TRUNCATE measured accepted commit is 3.791663/18.114662 ms; acquiring commit is 3.031498/4.917938 ms. TRUNCATE’s 229.479492 ms initialization maximum belongs to warmup index 2, not the measured 30. Its instrumented capture trace has a raw 117.170299 ms p95 pass flag. Preserve that flag, but do not promote the diagnostic or rejected journal policy into clean acceptance.

The older cache outlier remains real after correction: accepted initialize took 532.612788 ms. The prior worker’s 600.663781 ms commit started −156.578517 ms and ended +444.085264 ms relative to accepted begin. Initialize ended +532.757472 ms, 88.672208 ms after the prior commit marker. That overlap is consistent with a contention/retry hypothesis, but does not identify the exact blocked call, lock-release point or sleep schedule. SQLite 3.45.1’s Unix VFS can reject same-process handle conflicts with SQLITE_BUSY before an OS lock call; therefore zero failed fcntl calls cannot rule out in-process contention. [Pinned upstream source](https://github.com/sqlite/sqlite/blob/version-3.45.1/src/os_unix.c#L1683-L1696).

The newer seven-column diagnostic uses exact worker PID/TID and per-marker realtime clocks. It did not reproduce that 532 ms initialization. Its worst measured worker, index 19 (sample 14 after warmups), instead committed in 270.594567 ms with 268.275 ms fdatasync, a 171.402 ms maximum sync call, user_version execution 0.772477 ms and no prior finalizer overlap. Index 20 committed in 96.050506 ms with 94.531 ms sync. Warmup index 4 showed approximately 19 ms user_version execution with prior-finalizer overlap. Keep both observations conditional; neither a generic SQLite contention claim nor a mutex explains every tail. Raw syscalls are excluded, with their full fingerprint saved; the complete correlation requires the protected local raw trace.

After the three named old VMs stopped, the original-policy candidate still failed: 30 samples, no harness error, p95 228.082128 ms, median map 53.8543485 ms and median paint 93.914905 ms. This limits the VM-load explanation without proving the host was otherwise idle. Current functional, memory and clean timing acceptance remain open.

Replay the corrected numerical pairing without starting the application:

```bash
python3 docs/evidence/capture-parity-debug/analyze_history_threads.py \
  --trace docs/evidence/capture-parity-debug/phases/cache-phases/phases.log \
  --output /tmp/pg-history-cache-corrected.json
python3 docs/evidence/capture-parity-debug/analyze_history_threads.py \
  --trace docs/evidence/capture-parity-debug/phases/truncate-phases/phases.log \
  --output /tmp/pg-history-truncate-corrected.json
```

Compare those outputs byte-for-byte with the corresponding `summary-thread-paired.json`. The narrower diagnostic’s historical analyzer additionally needs its raw syscall file, which this export deliberately omits. Temporary source instrumentation was restored; release binaries still contained diagnosis markers at that handoff and require a normal rebuild before clean timing.

## Replaying numerical attribution

The packet excludes profile configuration, databases, clipboard/capture payloads and pixels, credentials, resident stderr, driver stderr and arbitrary debugger logs. Phase logs contain fixed labels and numerical clocks/process/thread identifiers. The syscall export allows only numerical synchronization and locking calls; signal/exit notices were omitted and the original trace fingerprint is recorded.

From the repository root:

```bash
python3 docs/evidence/capture-parity-debug/phases/native-phases/analyze-nearest-rank.py
python3 docs/evidence/capture-parity-debug/phases/journal-sync/analyze.py
python3 docs/evidence/capture-parity-debug/analyze_sync.py > /tmp/putgd-sync-recomputed.json
```

The first two historical analysis scripts recreate their summaries alongside the archived trace. Run them in a disposable copy if preserving a byte-identical checkout is required, then verify again. The sync script prints derived JSON without modifying the packet. Its clock mapping uses the saved realtime/monotonic calibration, including resumed syscalls, and never introduces FD paths or buffers. These are source-local locators, not causal benchmarks.

Historical `instrument.py` scripts and pre-instrumentation source hashes are archived to explain the markers. They assert exact text matches and include historical absolute paths. Do not apply them blindly to a newer implementation or treat failed insertion/build attempts as timing evidence. Recreate equivalent bounded markers on a private candidate only after checking source identity, save original files and fingerprints, use standard CMake/Ninja, and remove every marker before clean acceptance. The malformed native-start instrumentation script is retained separately as a failed attempt.

## Running a new clean candidate

Use C++20, distro Qt 6.4.2 or newer, normal CMake presets/Ninja and the documented official AWS SDK provisioning. Use the authorized distro Xcursor package with the existing toolchain; no Node fixture or replacement toolchain is required. First build and run the development suite:

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

After tests pass, build the normal Release preset with the existing physical benchmark option:

```bash
cmake --preset release -DPUTGD_PHYSICAL_BENCHMARKS=ON
cmake --build --preset release
```

 Use the SDK flags from `docs/build.md` for this checkout; record the resolved prefix, Qt/SDK identity, compiler, source tree, `--version` outputs and binary hashes. Do not reuse an old VM binary or describe a dirty locator as a versioned release. Preserve the tray, native settings fonts/dialogs, invocation focus/clipboard revision, cancellation, provider-neutral APIs and both separate durable accepted/acquiring checkpoints.

Drain host/guest builds before physical measurements. Record available CPU, memory and I/O pressure counters; high pressure is evidence to investigate, not permission to raise the gate or discard a failed run. Reserve the physical Cinnamon session for one measurement at a time. Its live-outline harness requires X11, scale 1 and a shared vertical strip across enabled displays. VM, offscreen and mocked checks cannot establish this physical budget.

Keep `tests/performance/run_capture.py` and its driver frozen for the comparison. Exact source49 copies are archived under `frozen-source49/`; all 34 historical runs recorded harness SHA256 `caf58fd076ce0c46835753739055bbda2ee3c2cfd1b0ce2822290f8b2783fb94`. These copies explain the workload; build through the normal project, not a separate fixture toolchain. It performs five warmups and thirty measured invocations, uses the real Ctrl+Shift+F12 shortcut, paints synthetic covering windows on every enabled display and cancels with Escape. The gate is nearest-rank p95 ≤150 ms. White drawable pixels include injection, query/readback and driver scheduling costs; they are not compositor-presentation or mixed-DPI evidence. Do not reduce sample counts, discard tails, remove ownership checks, change 100 ms cooldown or relax the threshold to make a candidate pass. An incomplete run has no valid p95.

Before launching an isolated app, validate **the actual resolved socket**, including the harness-created private runtime directory. `run_checked_capture.py` intercepts only temporary-directory entry and invokes the repository checker before the harness launches its first app. It leaves the frozen harness file and timing/input parameters unchanged, fingerprints the intended harness first, and saves `socket-preflight.json`. Use a short new output directory; the UTF-8 socket limit is 107 bytes excluding NUL.

```bash
harness="$PWD/tests/performance/run_capture.py"
harness_sha=$(sha256sum "$harness" | cut -d ' ' -f1)
python3 docs/evidence/capture-parity-debug/run_checked_capture.py \
  --harness "$harness" --harness-sha256 "$harness_sha" \
  --checker "$PWD/tools/diagnostics/check_socket_paths.py" -- \
  --app "$PWD/build/release/putgd" \
  --cli "$PWD/build/release/putgd-cli" \
  --driver "$PWD/build/release/putgd-bench-capture" \
  --capture-helper "$PWD/build/release/putgd-capture" \
  --live-outline --output /tmp/pg-cap-next1
```

Select and record the intended frozen harness hash before measuring; computing the current hash above documents that choice, not equivalence to a historical run. If the checker is available only in the main checkout, pass its exact path. For original resident-owned controls, omit `--capture-helper`. Keep ownership verification appropriate to the architecture. Preserve every attempt's command, parameters, version/hash identities, samples, exits and synthetic operation counts. Never include credentials or payloads in a review packet.

A past strace attempt failed because the next hotkey encountered `busy_selecting` after the unchanged 100 ms cooldown; it was not a confirmed painting crash. The current incomplete source45 control is retained as incomplete; it must not be scored or cited as a passing control. New failures need their own diagnosis rather than automatically assigning this explanation.

Only after the candidate's Cinnamon functional **and memory** gates pass may GNOME/KDE expansion begin. Verify actual native capture/window/desktop/clipboard actions, region cancellation, live transparent background, outline/cursor/DPI behavior, helper exit, native settings fonts/folder dialogs and tray persistence. Repeat cold and closed whole-family memory using the frozen memory harness and short-socket preflight; target ≤40 MiB on physical Cinnamon. Then test current-version builds on independent declared desktops, keeping graphical work visible through localhost VNC on the user's display wall. Record SSH build work separately from graphical acceptance. Agents restore locked sessions using protected local login input and the visible console. Do not boot or modify sealed VM bases.


## Approved early-selection boundary

On 2026-10-09 the user approved visible region selection before journal commits.
Earlier no-map/no-grab expectations in historical evidence describe the old
contract; bootstrap remains invisible under the new one. READY is followed by
one authenticated `preview` for region only, carrying the frozen generation,
nonce and cursor ID. Preview reads no screen pixels. BEGIN uses the same cursor
and follows both successful durable commits. The acquisition join is queued once
to drain buffered cancellation/rejection before entering the pixel path.

A completed rectangle retains bounded geometry and frozen target metadata, with
no image buffer or live selector. Pixels are acquired after BEGIN, so changing
content can differ from the frame at mouse release. Selection is bounded to 120
seconds; completed geometry waiting for BEGIN is bounded to 10 seconds. An empty
owned private result.png lease may exist, but image bytes may not. The exact
frozen target/layout and limits must be valid at selection and pixel acquisition.

OperationService latches early terminal metadata on its session thread without
calling the ordinary payload continuation. Successful acceptance remains the
only basis for acknowledging an operation. Failure of either journal checkpoint
wins over a simultaneous early Escape; after a successful checkpoint explicit
operation cancellation wins, then the first early terminal event. Terminal
persistence may itself fail and must not be reported as a successful cancellation.
The current rollback-journal durability settings are unchanged.

Regression evidence and build/test receipts for this boundary are under
`work/capture-parity/early-selection/`. Synthetic Xvfb events and fixture protocol
checks establish gates and cleanup, not physical compositor performance. Run the
unchanged physical timing test and clean-version whole-family memory checks
before declaring parity or expanding to current-source GNOME/KDE.


### Archived early-selection regression receipts

The safe archive is `docs/evidence/capture-parity-debug/early-selection/`,
indexed and hashed by `docs/evidence/2026-10-09-capture-parity-debug.json`.
It retains the expected failing operation/controller/cancellation-precedence
attempts beside subsequent focused green results and standard CMake build
receipts. Parsed synthetic JUnit summaries exclude system output, credentials,
configuration, databases and payloads. These results establish the synthetic
protocol/widget checks exercised; they do not establish physical performance.

Activity layout evidence includes its corrected missing-second-line red test,
focused green and GUI-suite pass. Its initial setup crash is recorded separately
as test fixture misuse: a cached row pointer was invalidated by asynchronous
refresh. The corrected test resolves the current row inside each wait. Full
file paths remain in tooltips/copy behavior; font-scaled two-line file cells
require current native visual acceptance in addition to widget checks.

The reduced cold-memory summary describes the earlier dirty investigation
binary preceding early-selection implementation. Its one-process family
observation does not establish final candidate memory acceptance. All prior
371 archived artifacts and timing failures remain retained. Freeze the current
candidate identity and finish Cinnamon functional, timing and memory gates
before expanding to GNOME/KDE or reporting parity.

The later dirty integrated development attempt passed 47 of 48 suites and is
retained as a failed full run. Its `x11_isolated` failure assumed synchronous
cancellation in an existing test. The assertion-only correction waits for the
documented async callback and preserves independent clipboard completion and
payload checks; the affected X11 suite then passed. This chronology is not a
single full 48/48 run. The archived source hash map and supplementary X11 test
hash identify the tested edits; clean-version physical acceptance remains open.


### Source drain and journal completion

The first early-preview physical run completed 25/30 measured samples then
rejected a hotkey as `busy_selecting`; all 30 admitted operations cancelled.
Keep its incomplete result and frozen timing parameters. Favorable partial
samples do not establish the 150 ms gate.

A prepared helper now reports release only after its child, decoder and owned
artifact have drained. OperationService tracks source ownership separately from
terminal journal completion. It retains the active operation slot until both
are complete; a drained cancelled selector can admit the next hotkey while the
old journal finishes, and a fast journal cannot unlock a still-live helper.
Callbacks carry the exact operation identity, so a stale release cannot clear a
new selector. Legacy adapters remove only the helper marker at preparation and
retain their ordinary interactive reservation until acquisition completes.

Pre-BEGIN terminal metadata is queued before source release. An installed
ordinary callback means BEGIN already owns terminal delivery, so no early
metadata from that generation can be pending: errors populate the ticket before
notification, and BEGIN checks that ticket first. Completion detaches the release,
ordinary callback and result after cleanup, delivers normal release first, and
accesses no controller state afterward. Either callback may destroy the controller.
Owner teardown suppresses pending early metadata/release callbacks.

Late BEGIN preserves the running 120-second preview-selection deadline. It may
start a new helper pixel budget after geometry has completed; the resident's
preview deadline remains an overall bound. Independent Anthropic review of the
preceding frozen packet identified the timer issue; the subsequent source-release
extension has local static and separate runtime review, not a peer attestation.


### Integrated pre-commit confirmation

The later source-release revision passed all 48 development suites, including
the helper drain ordering, legacy reservation and callback-destruction regressions.
The unchanged physical live-outline run completed five warmups and all thirty
measured samples: p95 95.945 ms, median 81.074 ms, with all 35 unique operations
cancelled and `no_result`. The preceding incomplete 25-sample run stays archived.

These results identify a dirty investigation build, not a clean versioned release.
The witness reads white outline pixels from owned window drawables; it does not
prove compositor presentation, captured pixel correctness, mixed DPI, S3 or the
final cold/closed memory gates. A final combined static scan found no actionable
issue in the cursor, selector, helper and operation lifecycle changes. The later
release extension has local review and runtime coverage; the earlier bounded
Anthropic packet is separately identified.


### Runtime Qt cursor-loader boundary

The source53 KDE distro build (Qt 6.10.2, libxcb-cursor 0.1.6) failed the real-helper
static cursor equality test: the helper selected the environment theme, while Qt
selected the resource/default theme. Preserve `work/capture-parity/clean53/kde-dev-failure.json`, SHA256
`f555418c6ba8a5ccf71d55c200f94c7a012b16b1ed765a7e5158f213d1b00d19`
and its recovered dev JUnit SHA256
`51531f8517a2a8351b565756886bb3da231e877a1d2c1f81c3eae8d4b3e3c8cd`.
They are source53 failures; the earlier Cinnamon gate remains historical evidence.

[Qt 6.4.2](https://code.qt.io/cgit/qt/qtbase.git/plain/src/plugins/platforms/xcb/qxcbcursor.cpp?h=v6.4.2)
uses libXcursor (source SHA256
`62adba886c6a4c1b188dd62f2b8e572f121f3bf78c0667887191bbdfe601ae64`);
[Qt 6.5.0](https://code.qt.io/cgit/qt/qtbase.git/plain/src/plugins/platforms/xcb/qxcbcursor.cpp?h=v6.5.0)
already uses libxcb-cursor (SHA256
`144c2fde1a65f1bf61391b1830a3de07f26a3c27a79ac3fea5c23b3d8c37b306`).
[Official xcb-util-cursor 0.1.6 source](https://xcb.freedesktop.org/dist/xcb-util-cursor-0.1.6.tar.gz)
(archive SHA256 `eae38b2dfc5c529a886e507ef576b12d2a20aa1f149608e4853af760f31be60b`)
uses the root resource theme and honors XCURSOR_PATH/SIZE, but ignores
XCURSOR_THEME. [libXcursor 1.2.3 source](https://xorg.freedesktop.org/archive/individual/lib/libXcursor-1.2.3.tar.xz)
(archive SHA256 `fde9402dd4cfe79da71e2d96bb980afc5e6ff4f8a7d74c159e1966afb2b2c2c0`)
adds a display-based resizing path that the helper's image lookup cannot reproduce.

The optional cache therefore uses the **runtime** Qt version: Qt 6.5 and newer
clear/bypass resident cache lookup, request no seed and send cursor ID 0 at
bootstrap/preview/begin. Qt supplies its normal native cursor. Explicit
XCURSOR_RESIZED also disables the cache; the helper declines display-based
resizing when the public runtime XcursorGetResizable API is present. Existing
1.2.1/1.2.2 headers lack that API, so it is resolved through QtCore QLibrary;
library-load failure declines the seed. No extra package or private Qt API is used.
The frozen layout now hashes cursor environment values and presence bits
(unset differs from set-empty) as well as root/XSETTINGS
metadata; file-only theme changes still require separate invalidation evidence.

After the normal dev configure/build, replay:

```sh
ctest --preset dev -R '^(capture_control_integration|capture_process_integration|capture_preparation_x11)$'
```

 The actual-helper suite requires empty
seeds on newer Qt/resizing and retains exact dimensions, hotspot and every-pixel
Qt comparison on 6.4. Controller tests retain malformed-seed rejection and
normal helper cleanup, and assert all-zero fallback IDs when incompatible.
These private Xvfb checks establish protocol behavior only. A corrected candidate
still needs current-version visible Cinnamon functional/timing/memory gates before
GNOME/KDE expansion; no newer-Qt desktop, timing or memory acceptance is implied.
