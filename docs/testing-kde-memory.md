# KDE resident memory checks

KDE follows the retained Cinnamon and GNOME checkpoints. Its baseline must use
the same frozen product revision as those checkpoints; older VM binaries and
dashboard peaks are historical evidence, not the new baseline. The initial
revision is source57, `2a02118ffc7bcf3f4643b83f83d5dd7594a8207f`, tree
`c0e6b8d924bd2fafb6bd4a8d68a679799b214031`, version
`0.1.1-dev.57+g2a02118ffc7b`. The five-minute post-local-native baseline measured
139.42 MiB peak/final RSS, 33.96 MiB PSS and 21.15 MiB private memory, with
0.0667% CPU of one core. All 61 samples passed identity and coverage checks;
RSS was constant. The RSS target is not met. Current cold memory was not measured.
See the [bounded evidence record](evidence/2026-10-10-kde-source57-memory-baseline.json).

## Build and session identity

Use an owned disposable Linux overlay through the lab workflow. Never boot or
modify its backing golden disk. Stop completed, unused lab sessions normally
and prove their exact QEMU generations absent before starting another. Preserve
profiles, packages, build caches and original evidence. Keep KDE VNC visible on
the display wall at http://127.0.0.1:8765/ during every graphical interaction.
Restore locked sessions through the visible masked login field using the private
login input recorded in bootstrap metadata. Never use `credentials.txt` for login.

Before each isolated launch, check every resolved Unix socket path with
`python3 tools/diagnostics/check_socket_paths.py --socket`: application socket,
harness runtime plus `putgd/session.sock`, Wayland, session bus and QMP. The
maximum is 107 UTF-8 bytes. Use fresh short private output directories, and
preserve rejected attempts and corrected commands. If QEMU recreates its owned
control socket with nonprivate permissions, verify exact VM/overlay and socket
owner, type and inode before restoring mode 0600. Preserve the rejection; never
relax the control-socket ownership guard.

Build against KDE's native distro Qt with standard development packages, CMake
presets, Ninja and the pinned official AWS C++ SDK. Reuse a verified existing
source/build cache only after recording its clean Git revision, tree, source
path and SDK provenance. Apply the exact source bundle without editing files.
Run `cmake --preset dev`, `cmake --build --preset dev`, and `ctest --preset dev`;
record actual registration and completed JUnit, then build/package with the
normal release preset. Install through the normal package manager. Record all
installed binary/module hashes, package hash and build version, and prove that
the live PID/start/executable hash matches the package. Installed files alone
do not identify an already running resident.

Compare the exact test names for that revision and available standard test
dependencies, rather than requiring another distro's test count. Source57 KDE
and the host register the same 49 tests; GNOME additionally registers
`gnome_ui_ownership_contract` when GJS is installed. All registered tests must
complete successfully. Record an unavailable optional test explicitly. A count
guard failure after a successful build requires verification of the original
registration, JUnit and package, not an unnecessary rebuild or extra package
installation to match a number.

Run adapters may publish immutable receipts at fixed names. Call a method that
publishes `inventory.json` only once per attempt; additional snapshots use the
identical read-only guest probe and distinct receipt names. If publication fails
after an effect such as profile preservation, retain the original command stdout,
exit status and effect receipt. Start a new continuation attempt, revalidate that
actual state and the untouched build inputs, then perform only the remaining
steps. Never repeat a completed profile rename or overwrite an earlier receipt.

Some inherited lab indexes describe only the resident, CLI and view helper.
The full installation inventory additionally includes the capture helper,
desktop fixture and S3 module provenance. A strict focused check must compare
the full selected inventory. If a legacy index has the same source and shared
hashes but an incomplete schema, preserve its original bytes, hash and file
metadata; collect a fresh full inventory and require equality with the original
installation receipt before enriching the derived index. Keep the original
rejection. Do not weaken the equality check or reinstall an unchanged package.

KDE currently uses QApplication. Do not change it to QCoreApplication before
replacing and verifying its real GUI dependencies: output layout/focus checks,
QWidget region surfaces, per-window LayerShellQt roles and native Wayland
unmap fencing. Static inspection of DBus transports is not feature parity.

