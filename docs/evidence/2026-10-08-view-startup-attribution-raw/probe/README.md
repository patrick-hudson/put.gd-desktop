# BASE41 startup attribution probe

This is a locating measurement only. It is the recent original source41 archive
plus an explicitly recorded diagnostic overlay. Its supplied compiled source label
describes the base archive; it does not prove that the binary contains original
unmodified bytes. It is not a product variant, a latency saving, or native desktop
certification. Current product and independent GUI polish are not used here.

`prepare.py` verifies the original archive SHA-256 and all 1,348 source hashes,
extracts it without modifying the original, applies only the recorded instrumentation,
and checks every unchanged source hash. `instrumentation.patch` and
`source-provenance.json` record every change. `build.py` reproduces the original
native host release configuration/build commands using ordinary C++20, Qt, CMake
presets, Ninja and the existing official SDK. `build-commands.json` retains exact
arguments/status; final provenance includes each runtime/fixture binary and module
hash, compile command hash, CMake cache/Ninja hashes and compiled base-label hash.

Instrumentation changes `src/ui/main.cpp`, `src/ui/UiSession.cpp`,
`src/ui/SettingsWindow.cpp`, `src/ui/UiApplicationController.cpp`,
`src/app/UiProcessController.cpp`, and adds `src/ui/StartupTrace.h` in this overlay.
Stages are main entry, pre/post QApplication, post VisualStyle and branding,
accepted bootstrap, snapshot request/reply/accepted, pre/post ready send, Settings
constructor enter/exit, Settings activation enter, show pre/post, and shown send.
Resident launch and process-started markers are included for launch attribution.
All trace values are fixed labels, PID and monotonic nanoseconds on stderr. The
resident relays only a bounded strictly validated trace grammar; ordinary helper
stderr remains discarded. Native fonts/dialogs/branding, widget construction,
private frames, epoch/build/classified-state validation and lifecycle are unchanged.

Parent alone owns graphical replay. Wait for all build/test/native desktop work
to quiesce before any replay. Keep the physical Cinnamon graphical session visible.
No VM access or host pixels/clipboard/upload activity is needed. To print the exact
replay plan after build completion (this does not interact with the display):

```sh
python3 work/view-startup-latency/probe/replay.py --output work/view-startup-latency/probe/run-1
```

Parent can then append `--execute` to run the frozen production
`tests/performance/bench_ui_process.py` with three views/three cycles,
warmup5/visible2/quiet30/interval5. The validated original private empty fixture is
`/tmp/putgd-startup-reclaim-exp001`. No edits to the fixture or frozen harness are
made. Each output directory must be new. The plan checks all binary hashes and the
frozen harness hash before invocation. `--allow-dirty` visibly labels the workload
as diagnostic even though the supplied base manifest cannot express overlay dirt.

`parse_trace.py` exports only the fixed safe timestamp grammar and Settings-stage
intervals, correlated by helper PID to workload cycle. It retains CLI shown
acknowledgement separately from independent mapped observer latency; their
difference includes observer overhead. Raw harness family identity records remain
the authority for PID/start/executable identity. Show return/CLI readiness does not
prove mapped or painted pixels. This does not establish p95, populated history or
post-upload memory. Instrumentation adds overhead and must not be scored as a
product optimization.

```sh
python3 work/view-startup-latency/probe/parse_trace.py --log work/view-startup-latency/probe/run-1/resident.log --results work/view-startup-latency/probe/run-1/results.json --output work/view-startup-latency/probe/run-1/stage-attribution.json
```

Preparation validation: Python syntax checks; parser checked against synthetic
safe/invalid trace records and independent CLI/mapped times. Build and replay
status are recorded in `source-provenance.json`. No graphical/runtime/native run
is performed by this preparation agent.
