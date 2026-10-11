# Testing shortcut dispatch

A confirmed desktop binding proves registration, not that a capture was accepted.
An upload action without a destination is a valid saved setup state, but
`OperationService::invoke` rejects it with `destination_required` before creating
an operation, opening a selector, reading pixels or copying the clipboard.
Native hotkeys and tray actions share admission-error presentation. Native
`busy_selecting` and `initializing` re-entry stays quiet so feedback cannot map a
view over selection/recovery. Other native errors notify the user without opening views; repeated feedback for
the same error is limited to once per two seconds. Tray-menu errors retain their
Activity fallback.
The gate holds one error code and a monotonic clock, without a timer or payload
cache. Logs retain rejection codes without payloads.

## Regression coverage

`native_shortcut_rejection_integration` launches the actual resident and CLI on
a private D-Bus session and Xvfb desktop. A real XTEST Ctrl+1 chord reaches an
upload action without a destination. The test checks the notification's message,
absence of an operation, bounded repeated-press feedback, absence of a UI helper
and normal shutdown. The GUI tray suite also checks selection and recovery
re-entry, full queues and clipboard admission errors without mapping a view. Its pre-fix
result was zero notifications. This fixture proves transport and wiring; it is
not compositor or visible notification acceptance.

`liveDigitRebindRemovesOldChordAndKeepsOtherActions` in `x11_isolated` exercises
default -> Ctrl+1 -> Ctrl+Alt+1, old-chord removal, unchanged desktop shortcuts
and exactly one action per press. `savingOnlyAShortcutPreservesEveryUploadDestination`
in `gui_settings` checks the real asynchronous settings-save service without
provider network requests or credential access.

Use the standard development presets. Before launching, create a short private
TMPDIR and check complete resolved socket paths; the full path, including
`putgd/session.sock`, must fit in 107 UTF-8 bytes. For example:

```sh
cmake --preset dev
cmake --build --preset dev
export TMPDIR=$(mktemp -d /tmp/pgsc.XXXXXX)
python3 tools/diagnostics/check_socket_paths.py --socket \
  "$TMPDIR/test_native_tray-XXXXXX/r/putgd/session.sock"
ctest --preset dev -R '^(native_shortcut_rejection_integration|x11_isolated|gui_settings|native_tray_integration|gui_tray)$'
ctest --preset dev
```

The dedicated shortcut test is guarded by its CTest-only environment flag so the
ordinary tray suite cannot inject input on an inherited physical desktop.

## Desktop acceptance and diagnosis

Use an isolated local-only destination and cancel selections before capturing.
Verify the GUI Save/rebind path, new-chord activation, old-chord removal and
unchanged actions on Cinnamon first. Include foreground applications and lock
states. Also verify a missing destination produces visible feedback without an
operation, then explicitly select a destination and retry. Preserve failure
attempts separately from corrected test-driver runs. Record build manifests,
executable hashes, revisions, operation IDs, terminal outcomes and UI/helper
lifetimes; exclude secrets and payloads.

Run the unchanged whole-process-family memory harness after functional acceptance,
with views closed and no outside input. Expand to current GNOME and KDE builds
only after Cinnamon functional and memory gates pass. Keep guest graphical
interactions visible through their VNC listeners. Headless successes and earlier
versions do not establish these desktop gates.

The original issue #8 preview had an accepted/bound Ctrl+1 but no region upload
destination; the other actions retained theirs. The preview setup script assigned
all four destinations. A clean shortcut-only GUI Save preserves them. The cause
of the original assignment becoming empty is unresolved; do not attribute it to
keyboard mapping or claim this change repairs unknown settings corruption.
Activity records accepted operations only. If desktop notification support is
unavailable, a native admission error remains in the application log; the tray
menu retains its existing Activity fallback without a rejected-operation row.
This delivery limitation is distinct from a shortcut registration failure.