## Functional workload before sampling

Run the unchanged nine Qt scenarios and three GTK clipboard scenarios: region,
window, desktop, clipboard text/image/files, plus Escape, right-click and
zero-area region cancellation. Use synthetic local-only data and independent
paste clients. Inspect exact saved bytes and original QMP image witnesses.
Preserve failures and explain any retry. No guest Node fixtures.

Before focused paste checks, verify that the registered fixture output directory
is mode 0700 and its marker is not writable by other users. A fixture launched
with the desktop's default umask may fail this check. Preserve that rejection;
verify the exact fixture PID/start, binary hash, directory device/inode and owned
regular marker, then narrow permissions through descriptors opened without
following symlinks. Do not read or alter payloads, relaunch the fixture, or weaken
the original paste guard. Visibly focus the fixture before its reset shortcut;
a taskbar click alone is not focus evidence.

The Activity UI exposes a parent operation UUID tooltip and child filename/path
tooltips. Join an actual selected child to production item metadata; do not
claim that an unexposed child UUID was visible. A single-item parent's result
and its child's result contain the same text. That check proves the selected
local workflow only; distinguish a child from its multi-item parent before
claiming full subset-copy acceptance.

Verify current native tray/menu, production notifications, fonts/folder dialog,
live transparent selection and themed crosshair, external window identity,
Activity parent/item copies after resetting the clipboard, and normal Settings/
History/Activity helper exits. Remote History direct-link copy and cloud deletion
remain separate gates until actually tested on this version. No offscreen or
mock result establishes compositor acceptance.

Record every extra operation in the measured workload. During this baseline a
tray-coordinate mistake invoked clipboard capture instead of opening Settings.
Its synthetic local-only operation is preserved and excluded from the twelve
matrix cases; it remains part of the populated profile used for memory sampling.
The font observer correctly rejected the absent Settings helper before the
corrected selection. Font evidence establishes the mapped Plasma theme/font
libraries and absence of an application font-family override; it does not
introspect the resolved font family.

QMP screendumps omit the pointer. Pair original live-background before/after
frames with a separate bounded, shared VNC cursor observation that sends no
keyboard, mouse, clipboard or resize input. Join cursor metadata to the same
pending selection operation and retain the idle-versus-selection cursor assets.
Elapsed time alone does not prove that the background continued updating.

