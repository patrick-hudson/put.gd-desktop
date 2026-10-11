# Measuring the resident and UI helper

`tests/performance/bench_ui_process.py` measures the resident **and every observed
child process**, including `putgd-ui`, rather than moving a memory counter into a
second process. It records RSS, PSS and private memory per process and in total.
Summed RSS counts shared resident pages more than once; use total PSS and private
memory alongside RSS. KDE's shared graphics mappings make this distinction
particularly important.

The script uses Python's standard library and Linux `/proc`. The optional physical
Cinnamon window workload also uses the already established `xprop`, `xwininfo`
and `wmctrl` tools. It keeps normal Qt fonts and dialogs. It introduces no build
system, toolkit substitute, allocator hook or dependency installation. Importing
it or asking for `--help` does not contact the display or launch the app.

## Before comparing builds

1. Freeze a clean source archive and full commit for each build. Use the standard
   CMake/Ninja Release build and distro Qt, with the same SDK configuration.
   Preserve archive, package, installed binary and build-manifest checksums.
2. Complete the current-source functional wave on Cinnamon/X11, GNOME/Wayland and
   KDE/Wayland. Build and test independent VMs in parallel, keeping every graphical
   interaction visible through its localhost VNC listener and the display wall.
   Reuse needed guests and shut down superseded guests before starting replacements.
   Verify the old app's PID/start identity and terminal operations, preserve its
   synthetic profile, then stop it while the replacement builds. A needed build VM
   may stay running; an old app version must not remain live on the dashboard.
   The harness accepts `--acceptance-receipt` repeatedly to fingerprint those
   receipts; it does not infer their coverage or success from their existence.
3. Check the tray remains visible after closing all windows, all four actions,
   hotkeys, clipboard survival, draft/conflict retention, native dialogs/fonts,
   precise deletion, helper failure/restart and accepted work completing after
   UI exit. Keep these operation IDs and graphical witnesses in the functional
   receipt. Empty-view memory results do not establish those gates.
4. Wait for builds and active graphical tests to finish before quiet sampling.
   Run the two physical Cinnamon measurements **serially**. GNOME and KDE can run
   their independent functional wave in parallel; each VM's memory observation
   must be quiet and labelled as VM evidence. Preserve failed attempts separately.

The harness requires `--source-revision` to match the compiled version's short
hash. When a CLI endpoint is supplied, it also requires the resident's full
`build.gitCommit` and version to match. All supplied executables must report the
same version. It hashes each executable and validates the live resident PID,
start time, UID, executable path, device and inode; every measured process has a
live executable checksum. Dirty builds require `--allow-dirty` and are diagnostic
only. An equal version string alone is not sufficient provenance.

## Matched empty-view comparison on physical Cinnamon

First create or reuse a successful private >=300-second empty fixture with the
[existing physical idle procedure](testing-memory.md). The new harness copies
only its validated local-only configuration into new private XDG directories. It
copies no journal or payloads. All four actions are local, profiles and shortcuts
are empty, and notifications/login startup are disabled. It does not acquire the
clipboard, capture screenshots, or contact S3.

Baseline `bcbe63f0d1e1ad00f36ac110fb596d0245869617` accepts `ui.show` for Settings,
History and Activity only. Compare the same three views on both builds:

```sh
python3 tests/performance/bench_ui_process.py \
  --mode retained --app /absolute/baseline/putgd \
  --cli /absolute/baseline/putgd-cli \
  --source-revision bcbe63f0d1e1ad00f36ac110fb596d0245869617 \
  --source-archive /absolute/baseline-source.tar \
  --fixture /tmp/putgd-idle-fixture \
  --output /tmp/putgd-retained-run-1 \
  --views settings history activity --cycles 3 \
  --warmup-seconds 5 --visible-seconds 2 --quiet-seconds 30 --interval 5 \
  --acceptance-receipt /absolute/baseline-native-receipt.json

python3 tests/performance/bench_ui_process.py \
  --mode split --app /absolute/candidate/putgd \
  --cli /absolute/candidate/putgd-cli --helper /absolute/candidate/putgd-ui \
  --source-revision FULL_CANDIDATE_COMMIT \
  --source-archive /absolute/candidate-source.tar \
  --fixture /tmp/putgd-idle-fixture \
  --output /tmp/putgd-split-run-1 \
  --views settings history activity --cycles 3 \
  --warmup-seconds 5 --visible-seconds 2 --quiet-seconds 30 --interval 5 \
  --acceptance-receipt /absolute/candidate-native-receipt.json
```

