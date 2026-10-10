# GNOME resident memory checks

The resident and the settings/history UI are separate processes. A GNOME memory
result must establish the actual resident route before comparing memory. The
on-demand UI retains distro Qt, native fonts and native folder dialogs.

## Session discovery

Run graphical tests in the existing unlocked GNOME session with its VNC display
visible. Do not infer the client environment solely from `/proc/<gnome-shell>/environ`:
that is the compositor’s initial environment and may lack values published later.
The source56 lab run lacked `WAYLAND_DISPLAY`, selected QApplication and measured
63.01 MiB RSS. It did not measure the proposed Core route.

Read the existing session user manager’s environment in memory. Export only the
selected display value needed by the launcher, never the whole environment to logs.
Require GNOME/Wayland, reject mixed Cinnamon/KDE sessions and conflicting explicit
platform overrides, and validate the actual display endpoint:

- Resolve a relative `WAYLAND_DISPLAY` under the original `XDG_RUNTIME_DIR`.
- Require a nonsymlink Unix socket owned by the desktop user. The original runtime
  directory must be private and owned by that user; do not require mode 0600 on the
  socket itself. A normal compositor socket can have mode 0775 inside that directory.
- Verify the compositor process identity and selected manager/display endpoint
  remain unchanged across discovery and launch. Reject missing or conflicting values;
  do not invent `wayland-0`.
- If a cold test supplies its own runtime directory, preserve the validated original
  display as an absolute socket path before changing `XDG_RUNTIME_DIR`.

The run-specific corrected lab helper wraps both resident launch and independent
native fixture launch. Its pinned script and input manifest remain in the local
source57 run records. Incorporating this tested discovery rule into the repository
lab runner with focused contracts is still a separate follow-up; do not assume a
clean checkout already contains it. Production environment guards remain unchanged. A valid test launch is not
permission to route ambiguous sessions into the native bridge.

## Route and functional prerequisites

Pin a clean source commit, tree, count, version manifest, package hash and actual
installed executable hashes. Use the standard CMake presets, Ninja, CTest and CPack;
reuse a verified existing build cache for incremental source transitions. Retain
all failed attempts. A package build or headless test is not graphical acceptance.

Before measuring, record the actual resident PID/start identity, selected environment
fields and argv, compiled GNOME support and bounded library-map metadata. Source57
Core routing requires an actual GNOME Wayland display and ordinary resident flags.
Graphical or unknown Qt arguments retain the QApplication fallback. Verify that
fallback separately without using it as Core memory evidence.

First run the unchanged Cinnamon regression gates; do not alter its implementation.
Then run current-source GNOME capture/clipboard cases with independent Qt/GTK paste,
tray opening/reopening, native settings fonts and folder dialog, real notification
receipt, live region background/crosshair, History/Activity behavior and normal
helper exits. Preserve any unproven remote link-copy behavior as an explicit pending
gate. Local diagnostic acceptance does not establish full feature parity or release.

## Measurements

The frozen GNOME protocol uses a five-second warmup, a 300-second observation and
five-second intervals, producing 61 samples. Require span at least 299.5 seconds,
maximum gap at most 7.5 seconds, one exact resident family and no live GUI, capture,
fixture or build process. Keep the sampler and evaluator hashes fixed. Record RSS,
PSS, private memory and CPU as a percentage of one core; do not substitute PSS for
the RSS target.

After the local native workload, run two independent observations on the same
verified resident. Both must satisfy the 40 MiB RSS limit and CPU below 0.5% of one core.
Retention also requires a measured whole-family reduction greater than 1 MiB
against the reference, with the launch-provenance difference disclosed. Before keeping
a candidate, also run a fresh 300-second cold observation with an empty local-only
profile, no GUI/source/S3 use, unchanged native bridge/tray and normal shutdown.
The existing sampler’s `--pid` mode does not apply its warmup option: explicitly
record a guarded monotonic five-second quiet warmup before invoking it.

Record cold, post-local-native, populated-history and post-S3 workloads separately.
Comparing a corrected session launch against an older fallback reference must name
that launch difference; a lower number alone cannot establish a strictly matched
code-only effect. Missing `WAYLAND_DISPLAY` is directly established for the
source56 locating run; source55’s stored launch environment is not available.
VM measurements do not become physical-desktop acceptance.
Store structured results, sample coverage, exact identities, original screenshots,
failures and normal-exit/cleanup receipts under the run directory. Export bounded
nonsecret summaries to tracked evidence; never export clipboard payloads, credential
files or raw environment dumps.

## Current status

Clean source57 (`2a02118ffc7bc`, `0.1.1-dev.57+g2a02118ffc7b`) passed all 50 registered
GNOME guest tests and installed through the normal package. Its actual GNOME Core
route and unchanged twelve-case capture/clipboard/cancellation matrix are verified.
The tray, native folder dialog, full production notification banner, Activity parent
and item copies with independent paste, live selection background/crosshair and
normal UI exit also passed. All helpers and fixtures stopped; the resident is idle.
An initial 26.72 MiB RSS snapshot is a locating observation. Two frozen five-minute
post-use runs both passed at 32.77 MiB peak RSS, with worst CPU 0.0334% of one
core and all 61 samples valid in each. The required fresh-start confirmation
is pending; the candidate has not been retained. All guest commands finished
before the approved four-hour experiment limit. That limit has now ended;
the requested extension must be approved before further measurements.
Structured results are in [the source57 evidence](evidence/2026-10-10-gnome-core57-memory.json). Remote History direct-link copy
is explicitly unproven on this guest; the local-native diagnostic is not full parity.
The Cinnamon regression passed with its implementation unchanged. KDE and post-S3
memory remain separate open gates.
