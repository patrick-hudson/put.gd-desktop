# KDE resident and region selector

Status: candidate implementation. Desktop and memory acceptance are pending; host
contract tests do not establish compositor support.

On an unambiguous native KDE Wayland session, the resident uses `QCoreApplication`.
It retains the existing native tray, KGlobalAccel, private Wayland data-control
clipboard connection and KWin ScreenShot2 connection. Settings, History and Activity
continue to run in the existing transient UI process with native fonts and dialogs.
Cinnamon and GNOME adapter implementations remain unchanged.

The startup guard requires a KDE Wayland environment and a Wayland socket name.
Ambiguous desktop names, explicit non-Wayland Qt platform requests, fallback platform
lists and graphical command-line options take the existing graphical startup path.
`native_session_guards` checks this routing contract. Environment recognition alone
is not proof of compositor availability: native transports must also connect and
advertise their required protocols.

## Output ownership

`KdeOutputs` owns an independent nonblocking Wayland connection. It combines current
`wl_output` mode, transform and scale with named `xdg-output` logical geometry. It
waits for complete protocol batches, bounds output count and geometry, and rejects
missing or malformed metadata. Fractional logical dimensions remain compositor
values; they are not reconstructed from the integer output scale.

An actual output-property change invalidates the resident's layout revision before
completion, including a change away and back. Invocation, selection, transfer and
composition check the frozen revision. Output removal, connection loss or incomplete
metadata fail closed. Version 3 supplies atomic completion. After a real change,
version 2 conservatively requires fresh completions on both channels; a compositor
that omits one remains unavailable rather than exposing guessed density. `kde_ownership_integration` includes a small native Wayland
server fixture for protocol batching and invalidation. This fixture is a contract
test, not KDE desktop acceptance.

## Region helper

`putgd-kde-region` creates the existing transparent LayerShellQt selector, keeping
the live background, native crosshair and compositor unmap fence. The resident sends
only bounded output geometry through private process pipes; the helper returns only
a rectangle. It does not capture pixels, persist files, upload, or handle credentials.

The private protocol checks exact build identity, nonce, generation, message order
and geometry bounds. Selection is accepted only after the helper's fenced unmap and
normal zero exit. KWin pixel capture remains in the resident, preserving the
restricted desktop-entry authorization against its installed executable. Startup,
selection and shutdown deadlines bound failure; repeated terminal frames cannot
extend shutdown. Normal cancellation fences teardown on the same Qt Wayland
connection before its terminal reply. A helper that exits after beginning selection
without an authenticated cleanup certificate blocks further screen captures until
the resident restarts; clipboard and UI remain available. Process death or a sync
on another connection is not treated as surface-cleanup proof. Cancellation and
malformed replies fail closed. Diagnostics retain launch PID/start ticks and the last exit without
retaining image data. `kde_region_control` checks lifecycle failures with a C++ test
peer; no Node.js fixture is required on guests.

## Repeating verification

Use the normal build tools:

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev --output-junit build/dev/Testing/results.xml
```

Retain the exact clean commit, compiled manifest, executable hashes, registered test
set, JUnit and logs. Every implementation commit receives the generated development
version documented in [versioning.md](versioning.md); rebuild after committing.
Include `putgd-kde-region` in installed version/hash inventories and escaped-helper
checks. Linux truncates its process name to `putgd-kde-regio`; process names alone
are insufficient identity proof.

For this approved experiment, iterate on KDE first. Keep graphical interaction
visible on the VNC wall. Check resolved Unix socket byte lengths before isolated
launches. Preserve the original synthetic profile and normally quit the previous
resident before installing or starting its replacement. Use a new private candidate
profile and retain all failed attempts. Read-only exploratory RSS/PSS/private-memory
snapshots can reject an unpromising candidate; they do not prove a five-minute gate.

Before acceptance, validate native tray/shortcuts, window/desktop/region acquisition,
clipboard ownership and independent paste, live selector/cancellation, UI native
fonts/dialogs, notification and history behavior, and normal helper drain. Confirm
two independent post-local-native 300-second memory runs and a fresh empty-profile
cold run, with the frozen quiet/identity/coverage/CPU checks. The target is at most
40 MiB peak RSS for the complete resident family. Preserve PSS/private measurements
as diagnostics. Local capture evidence does not establish remote upload or deletion.

Once KDE is satisfactory, run both Cinnamon and GNOME functional and memory
regressions on the exact candidate version. Do not run the full three-desktop matrix
for every small KDE iteration. Preserve their checkpointed implementations while
fixing KDE-specific failures. See [KDE memory evidence](testing-kde-memory.md) and
[VM lifecycle](vm-lab.md) for the original baseline and lab rules.