Repeat with new `run-2` output paths after the preceding run finishes. Keep the
same fixture, arguments, observer and native session. Compare the median of the
two `summary.finalClosedTotals` values for each metric; compare
`summary.peakOpenTotals` separately so the price of a visible helper is clear.
Do not compare a candidate's four-view workload against the baseline's three-view
workload without labelling that difference.

For additional candidate acceptance, omit `--views`: split mode defaults to
Settings, History, Activity **and About**, with three cycles. Retained mode
defaults to the three protocol-supported views and rejects About. About in the
baseline remains a separate visible tray interaction; the harness does not modify
or add a test entry point to the historical binary.

Each activation records CLI acknowledgement latency and time until the expected
number of mapped, PID-owned EWMH windows exists. The split acknowledgement means
that the helper reported the window shown. The baseline acknowledgement only
means its activation callback returned, so `acknowledgementIsShown` is false and
mapped-window readiness supplies the independent condition. Neither is a proof
that every pixel has painted; keep graphical acceptance witnesses. The configurable
visible interval starts **after** that readiness condition. It is not a fixed sleep
substituted for readiness.

The script requests normal window-manager Close on owned windows, waits for them
to disappear, and in split mode requires both the resident's reported UI PID to
clear and the observed helper process to exit. It then samples the entire family
through the quiet interval and repeats. No signal terminates the UI as a substitute
for closing it. The final process-group cleanup happens only after results have
been recorded and only targets the isolated resident launched by this script.

`ui.snapshot.diagnostics.trayAvailable` must remain true, and the same resident
identity must stay alive. Availability is a desktop capability, not a rendered
icon witness. The summary explicitly leaves `renderedTrayAcceptanceEstablished`
false; visible tray/menu acceptance belongs to the functional receipt.

## Existing-process observation in a VM

For GNOME/KDE, use the established visible QMP/desktop tests to open and close
windows, then run the sampler over SSH. This mode never launches the app, changes
configuration, touches the clipboard, opens/closes a window or sends a termination
signal. It needs no X11 tools and can be copied to a guest as a single Python file.

Obtain the resident PID and start ticks from its current install/launch receipt.
Start ticks are Linux `/proc/PID/stat` field 22; use the script's `identity(pid)`
function when checking an existing process, because the process name can contain
spaces or parentheses. Never reuse a PID from an older VM or build receipt.

```sh
python3 tests/performance/bench_ui_process.py \
  --mode split --pid RESIDENT_PID --start-ticks EXPECTED_START_TICKS \
  --app /usr/bin/putgd --helper /usr/bin/putgd-ui \
  --cli /usr/bin/putgd-cli --socket /absolute/current-session.sock \
  --source-revision FULL_CANDIDATE_COMMIT \
  --output /tmp/putgd-kde-after-close-unique \
  --label after-native-actions-and-close --seconds 120 --interval 5 \
  --acceptance-receipt /absolute/current-native-receipt.json
```

With `--pid`, the CLI is optional; supplying it requires an explicit `--socket`.
The sampler checks Unix `SO_PEERCRED` against the requested resident PID and UID
before calling diagnostics, preventing a different default resident from supplying
its identity. Without a CLI, provenance is limited to executable versions/hashes
and the supplied full commit prefix; the recorded build object is absent. Prefer
the CLI check. `--seconds 0` takes one snapshot; longer runs record timed samples.

Existing-process mode includes every observed descendant whether open or closed.
It does **not** assert that the caller has completed the workload or closed the UI.
Use its `uiProcessCount` and the functional close receipt to establish that state.
An independently launched UI is not supported by production and must not be used
to evade the process-family accounting.

## Evidence format and limitations

A new private output directory contains:

- `progress.json`: parameters, exact command, environment/package versions,
  source/build identity, checksums, lifecycle events and the latest compact sample
  total. The complete sample stream remains in `samples.jsonl`.
- `samples.jsonl`: each process's PID/start time, role, executable/hash, RSS/PSS/
  private bytes, CPU ticks and thread count, plus family totals and measurement
  duration. No environment dump, process command line, window title or payload is
  included. Private hugetlb and swap bytes are separate attribution fields;
  `privateBytes` is the conventional `Private_Clean + Private_Dirty` value.
