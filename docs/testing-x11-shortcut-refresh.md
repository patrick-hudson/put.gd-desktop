# X11 shortcut refresh regression

X11 shortcuts must remain active when a keyboard mapping notification leaves
their native keycode/modifier combinations unchanged. The original implementation
removed every passive grab, flushed those removals, then queried the modifier map
and installed replacement grabs. A chord processed between removal and replacement
never reached the shortcut connection. Waiting longer could not recover it.

`X11Shortcuts::bind()` now reconciles native combinations. It retains existing
combinations, installs missing ones, and removes obsolete ones after resolving all
actions. A rejected action rolls back only its newly installed combinations; the
final cleanup releases its retained combinations unless another accepted action
owns them. Sorted action precedence, status errors, lock variants, mapping changes,
and release of the active keyboard grab before region selection remain unchanged.

## Reproduce with ordinary tools

Use the system dependencies and SDK setup in [build.md](build.md). No new package,
alternate compiler, production hook, or live desktop is required. Run from the
repository root:

```sh
cmake --preset dev
cmake --build --preset dev --target test_x11
mkdir -p work/x11-shortcut-refresh
ctest --preset dev -R '^x11_isolated$' \
  --output-junit "$PWD/work/x11-shortcut-refresh/ctest.xml"
```

For the deterministic regression alone:

```sh
dbus-run-session -- xvfb-run -a env QT_QPA_PLATFORM=xcb \
  build/dev/test_x11 mappingRefreshKeepsUnchangedChordGrabbed \
  -o "$PWD/work/x11-shortcut-refresh/regression.xml,junitxml" -o -,txt
```

These commands create an isolated Xvfb display and session bus. Environments that
block local Unix sockets need permission to create those sockets. Do not point
these tests at the user's physical display or a lab desktop.

## Why this test is deterministic

The test binds a shortcut, changes an X server keyboard mapping to the same
symbols, and processes the resulting real `MappingNotify`. The scoped
`X11MappingBarrier` intercepts the modifier-map request in the test executable,
then forwards it to the distro's real libxcb through `QLibrary`. The fixture has
no effect outside its scope or on another thread.

At that boundary, a server roundtrip on the shortcut connection confirms earlier
requests have completed. A separate X11 client then sends one real XTEST chord
and completes another roundtrip before the mapping request proceeds. Thus the
server processes the chord during refresh without relying on sleep duration,
thread scheduling, test warmup, or retries. The test checks delivery of the
action, the refresh signal, and the bound status. It does not merely count grab
function calls or mock keyboard events.

`mappingConflictReleasesRejectedGrabsAndPreservesOtherActions` adds a native
keycode whose base combination is already owned by another client. It checks
the conflict result and attempts every Control-containing modifier mask on both
the retained and added keycodes, catching incomplete rollback without assuming
which modifier holds Num Lock. It also covers retry after conflict removal,
unaffected action delivery, native-combination transfer to another action,
duplicate-action precedence, disabled actions, and release on removal.

The existing tests still cover actual changed-keycode delivery, Caps Lock and
Num Lock, conflicts between clients, and handing a held shortcut chord to the
region selector.

## Evidence and limits

On 2026-10-08, the new regression failed against the original product source at
`0862bdade82994da23f4a11cb32b9fd31d483c26`: `activated` was zero instead of one.
The mapping barrier had fired and a refresh had completed. After the product
change, the regression and the complete isolated X11 suite passed on Linux Mint
22.3 with distro Qt 6.4.2 and GCC 13.2.0. The original diagnostic delay probe also
passed; unchanged mappings no longer reached its destructive rebind boundary.

Structured local evidence is under `work/memory-surfaces/x11-grab-fix/`:

- `pre-fix.json` records the starting commit, unrelated working changes, and
  red-test source hashes.
- `red.json`, `red.log`, and `red.xml` preserve the genuine delivery failure.
  `sandbox-red.*` separately preserve the initial socket-permission failure;
  that setup failure is not the red result.
- `green.*` preserve the first focused successful run.
- `original-probe.*` preserve replay of the unchanged diagnostic interposer.
- `final-x11-isolated.*` record the final complete suite, command, source and
  executable hashes, JUnit output, and detailed QtTest output.

Keep these receipts with the integration evidence when archiving or publishing
the fix. They distinguish source changes from a re-run of an old binary.

This proves an input-loss window and its correction on an isolated X server.
The first intermittent Qt 6.10.2 VM failure did not record its event scheduling,
so its exact cause remains unproven. Xvfb does not establish real Cinnamon,
GNOME, or KDE graphical acceptance. This correction is separate from the frozen
retained-window memory candidate; native package and integration evidence must
identify which combination of changes was actually built.
