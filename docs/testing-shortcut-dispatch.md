# Testing shortcut dispatch

A confirmed desktop binding proves registration, not that a capture was accepted.
An action without an upload destination is a valid saved setup state. When
`archive.keepUploads` is enabled, it saves locally. With both outputs disabled,
`OperationService::invoke` rejects it with `output_required` before creating
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
Verify the GUI autosave/rebind path, new-chord activation, old-chord removal and
unchanged actions on each desktop. Include foreground applications and lock
states. Also verify disabling both outputs produces visible feedback without an
operation, then enable a destination or local saving and retry. Preserve failure
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
all four destinations. A clean shortcut-only GUI save preserves them. The cause
of the original assignment becoming empty is unresolved; do not attribute it to
keyboard mapping or claim this change repairs unknown settings corruption.
Activity records accepted operations only. If desktop notification support is
unavailable, a native admission error remains in the application log; the tray
menu retains its existing Activity fallback without a rejected-operation row.
This delivery limitation is distinct from a shortcut registration failure.

## KDE shifted keys and keyboard layouts

KGlobalAccel reporting a bound shortcut is insufficient proof of native delivery.
The KDE test at version 100 exposed an older registration defect: the configured
`Ctrl+Shift+1` was sent as a literal Qt key combination, while the same physical
press on the guest's US layout reached KWin as `Ctrl+!`. An explicit `Ctrl+!`
registration delivered that press; plain `Ctrl+1` also delivered. The failed
native default-key attempt remains a failure, separate from these diagnostic
controls. Earlier KDE evidence using F9–F12 did not qualify the default chords.

For printable ASCII number and punctuation keys with explicit Shift, registration derives the
effective symbol and consumed Shift from the compositor-provided XKB map and
current KDE layout. Unshifted symbols (including AltGr-produced ones), letters, special keys and
keypad/group-switch chords retain their existing
literal wire translation, including Qt/KWin handling of Latin shortcuts in non-Latin layouts.
This change does not recreate Qt's private key-conversion implementation. Settings retain the original
requested text. Diagnostics expose the effective sequence separately. The
existing private Wayland connection subscribes to its seat's keyboard map;
no graphical application, surface or clipboard ownership change is needed.
An incoming map is limited to 1 MiB, read from a checked regular descriptor and
compiled once; its text buffer is released immediately. Each replacement gets
a fresh compilation context, released with the map, so interned names from old
keyboard maps cannot accumulate in a long-lived shared context. Every incoming descriptor
is closed on success or failure. The resident retains one compiled map, shared
by reference with shortcut registration, without a per-key cache.

A client without keyboard focus cannot depend on Wayland modifier events for
its current global layout. Registration queries `org.kde.keyboard` at `/Layouts`
using `org.kde.KeyboardLayouts.getLayout` for multi-group maps. A map with one
group determines group zero directly, regardless of its language or layout-service
availability. Valid layout-change signals recompute immediately; unchanged effective
registrations remain live. Unknown multi-group state leaves bindings unavailable.
Failed discovery retries use capped backoff without repeated teardown or diagnostics
refresh. Async generation checks reject stale query and registration replies.
A returned or externally changed sequence must match the effective request before
it can be reported as bound.

The `kde_shortcut_normalization` unit suite checks symbols and consumed modifiers
across synthetic US/French layouts and Latin-1 shifted symbols such as £, § and ·. `kde_keyboard_map` checks malformed, truncated,
unsupported and oversized maps and descriptor cleanup. `kde_shortcuts_integration`
uses a disposable D-Bus fixture to check registration, layout changes, conflicts
and lifecycle races, including a layout change while the initial event-subscription
lookup is pending. Normalization failures retain specific status/error codes rather
than being mislabeled as syntax errors; the GUI preserves the saved draft and exact
diagnostics. These are protocol/unit checks, not Plasma acceptance.

For native acceptance, preserve the recorded source revision and executable hash;
start with the unmodified default four shortcuts and invoke each with actual keys.
Check exactly one delivery, then edit region to `Ctrl+1` and `Ctrl+Alt+1`, verifying
old-chord removal and unchanged actions. Check the requested and effective fields
separately. Repeat a controlled layout switch while the app is unfocused, restoring
the prior layout afterward. Capture and clipboard cases retain their full-pixel
and paste oracles. Only after functional checks pass, run the unchanged serial
whole-process-family memory sampler. Keep all failed attempts and diagnostic
controls labeled separately. Do not substitute function keys for failed defaults.

Implementation references: the public [Wayland keyboard keymap protocol](https://wayland.freedesktop.org/docs/html/apa.html#protocol-spec-wl_keyboard),
[libxkbcommon consumed-modifier API](https://xkbcommon.org/doc/current/group__state.html),
and KDE's [keyboard layout D-Bus interface](https://github.com/KDE/kwin/blob/v6.6.6/src/keyboard_layout.h)
and [shortcut event processing](https://github.com/KDE/kwin/blob/v6.6.6/src/xkb.cpp).