The source57 KDE baseline exposed a separate VNC cursor defect. Its actual
QEMU alpha-cursor buffer contains a full cross: 432 pixels have nonzero alpha,
but only 23 are fully opaque. Applying the installed QEMU's `alpha == 255`
mask rule exactly reproduces the incomplete legacy RichCursor mask. Preserve
that rejected mask observation; do not replace it with an inferred image or
claim the display wall rendered the full cross. The separate bounded, shared,
no-input alpha observation proves the native glyph and joins its RGB hash to
the original cursor. [QEMU's upstream fix](https://fuchsia.googlesource.com/third_party/qemu/+/b4189dbf805c297fdd6dd228ffab673d44560bcd%5E%21/)
documents this alpha loss. No host QEMU upgrade or product cursor change was
performed. Both cursor observers and the fresh selector's cancellation remain
part of the original evidence.

For the production notification check, temporarily enable only the notifications
preference through its separately reviewed admission. Keep the base input
admission immutable. Invoke one ordinary synthetic clipboard operation, capture
the actual desktop banner, then restore the original configuration exactly.
Record the enable/restore revisions, document hashes and unchanged resident;
require the final settled configuration before sampling.

Source57 may reuse its unchanged Cinnamon checkpoint for baseline preparation.
Each new product candidate must pass Cinnamon functional and memory gates first;
expand only after passing. Preserve retained Cinnamon and GNOME implementations.
Shared startup changes also need current-candidate GNOME regression evidence.

## Frozen observation

After builds, input, synthetic fixtures and graphical helpers finish, record a
guarded five-second quiet warmup followed by 300 seconds at five-second intervals.
Require 61 samples, at least 299.5 seconds of coverage, positive gaps no greater
than 7.5 seconds, and the same exact resident throughout. Sum the resident family;
reject live or escaped UI/helpers. Record peak/final RSS, PSS, private memory and
CPU as a percentage of one core. The requested threshold is RSS at most 40 MiB;
PSS and shared-library accounting explain RSS rather than replacing that target.
Idle CPU must remain below 0.5% of one core.

Keep `tests/performance/bench_ui_process.py` frozen. The KDE evaluator differs
from the GNOME evaluator only in its guest label. Its numeric checks do not
replace independently joined source/build/native receipts or the external
RSS/CPU decision gates. Save result JSON, all JSONL samples, grouped attribution,
exact process identity, quiet metadata, parameters, command/exit status and
original hashes. Evaluate original counters, not dashboard rounding.

Collect bounded grouped smaps attribution outside scored sampling windows at
fresh empty-profile cold idle and post-local-native settled idle. Record library
families, anonymous/heap/shared totals and bounded shared-memory counts without
exporting arbitrary mapped paths, configuration, environment or payloads. These
snapshots can distinguish the cold GUI/renderer floor from retained resources
after use; do not assume that every mapped byte is removable.

Before keeping a candidate, confirm two independent post-local-native five-minute
observations and a fresh empty-profile cold observation, all at or below 40 MiB.
Require a measured saving over 1 MiB with preserved functional behavior. Cold
means a fresh resident with empty application data, not an empty OS page cache
or an observed transient startup peak. Quit normally afterward and verify exact
process/socket absence and unchanged preserved profile. Record populated-history
and post-S3 workloads separately. VM evidence is not physical KDE certification.

## Checkpoint status

The KDE current-source post-local-native baseline is measured and independently
checked. All 49 registered tests and twelve native local scenarios passed, with
focused checks qualified above. RSS remains 139.42 MiB against the 40 MiB target.
No KDE memory candidate has been implemented or accepted. Cinnamon and GNOME
checkpoints remain intact;
KDE cannot inherit their native or memory acceptance. The canonical lab session
discovery follow-up and full cloud, geometry, visual, MCP and release gates
remain open. The local optimization state is under
`.context/compound-engineering/ce-optimize/kde-native-boundary/`; raw local
receipts are under `work/kde-native-boundary/`. These ignored directories do not
travel with a branch, so export bounded evidence to tracked documentation before
reporting a durable checkpoint. Include the exact input/focused contracts,
frozen observer/sampler/evaluator identities and materialized transfer/measurement
commands and hashes. Preparation templates with unresolved evidence bindings
are not runnable acceptance evidence.

The current run used the materialized transfer recipe at
`work/kde-native-boundary/memory-transfer-materialized-a1/prepare-memory-transfer57-kde.py`,
then the measurement recipe at
`work/kde-native-boundary/memory-measurement-materialized-a1/run-memory57-kde-a1.py`
with `--root-start-authorized`. Input was `/tmp/pg57km1`, output `/tmp/pg57kcm1`.
These paths already exist; never rerun against them. A new observation needs new
short private directories and reviewed literal bindings to its actual source,
resident, native packet and transfer receipt. Preserve the observer and sampler.
The evidence record pins both recipes, their completed transfer, all output files,
the independent numeric review and the original command parameters. The result
directory is `work/kde-native-boundary/kde-memory-run-a1/`.

The official optimization measurement runner also evaluated the retrieved data
with a 30-second timeout, working directory equal to the clean source57 checkout,
and `PUTGD_KDE_MEMORY_RUN` pointing to that result directory. Its command was:

```sh
python3 -B /home/patrick/work/put.gd/work/kde-native-boundary/evaluate.py \
  "${PUTGD_KDE_MEMORY_RUN:?Set result directory}"
```

This evaluates preserved observations without launching the app. The evaluator
passed its integrity, local-native, normal-exit and CPU gates; the separate RSS
comparison failed. One stable baseline run was sufficient. No second observation
or optimization experiment was started. The proposed experiment scope is two
candidates/four hours after approval, with no new dependencies or cloud machines.
