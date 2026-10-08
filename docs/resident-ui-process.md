# Resident service and an on-demand UI

Status: direction approved by the user, 2026-10-08; implementation pending.
The resident must retain a tray icon and menu while all UI windows are closed.
It must still offer capture/upload actions and reopen Settings, History and
Activity. Closing the UI must leave shortcuts, clipboard ownership and operations
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
The S3 SDK is already loaded on demand. The current retained-window experiment
releases native drawing buffers after explicit close while preserving logical
widgets and drafts. It does not unload Qt Widgets or desktop styling libraries.

## Remaining dependencies

| Component | Current dependency | Work needed for a smaller resident |
| --- | --- | --- |
| Settings, History, Activity, About | Direct service references and Qt Widgets | Client interfaces and a separately launched UI executable |
| Tray and menu | `QSystemTrayIcon` and `QMenu`, both in Qt Widgets | Measure retained cost; removing Widgets requires a different tray/menu implementation or helper |
| X11 clipboard and capture | `QGuiApplication`, `QClipboard`, `QScreen` | Keep the active graphical event loop; do not assume a Core-only service is sufficient |
| Region overlays | Widgets on X11 fallback and KDE | Move selection to a temporary helper or retain Widgets; preserve native target identity, cancellation and live background |
| GNOME and KDE integrations | Desktop-specific capture/shortcut/clipboard contracts | Preserve bridge identity and permissions across any helper boundary |
| Settings credentials and destination tests | Direct `CredentialStore` and tester callbacks | Add bounded private IPC operations with no secret logging, command-line secrets or credential export |
| Live view updates | In-process signals | Add bounded event delivery or measured polling only while views are visible |
| Unsaved drafts and history state | Retained widget state | Explicitly preserve safe draft/selection state across UI exits; clear credential entry fields and retain revision-conflict checks |

Moving only the settings windows will not remove Qt Widgets from the current
resident link graph: `putgd_x11`, `putgd_kde` and `TrayController` still require it.
Replacing all desktop integration with a new native framework is not justified
by this analysis.

## Evidence and limits

The candidate's physical Cinnamon cold snapshot was 46,809,088 bytes RSS. Resident
Qt Core/Gui/Widgets mappings contributed 13,025,280 bytes; Widgets alone contributed
3,530,752 bytes. The heap contributed 3,026,944 bytes. These mappings are attribution,
not a forecast of removable memory: Core/Gui remain useful, libraries can be shared,
and the GUI process would have its own startup cost. The subsequent five-minute
cold run ended at 44.9 MiB; the 40 MiB target remains unmet.

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

After the current buffer-release change is integrated, prepare a separate measured
implementation with its own frozen baseline and explicit mutable files. First measure
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
and IPC lifecycle. No memory saving or 40 MiB result is promised here.
