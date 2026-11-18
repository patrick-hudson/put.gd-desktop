# Hunting data races and ordering bugs

Two tools find what an ordinary test run does not: ThreadSanitizer with the instrumented Qt
of [thread-sanitizer.md](thread-sanitizer.md), and `rr record --chaos`, which runs every
thread of the recorded processes one at a time in a randomized order and replays a failing
run exactly. Run both on a build server, headless, never on a desktop session. The drivers
are in `tools/lab/`:

| Tool | What it does |
| --- | --- |
| `race_loop.py tsan` | Loops `action_fuzz.py` seeds against a ThreadSanitizer resident, optionally under busy loops. |
| `race_loop.py rr` | The same under `rr record --chaos`; a clean seed's complete traces are removed with `rr rm`, a finding keeps them. |
| `rr_loop.py` | Loops one CTest test (or one test function) under `rr record --chaos` and keeps the trace of a failing run. |
| `tsan_summary.py` | Groups TSan report files: put.gd frames first, other reports next, excused Qt-internal races last. |
| `ctest` under `setarch x86_64 -R` | The unit and integration suites against the instrumented build. |

## ThreadSanitizer

Follow [thread-sanitizer.md](thread-sanitizer.md) for the instrumented Qt and the
`build/tsan-instrumented-qt` build, and run CTest as it shows, with
`ignore_noninstrumented_modules=1` and `GST_REGISTRY_FORK=no` in the command itself.

`ignore_noninstrumented_modules=1` stops reports that exist only because GStreamer, FFmpeg,
libcurl, OpenSSL or the AWS SDK are distribution builds without TSan. It has a cost. TSan then
ignores the memory and synchronization calls (`memcpy`, `free`, mutexes) those libraries make
through its interceptors. So a race in which one access is put.gd's and the other happens
inside such a library is not reported, and runs with the option do not cover those mixed races.
The option stays because a run without it is unusable: on m74 a fuzzer seed failed at step 9 on
12 reports from the recording helper's GStreamer plugins (mutex misuse, `new`/`delete` and
`getrandom` races). With the option, the same seed ran to step 20. `GST_REGISTRY_FORK=no` keeps GStreamer from starting its distribution
`gst-plugin-scanner`, which would load the instrumented GLib without libtsan and crash.
`halt_on_error=0` with `log_path` collects every report of a run in its own file.

