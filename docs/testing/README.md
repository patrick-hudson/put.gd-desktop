# Testing and acceptance

What must be tested, how, and what has been recorded.

## Guarantees

- Required Linux coverage is Mint, Ubuntu, Kubuntu, Fedora and Debian, with Cinnamon plus GNOME and KDE and real X11 and Wayland sessions. The [acceptance matrix](acceptance-matrix.md) sets coverage; it does not establish that any row has passed.
- Offscreen tests prove widget and core behavior only. Never report a mock as desktop proof.
- Every declared desktop requires current-version evidence on the exact build before acceptance. A failure on any desktop blocks acceptance until it is fixed or shown to predate the build.
- Never substitute VM or XWayland evidence for a gate that requires physical or native hardware.
- Use synthetic payloads and an isolated test destination. Never put credentials in source, configuration, logs, diagnostics, history, command lines or review packets.
- Age alone does not justify deleting evidence. Evidence records live in [put.gd-evidence](https://github.com/patrick-hudson/put.gd-evidence); never add files under `docs/evidence`.

## Documents

- [Testing](testing.md): the repository's test suites and what each proves.
- [Implementation evidence](verification.md): observed acceptance and blocked gates.
- [Removed tests](removed-tests.md): what the November 2026 cuts removed, what no longer gets caught and how to bring a test back.
- [Acceptance matrix](acceptance-matrix.md): required coverage and execution placement.
- [Evidence retention](evidence-retention.md): what to keep, where records go and how to publish them.
- [ThreadSanitizer dependency build](thread-sanitizer.md): the instrumented Qt used for thread checks.
- [Surface driver prerequisites](surface-driver-prerequisites.md): target tools and cleanup for the surface driver.
- [Live S3 acceptance](live-s3.md): acceptance against an isolated synthetic S3 destination.
- [Hotkey dispatch](shortcut-dispatch.md): testing that a hotkey's capture is accepted, not only registered.
- [Tray progress](tray-progress.md): the replay path for tray progress on the disposable desktop lab.
- [X11 hotkey refresh](x11-shortcut-refresh.md): the X11 keyboard-mapping regression and its checks.
