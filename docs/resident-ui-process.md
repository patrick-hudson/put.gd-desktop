# Resident service and an on-demand UI

Status: retain the split and native listener boundaries. Source63 meets the 40 MiB
resident target in the recorded local workflows on Cinnamon, GNOME and KDE;
current real S3/remote History and full release acceptance remain pending. Read
[the current strategy and workload limits](memory-strategy.md). Measurements below
from source41 and earlier are historical, not the current baseline. See
[protocol and lifecycle](ui-process-protocol.md).
The resident must retain a tray icon and menu while all UI windows are closed.
It must still offer capture/upload actions and reopen Settings and History
(the former Activity view is now History). Closing the UI must leave shortcuts, clipboard ownership and operations
running; the explicit Quit command exits the application.

The user also raised replacing Qt with a visual framework built from scratch.
The current recommendation is to measure process separation first. A full
framework rewrite would add text/input/accessibility/scaling and desktop
integration responsibilities while its actual memory benefit remains unknown.
Custom visual design does not require a custom toolkit. The attribution below
must not be presented as the amount a replacement would save.

The user proposed keeping only clipboard and shortcut handling resident while
Settings and History sleep. An on-demand UI process is a plausible way to release
its complete address space when its last window closes. Pausing the current
process would retain memory and stop the event loop needed by clipboard, shortcut
and operation callbacks.

## What the current code already supports

`putgd-cli` is a client of the resident's private local socket. Uploads run in
`OperationService`; the CLI does not contain a second upload engine. Keep that
ownership: the UI and CLI should use the same service, and closing the UI must
leave accepted uploads, local saves, clipboard ownership and recovery intact.

`LocalServer` already exposes settings validation/patching, actions, operation
status/cancellation/retry, history queries, prepared deletion and diagnostics.
The S3 SDK is already loaded on demand. The earlier retained-window experiment
releases native drawing buffers after explicit close while preserving logical
widgets and drafts. It does not unload Qt Widgets or desktop styling libraries.

## Dependency graph before the native listener work

| Component | Current dependency | Work needed for a smaller resident |
| --- | --- | --- |
| Settings, History, Activity, About | Async session client in `putgd-ui` | Validate native lifecycle and measure the split |
| Tray and menu | `QSystemTrayIcon` and `QMenu`, both in Qt Widgets | Measure retained cost; removing Widgets requires a different tray/menu implementation or helper |
| X11 clipboard and capture | `QGuiApplication`, `QClipboard`, `QScreen` | Keep the active graphical event loop; do not assume a Core-only service is sufficient |
| Region overlays | Widgets on X11 fallback and KDE | Move selection to a temporary helper or retain Widgets; preserve native target identity, cancellation and live background |
| GNOME and KDE integrations | Desktop-specific capture/shortcut/clipboard contracts | Preserve bridge identity and permissions across any helper boundary |
| Settings credentials and destination tests | Resident `SetupService`, async session endpoints | Verify native store/prompt lifecycle; never export inputs as drafts |
| Live view updates | Coalesced private invalidations while a helper exists | Verify runtime updates and failure recovery |
| Unsaved drafts and history state | Bounded classified RAM checkpoint in the resident | Validate close/reopen conflicts and exclude credential inputs |

Moving only settings did not remove Qt Widgets from the source41
resident link graph: `putgd_x11`, `putgd_kde` and `TrayController` still require it.
Source63 instead retains Qt for transient graphical work and uses the existing
native tray and desktop-specific listeners. This historical graph is not the
current resident dependency inventory.

## Evidence and limits

The source-41 split (`97ca3a521714`) passes the current native functional subsets
on all three desktops. After identical empty-view workloads, two runs per build,
the median closed whole-family RSS falls from 65.09 to 45.04 MiB, PSS from 40.40
to 25.40 MiB and private memory from 37.59 to 22.89 MiB. The helper exits; the
tray, clipboard and local operations remain resident. Retain the split for that
measured benefit.

There is an open-state cost: maximum sampled RSS/PSS is 120.84/55.01 MiB with
the split versus 73.19/44.49 MiB retained. Summed RSS counts shared library pages
twice; PSS still shows an additional cost. Four repeated Settings reopens per
build have mapped-readiness medians of 332.68 versus 100.92 ms. These times include
observer overhead and do not certify completed painting or p95 latency. The
separate five-minute cold run peaks at 44.390625 MiB, so 40 MiB remains unmet.
Native Cinnamon fonts/dialogs remain enabled. See the [measured decision and raw
evidence](evidence/2026-10-08-ui-process-native-memory.json).

A subsequent four-run standard Qt cost probe kept the native GTK/font integration
and measured only 0.63 MiB settled RSS difference between full and native styling,
with 0.19 MiB immediate style allocation. It used a temporary tray and five-second
stages, so it establishes no product saving. Keep the custom styling. Next locate
KDE's first driver/theme load and repeated-capture lifetimes against each desktop's
own fresh baseline; integer-width changes and blind SQLite cache purges have no
support from this attribution.

The older attribution below predates the split; it is historical context, not
a prediction of savings from the current build.

The candidate's physical Cinnamon cold snapshot was 46,809,088 bytes RSS. Resident
Qt Core/Gui/Widgets mappings contributed 13,025,280 bytes; Widgets alone contributed
3,530,752 bytes. The heap contributed 3,026,944 bytes. These mappings are attribution,
not a forecast of removable memory: Core/Gui remain useful, libraries can be shared,
and the GUI process would have its own startup cost. The subsequent five-minute
cold run ended at 44.9 MiB; that candidate did not meet the 40 MiB target.

Local source evidence: `src/main.cpp`, `src/cli/main.cpp`,
`src/app/LocalServer.cpp`, `src/ui/TrayController.cpp`, the three view classes,
the desktop adapters and `CMakeLists.txt`. Raw attribution is retained at
`work/memory-surfaces/cold-initial-attribution.json`; it belongs to candidate
001b, not a future process-split build.

Qt documents the distinction between [QApplication and QGuiApplication](https://doc.qt.io/qt-6/qapplication.html),
the [X11 clipboard's event-loop requirement](https://doc.qt.io/qt-6/qclipboard.html#notes-for-x11-users),
and the [Widgets dependency of QSystemTrayIcon](https://doc.qt.io/qt-6/qsystemtrayicon.html).
Our supported minimum remains Qt 6.4.2; new code must use APIs available there.

## Reproducible experiment boundary

For future changes, prepare a separate measured implementation against a fresh
matched baseline with explicit mutable files. The source63 result is already
retained for its recorded local workload; it is not a forecast for another change. First measure
minimal service/tray variants with the existing system compiler, CMake, Ninja and
distro Qt. A tiny probe locates costs; it does not prove product savings.

Compare total live process RSS, PSS and private memory, both with the UI closed
and open. Record cold startup, three open/close cycles, populated history, capture,
clipboard upload, an upload continuing after UI exit, and UI reopen latency.
Use two serial physical Cinnamon samples and parallel native GNOME/KDE acceptance
on the same source archive. Preserve VNC visibility, source/commit/binary hashes,
exact commands, structured samples and failed attempts.

Correctness gates include native Cinnamon fonts/dialogs, all four actions,
shortcut rebinding, single-resident ownership, clipboard survival after UI exit,
unsaved-draft conflict handling, exact remote deletion acknowledgement, helper
failure/restart, shutdown ordering, version mismatch and current CLI/MCP parity.
Retain a split only if the measured memory benefit warrants the added process
and IPC lifecycle. Additional savings and unmeasured workloads remain unknown.