- `results.json`: completion or failure, the same metadata, exited-helper identities
  and the summary. Failed runs retain their evidence and must get a new output
  directory on retry.
- For a launched physical workload, the private configuration/data and resident log
  remain beside the results. They contain synthetic local-only state; credentials
  are never an input to this benchmark.

The sampler enumerates children of every resident thread and recurses into them.
It retains identities of previously observed children until they exit, even if
reparented, and continues discovering workers they create. These processes remain
in the totals while `inResidentTree` separately records current membership under
the resident. Each tracked process is identity-checked before traversing its
children and again after enumeration, so a reused PID cannot supply unrelated
workers. The physical workload rejects an escaped descendant. PID reuse cannot silently become an improvement: start time
and executable checks distinguish each process generation. Families are bounded
to 64 live members and 256 recorded exits; a run is limited to 4096 samples. Transient exits cause up to three
snapshot retries; a continuously changing family fails instead of producing an
apparently quiet sample.

The process snapshots are sequential and add observation work. Processes born and
exited wholly between samples may be missed. Desktop services outside the family
are not separately charged, and PSS depends on which other processes share pages
at that instant. A short-lived peak between samples is not captured. Keep these
limits and the same sample interval when comparing. CPU ticks are recorded for
attribution, but this harness makes no idle CPU claim.

`finalTotalRssAtOrBelow40MiB` is only a numerical observation. Neither an empty-view
run nor a VM observation establishes the cold 40 MiB target or the legacy five-minute
idle budget; those fields remain false. Populated history, capture, clipboard,
slow upload continuing after UI exit and repeated image cleanup need their own
workload receipts and total-process observations.

The older `bench_idle.py`, `bench_views.py` and `bench_payload_lab.py` intentionally
remain unchanged so historical evidence can be replayed. They account primarily
for the resident PID and cannot alone substantiate a process-split saving. In
particular, the old view script searches for resident-owned windows; split windows
belong to the helper. Use this harness for the new lifecycle and pair appropriate
existing functional workloads with its existing-process sampler.

## Harness tests

```sh
python3 -m unittest discover -s tests/performance -p test_bench_ui_process.py -v
python3 tests/performance/bench_ui_process.py --help
```

The unit suite launches only synthetic Python parents/children and verifies total
accounting, normal child exit/recreation, PID/executable mismatch rejection,
retention of a previously seen escaped child and its later workers, PID reuse
before/during child discovery, owner-only window selection and strict helper
diagnostics. These tests can run on Linux GitHub Actions using the
normal Python runtime; preserve stdout and the exit status. They establish sampler
contracts, not native desktop acceptance or a measured application saving.

## Replaying the source-41 observation

The [current-source record](evidence/2026-10-08-ui-process-native-memory.json)
links native receipts, exact executable/package identities, controlled commands,
quiet guards and physical results. Its allowlists exclude private XDG state,
resident logs, credentials and payloads. The tested software remains
`0.1.1-dev.41+g97ca3a521714` when a later documentation commit saves that evidence;
do not relabel the measurements as the later build.

Use the portable production harness above for a fresh run. Historical driver
scripts capture this machine's absolute paths, fixture and existing guest names.
To replay them, restore their recorded `work/u11/` layout or adapt those paths,
rebuild the exact recorded commits through normal CMake presets, and supply fresh
verified resident identities and native receipts. Generate a new source archive
from the requested full commit, check its checksum, and retain the original
archive. Use new output labels and directories; never overwrite a successful or
failed attempt. Native acceptance and lock restoration still require the visible
VNC console. The GNOME corrected replay driver preserves its original failing
launcher alongside the valid timed samples.

Physical cold sampling is a separate >=300-second workload: five-second warmup,
one-second family/mapped-window checks, recorded samples every five seconds, no
views/actions/clipboard/upload, empty local-only fixture, native Cinnamon fonts
and dialogs preserved. It records CPU ticks over the actual measured interval.
The matched three-view runs follow serial retained/split/retained/split order;
quiet VM observations finish before those activations. Record host memory/load
before and after each run. Separate initial Settings activation, later Settings
reopen, additional views and final-close latency; mapped readiness does not prove
completed painting. A small number of activations does not establish a p95 budget.