The action fuzzer sets its own `TSAN_OPTIONS` (the same options, `halt_on_error=0`, and
`log_path` under each seed's `san/`), so do not export them for it. A seed fails on any report
except one on the known list (below). GCC's TSan makes the resident exit with status 66 when
it reported anything. The fuzzer accepts that exit only when the seed has TSan report files and
every report in them is on the known list; any other exit status still fails the seed.

What is excused, and how:

- **One D-Bus lock-order report** is suppressed in `tests/sanitizer/tsan.supp`: the
  `deadlock:_dbus_connection_new_for_transport` entry, a construction edge of a connection no
  other thread can reach yet. Its diagnosis is in [thread-sanitizer.md](thread-sanitizer.md),
  in the paragraph on the dependency-only lock-order report.
- **Qt-internal races are not suppressed.** A `race:` rule would match its symbol anywhere in
  either access stack, or in a thread-creation stack, and could hide put.gd racing with Qt.
  Reports are captured whole, and `tools/lab/tsan_summary.py` excuses only reports on an
  exact, checked-in known list (`KNOWN_QT_RACES`). A report is excused when all of this holds:
  - it is a data race with two racing access stacks;
  - the top six frames of the first stack, and of the second, are exactly the symbols of one
    known entry, in that order. Symbols are the demangled names TSan prints, with arguments,
    never paths;
  - neither stack has a put.gd frame above `main()`. A put.gd frame is one whose module is a
    put.gd binary or library (`putgd`, `putgd-*`, `libputgd*`), or whose symbol is in the
    `putgd` namespace. Frames below `main()` are not read.

  Everything else is shown, and it fails a fuzzer seed. Thread-creation, mutex and allocation
  stacks are not read.

  The list starts with the three report shapes of the `QThread::started` hand-off in
  `QNetworkAccessManager`, seen on m74 (Qt 6.4.2, GCC 13 TSan). Because no put.gd frame may
  appear above `main()`, the same hand-off is shown whenever put.gd code (not `main()`
  itself) started the request, which is how the resident starts requests.

  To add an entry:
  1. Diagnose the report and write the diagnosis down in
     [thread-sanitizer.md](thread-sanitizer.md).
  2. Print its signature with
     `python3 -I tools/lab/tsan_summary.py --signatures "$report_dir"`.
  3. Copy that signature into `KNOWN_QT_RACES` with a comment that names the diagnosis.

  The list proves nothing beyond that exact match: a report with the same top frames, reached
  another way, is excused too. Never add a report whose stacks hold put.gd code.

The recording tests (`recording_storage_integration`, `recording_pipeline_throughput`)
cannot be certified under ThreadSanitizer: GStreamer is not instrumented, and the one
report that remains reads a `GstMessage` a GStreamer thread released. Tests that need a
desktop service (labwc, an accessibility bus, KDE or Hyprland) or the Release poster helper
fail on a builder without it, with or without ThreadSanitizer; those are environment, not
races. `gui_application`, `gui_settings_general` and `scroll_session_contract` run past
their own 60 s and 180 s timeouts under ThreadSanitizer.

Run the action fuzzer against the instrumented build. Give it a new, short, private `--out`,
and check the socket path first: a seed's session socket is `<out>/s<seed>/r/rt/putgd/session.sock`,
and a Unix socket path has at most 107 bytes.

```sh
d=$(mktemp -d) && out="$d/tsan" &&   # a private parent (mode 0700) and a new child; race_loop.py refuses an existing --out
python3 -c 'import sys; p = sys.argv[1] + "/s99999/r/rt/putgd/session.sock"; n = len(p.encode()); print(n, p); sys.exit(n > 107)' "$out" &&
  ACTION_FUZZ_SLOW=4 python3 -I tools/lab/race_loop.py tsan --build build/tsan-instrumented-qt \
    --tsan-prefix work/sanitizer-diagnosis --out "$out" \
    --first-seed 1 --steps 120 --hours 3 --stress 2
```

`ACTION_FUZZ_SLOW` multiplies every wait of the driver. Each step runs only if the one before
it succeeded: no launch without a private folder and a socket path that fits.

`--stress N` starts N busy loops, in a systemd user scope of their own, to shake the schedules. A seed
that finds a report stops there and keeps `finding.json`, `steps.jsonl` and the reports in
`san/`; a clean seed keeps only its small records. Group the reports of one run with

```sh
report_dir="$out/s17/san"   # a folder of tsan.<pid> files: a seed's san/, or a CTest run's log_path folder
python3 -I tools/lab/tsan_summary.py "$report_dir"
```

It lists reports with a put.gd frame first, then other reports, then the excused known ones.
`--signatures` prints the exact signature of each report that is not excused, ready to add to
the known list once it is diagnosed.

## rr

rr needs real performance counters (a bare-metal builder, not a VM) and a
`kernel.perf_event_paranoid` it accepts. Use a build with debug information (`RelWithDebInfo`),
not the ThreadSanitizer build.

**Performance counters.** Try the host's current value first. rr 5.9 and newer accept 2 on
Linux 6.10 and newer, and rr says which value it needs when it refuses. k72's Ubuntu default
is 4, and rr 5.10 says it needs at most 3 there. Change it only when rr refuses, and only for
the run:

1. Coordinate with whoever else uses the host.
2. Save the current value.
3. Restore it when the run ends, however it ends.
4. Read it back and fail loudly if it differs.

Run the change and rr together in one subshell, so the restore cannot be skipped by leaving an
interactive shell open:

```sh
(
  orig=$(cat /proc/sys/kernel/perf_event_paranoid) && [ -n "$orig" ] || { echo "cannot read perf_event_paranoid: not changing it" >&2; exit 1; }
  restore() {
    sudo -n sysctl -q kernel.perf_event_paranoid="$orig"
    now=$(cat /proc/sys/kernel/perf_event_paranoid) && [ -n "$now" ] ||
      { echo "cannot read perf_event_paranoid back: check it by hand (saved value $orig)" >&2; exit 99; }
    [ "$now" = "$orig" ] || { echo "perf_event_paranoid is $now, not the saved $orig: restore it by hand" >&2; exit 99; }
  }
  trap restore EXIT
  trap 'exit 130' INT
  trap 'exit 143' TERM
  sudo -n sysctl -q kernel.perf_event_paranoid=3 || exit 1   # the value rr named
  python3 -I tools/lab/rr_loop.py --rr ~/opt/rr-5.10.0/bin/rr ...   # or race_loop.py rr ...
)
```

**A pinned rr.** Use rr 5.10.0, not master, built into a private folder, not installed
system-wide. Install its build dependencies by name only, under the lab's package rules (no
broad upgrade).

