# Hunting data races and ordering bugs

Two tools find what an ordinary test run does not: ThreadSanitizer with the instrumented Qt
of [thread-sanitizer.md](thread-sanitizer.md), and `rr record --chaos`, which runs every
thread of the recorded processes one at a time in a randomized order and replays a failing
run exactly. Run both on a build server, headless, never on a desktop session. The drivers
are in `tools/lab/`:

| Tool | What it does |
| --- | --- |
| `race_loop.py tsan` | Loops `action_fuzz.py` seeds against a ThreadSanitizer resident, optionally under busy loops. |
| `race_loop.py rr` | The same under `rr record --chaos`; a clean seed's traces are removed with `rr rm`, a finding keeps them. |
| `rr_loop.py` | Loops one CTest test (or one test function) under `rr record --chaos` and keeps the trace of a failing run. |
| `tsan_summary.py` | Groups TSan report files: put.gd frames first, Qt/GLib/third-party internals after. |
| `ctest` under `setarch x86_64 -R` | The unit and integration suites against the instrumented build. |

## ThreadSanitizer

Follow [thread-sanitizer.md](thread-sanitizer.md) for the instrumented Qt, then run with

```sh
export TSAN_OPTIONS="halt_on_error=0:ignore_noninstrumented_modules=1:history_size=5:second_deadlock_stack=1:suppressions=$PWD/tests/sanitizer/tsan.supp:log_path=$PWD/tsan-reports/tsan"
export GST_REGISTRY_FORK=no
```

`ignore_noninstrumented_modules=1` stops reports that exist only because GStreamer, FFmpeg,
libcurl, OpenSSL or the AWS SDK are distribution builds: their threads synchronize with
atomics TSan cannot see. It is an option, not a suppression; put.gd's own code is checked as
before. `GST_REGISTRY_FORK=no` keeps GStreamer from starting its distribution
`gst-plugin-scanner`, which would load the instrumented GLib without libtsan and crash.
`halt_on_error=0` with `log_path` collects every report of a run in its own file.

The suppressions in `tests/sanitizer/tsan.supp` are limited to Qt 6.4 internals whose racing
accesses contain no put.gd code: Qt's own `QThread::started` hand-off in
`QNetworkAccessManager`, the `QNetworkReply` destructor chain, QtDBus's own thread, and
QtDBus's pending-call bookkeeping. Add one only after reading the report and showing that
the racing accesses are inside Qt. Never suppress a put.gd function.

The recording tests (`recording_storage_integration`, `recording_pipeline_throughput`)
cannot be certified under ThreadSanitizer: GStreamer is not instrumented, and the one
report that remains reads a `GstMessage` a GStreamer thread released. Tests that need a
desktop service (labwc, an accessibility bus, KDE or Hyprland) or the Release poster helper
fail on a builder without it, with or without ThreadSanitizer; those are environment, not
races. `gui_application`, `gui_settings_general` and `scroll_session_contract` run past
their own 60 s and 180 s timeouts under ThreadSanitizer.

Run the action fuzzer against the instrumented build:

```sh
export ACTION_FUZZ_SLOW=4          # every wait of the driver is multiplied by this
python3 -I tools/lab/race_loop.py tsan --build build/tsan-qt \
  --tsan-prefix work/sanitizer-diagnosis --out ~/work/races/tsan \
  --first-seed 1 --steps 120 --hours 3 --stress 2
```

`--stress N` starts N busy loops of its own (stopped by PID) to shake the schedules. A seed
that finds a report stops there and keeps `finding.json`, `steps.jsonl` and the reports in
`san/`. `tools/lab/tsan_summary.py --source . <reports folder>` groups reports by the top frames
of their two accesses and lists the ones with a put.gd frame first.

## rr

rr needs real performance counters (a bare-metal builder, not a VM) and
`kernel.perf_event_paranoid <= 1`. Use a build with debug information (`RelWithDebInfo`),
not the ThreadSanitizer build. Traps found on the builders:

- rr 5.9 aborts on glibc 2.43's `madvise(MADV_GUARD_INSTALL)` (Ubuntu 26.04 and newer);
  build rr from its master branch there. rr 5.7 from Ubuntu 24.04 works with glibc 2.39.
- rr aborts on btrfs's subvolume-info ioctl, which Qt 6.10 issues on a btrfs home. Put the
  test's `TMPDIR` and the fuzzer's `--out` on tmpfs or ext4.
- rr is several times slower, so a test's own time budgets can expire. A request timeout in
  the S3 worker (`curlCode` 28), a helper killed after its grace period, or a Proton
  transport timeout under rr is the slowdown, not a race. Raise `--time-scale` (`rr_loop.py`)
  or `ACTION_FUZZ_SLOW` and compare with an unrecorded run before reporting it.
- A background shell job ignores SIGINT and SIGQUIT, and every child inherits that. The
  fuzzer resets both for the resident; start your own helpers the same way when a test
  checks how they die.
- End a recording with SIGTERM (`stop_recording` does) so rr finishes the trace; a SIGKILL
  leaves it incomplete and unreplayable.

Replay a trace with a gdb server so a script can drive it:

```sh
rr replay -s 41999 -k path/to/trace &
gdb -batch -ex "target extended-remote :41999" -x commands.gdb
```

Break on the functions on both sides of the suspected race and print the order they run in.

## What the first hunt found (2026-11-17)

On m74 (Qt 6.4.2, ThreadSanitizer) and k72 (Qt 6.10.2, rr 5.10):

- The suite under ThreadSanitizer showed one data race in put.gd code: the log writer read the
  retention without the lock while a settings change replaced it (#1587, fixed in #1590).
- Two tests shared data with a server thread without synchronization, in `test_hyprland` and
  in `test_s3` (#1595, fixed in #1599).
- `rr record --chaos` reproduced the `http_uploader_integration` flake of #1393 in 2 of 35 runs
  and the replay showed its order: the stored outcome is visible in the history database
  before the operation is released, so an immediate Retry gets `operation_busy` (#1578, test
  fixed in #1580).
- Fuzzer seeds under ThreadSanitizer (default and S3 destinations, `crash` mode, with and
  without busy loops) and under rr found nothing else in the resident, `putgd-ui` or the
  S3 worker. A report from a helper that ThreadSanitizer cannot see into (GStreamer in
  `putgd-record`) is a limit of the tool; a recording-pipeline race needs another way to be found.
