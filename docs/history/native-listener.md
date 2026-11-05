# Native listener experiment

> **Historical record.** This page describes an app version, experiment or plan that no longer exists and is kept for context only. The current design is in [overview](../overview.md); see [memory-strategy](../memory-strategy.md). Do not use it as a work list.

The retained implementation selects a Qt Core resident on Cinnamon X11 and keeps
Qt Widgets settings, history and activity in the transient view process. GNOME
and KDE now have their own native boundaries. Source63 passed the recorded local
functional and memory gates on all three desktops; full remote/release acceptance
remains pending. See [the current strategy](../memory-strategy.md) and its exact
workload limitations. A successful build or fixture is not desktop acceptance.

## Routing and process ownership

`src/platform/NativeSession.h` rejects Wayland and explicit non-XCB platform
selection. The native adapters separately check their X server connections and
extensions. The resident still links QtGui for QImage and key sequences; it does
not initialize QApplication or a QPA plugin on the Core route. Do not claim that
all QtGui libraries have been removed.

`NativeTray` exposes StatusNotifierItem and a fixed production menu over the
session bus. Registration is asynchronous and bounded. Pending registration is
not absence of a tray host. Host loss and return are tested separately from
initial startup; a missing host preserves settings access. Notifications use the
accepted user preference.

On Wayland only the compositor decides whether a window comes forward. Plasma's
and GNOME's tray hosts call `ProvideXdgActivationToken` on the item just before
they activate it or send a menu entry's `clicked` (#657). `NativeTray` keeps that
token for the next click only (10 s at most, printable ASCII up to 512
characters). A view that click opens receives it through the helper's `activate`
frame, and "Capture region & edit" passes it to an open editor's raise. Without
a token, KWin keeps the window behind the focused application and turns its
taskbar entry orange, and GNOME shows "… is ready" instead.

`X11Clipboard` owns the output selection in the resident. It retains one encoded
payload for legitimate native clipboard ownership, sharing an image's existing
QByteArray storage rather than decoding or duplicating that image. Text and URI
representations are encoded once. SelectionClear or replacement releases current
ownership; an earlier accepted incremental transfer retains its independent
version only until completion, requestor destruction or its idle/absolute deadline.
There are at most sixteen streams and seventeen retained versions, each bounded
by the accepted Settings ceiling. Requests send chunks of at most 64 KiB.

This is an explicit memory exception: a local-only image must remain pasteable
while the resident owns the clipboard. Uploads normally own only the small URL
text; history previews and finished upload buffers are separate and must not be
kept alive by this owner. No disk spool is written or flushed on the event loop.
Copy commit remains synchronous, with a final clipboard revision check and server
ownership confirmation; asynchronous staging without an invocation eligibility
contract would allow a stale completion to overwrite newer user activity. Large
text/URI encoding still runs synchronously, and the native owner must be included
in any payload-retention memory measurement. Do not describe this as zero payload
heap retention or assume its tradeoff meets the cold/closed memory target.

Graphical acquisition runs in the ephemeral `putgd-capture` process. Before
launch, the resident freezes the active window, frame and output identities and
scaling inputs. The helper resolves Qt display coordinates from that frozen
identity and uses the existing live transparent region selector. It never
retargets capture to the window that becomes active after launch. Layout changes
fail closed. Resident/UI/helper windows are excluded from window capture.

The parent creates a private staging directory and output inode. Control frames
are bounded; they carry generation, nonce and build identity, not pixels. The
parent validates identity, PNG dimensions, encoded hash and accepted resource
limits before decoding. It acknowledges only a validated image and reports the
source result after normal helper exit. No helper remains hidden while idle.
Whole-family measurements must include capture children, including any escaped
or reparented child, as well as the view process.

## Replay and evidence

Use the normal documented CMake presets and distribution Qt 6.4.2 or later.
Build the official AWS SDK as described in build.md. Existing provisioned SDK
paths may be passed with CMAKE_PREFIX_PATH, PUTGD_AWS_RUNTIME_DIR and
PUTGD_AWS_SOURCE_DIR; do not invent a substitute SDK or toolchain.

Run the new Core/session, tray, clipboard, snapshot, capture protocol and helper
lifecycle tests before desktop rollout, followed by the complete development
suite. The new tests use C++/Qt and Xvfb; no Node fixture is required on guests.
Core XCB tests use an isolated Xvfb server with `-noreset` so that closing the last
native client does not reset its state between test functions.

The capture decoder regression runs with QT_IMAGEIO_MAXALLOC=1 and verifies a
larger explicitly accepted synthetic image without changing the global reader
guard. This does not prove all optional distribution image plugins work under
Qt Core. Vector codecs that require fonts need independent coverage.

Freeze a clean source commit before measuring. Save its complete commit, tree,
count, archive hash, build manifest and hashes of putgd, putgd-cli, putgd-ui,
putgd-capture and the S3 module. Stop an older resident and verify its exit before
starting the replacement. Preserve its synthetic profile and previous evidence.

Run Cinnamon native functional acceptance first, visibly through its VNC
listener. Restore a locked session through its visible masked console using the
protected bootstrap login input. Never put that input in command arguments or
logs. Check all four actions, cancellations, independent clipboard paste, tray
menus/notifications and normal helper exits. Fixtures do not establish native
acceptance.

The existing hotkey budget also remains a gate: thirty warm invocations must
have p95 hotkey-to-selector time <=150 ms. For the physical live-outline replay,
pass the exact same-build putgd-capture path with --capture-helper. The witness
accepts only the resident or its immediate child with the supplied executable
device/inode and PID/start identity. It retains the original synthetic canvases,
input timing and paint witness, and records observer overhead. No hidden helper
may be kept alive between invocations to improve this result. A latency regression
must be measured and fixed before keeping a memory win.

Only after the native gate passes, run two serial physical cold five-minute and two
closed-view measurements with the frozen tests/performance scripts. Follow the
[current whole-workload measurement guidance](../memory-strategy.md#measure-the-complete-workload)
and the [cold-family observer](../../tests/performance/observe_cold_family.py) /
[UI-process observer and evaluator](../../tests/performance/bench_ui_process.py). Preflight
every complete Unix socket pathname with tools/diagnostics/check_socket_paths.py.
Use fresh short private /tmp roots. Save commands, timestamps, parameters, failed
attempts and structured raw samples before reporting them. RSS, PSS and private
memory are different metrics; the target is <=40 MiB RSS for both cold sampled
peak and the settled whole family after views close. The original 100 MiB budget
is not the optimization target. CPU must remain below 0.5% of one core.

Expand to GNOME and KDE only after Cinnamon functionality and memory pass. Their
current-source native evidence remains mandatory; a Cinnamon result cannot
certify those desktops or their memory. Keep the immutable golden disks sealed
and use existing disposable overlays according to vm-lab.md.