```sh
git clone --depth 1 --branch 5.10.0 https://github.com/rr-debugger/rr.git ~/opt/src/rr-5.10.0
cmake -S ~/opt/src/rr-5.10.0 -B ~/opt/src/rr-5.10.0/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$HOME/opt/rr-5.10.0" -Ddisable32bit=ON
cmake --build ~/opt/src/rr-5.10.0/build --parallel 2 && cmake --install ~/opt/src/rr-5.10.0/build
```

Pass it as `--rr ~/opt/rr-5.10.0/bin/rr` to `race_loop.py` and `rr_loop.py`. rr 5.9 aborts on
glibc 2.43's `madvise(MADV_GUARD_INSTALL)` (Ubuntu 26.04 and newer). rr 5.7 from Ubuntu 24.04
works with glibc 2.39.

**Filesystems.** rr 5.10 aborts with "Unknown ioctl(0x81009431)" on `FS_IOC_GETFSLABEL`,
which Qt issues for its data folders. That happens on any filesystem that answers it: btrfs,
and ext4 too (seen on k72's ext4 home). Put everything a recorded run writes on tmpfs, as
absolute paths:

- `--out` for `race_loop.py`, and `--run-dir` for `action_fuzz.py`. The fuzzer sets the
  resident's `TMPDIR` to a private `tmp/` inside its run folder, so a tmpfs run folder covers it;
- `--run-dir` and `TMPDIR` for `rr_loop.py`, which passes the caller's environment, `TMPDIR`
  included, to the test it records.

A per-user quota on `/dev/shm` (k72) can run out under rr traces: a seed then fails with
"Disk quota exceeded" and its trace stays incomplete. Use a tmpfs without one, such as `/tmp` on k72.

**Time.** rr is several times slower, so a test's own time budgets can expire:

- `--time-scale` (`PUTGD_TEST_TIME_SCALE`) stretches the waits inside a test.
- `ACTION_FUZZ_SLOW` stretches the fuzzer's waits.
- `rr_loop.py --timeout` is how long one recorded run may take before rr_loop stops it.
  A run stopped that way is reported as a *possible* slowdown or hang, not as a race.

A request timeout in the S3 worker (`curlCode` 28), a helper killed after its grace period, or
a Proton transport timeout under rr is most likely the slowdown. Raise the scale and compare
with an unrecorded run before reporting it.

**Finished traces.** A trace replays only if rr finished writing it. All three tools count a
trace folder as complete only when its `version` is a regular file and there is no
`incomplete` marker of any kind (rr removes that marker when it finishes).

- The fuzzer kills only rr's tracees, lets rr finish and exit, and reports each trace as
  complete or incomplete. It sends SIGTERM to rr only if rr does not exit by itself.
- `rr_loop.py` stops a run that times out with SIGTERM to rr alone, and says whether the
  trace is replayable.
- `race_loop.py` removes a clean seed's complete traces with `rr rm`, never the
  `latest-trace` link, and reports a removal that fails.

**Stopping a loop, and what cleanup guarantees.** Both loops need a systemd user manager
(`systemd-run --user --scope`) and refuse to start without one. Each seed of `race_loop.py`,
its busy loops, and each recorded run of `rr_loop.py` runs in a transient user scope of its
own, with a name unique to that job (`putgd-lab-*.scope`). Every process the job starts stays
in that scope's cgroup, whatever session or parent it ends up with: rr's tracees, the
resident's helpers, an orphan.

- When a seed or run ends, however it ends, the loop stops its scope. Every process still in
  it gets SIGTERM, then SIGKILL after the unit's stop timeout. The loop says how many were
  left.
- Ctrl+C, or SIGTERM to the script alone, first lets the job finish properly:
  - `rr_loop.py` sends SIGTERM to its rr through the pidfd it holds, so rr can finish the
    trace, then stops the scope.
  - `race_loop.py` forwards the signal to the seed's process group, so the fuzzer stops its
    resident and receiver, waits up to 300 s, then stops the seed's scope and the busy loops'
    scope.

  Both exit with 128 + the signal, and the seed or run that was stopped is kept.
- The guarantee needs the loop itself to run its cleanup. If the loop is killed with
  SIGKILL, its scopes keep running. Stop them by hand with
  `systemctl --user stop 'putgd-lab-*.scope'`.

**Replaying.** Let rr start GDB with your commands, so no server is left behind:

```sh
rr replay path/to/trace -- -batch -x commands.gdb
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
