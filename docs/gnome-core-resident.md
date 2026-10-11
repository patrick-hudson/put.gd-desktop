# GNOME Core resident

On a native GNOME Wayland session, `putgd` can use QCoreApplication for the
listener, session-bus bridge and existing native tray. Settings, Activity,
History and About still run in the on-demand QApplication helper with native
fonts and dialogs. KDE and the existing Cinnamon X11 route retain their current
implementations. No GNOME extension, capture protocol or provider changes are
needed for this boundary.

## Routing contract

The Core route requires all of these conditions before Qt application construction:

- `XDG_SESSION_TYPE` is exactly `wayland` and `WAYLAND_DISPLAY` is nonempty.
- Colon-separated `XDG_CURRENT_DESKTOP` tokens include `GNOME`, ignoring case.
  A simultaneous `KDE`, `Cinnamon` or `X-Cinnamon` token rejects the Core route.
  Distribution tokens such as `ubuntu` may accompany GNOME. GNOME Xwayland may
  also set `DISPLAY`; that does not change the native Wayland route.
- `QT_QPA_PLATFORM` is empty, or its plugin token is exactly `wayland` or
  `wayland-egl`, optionally followed by colon options. Empty plugin names,
  lookalike prefixes, other plugins and semicolon fallback lists reject Core.
- Every argument is one of the resident's existing flags: `--background`,
  `--history`, `--activity`, `-h`, `--help`, `--help-all`, `-v` or `--version`.
  Any other token uses the graphical Qt path, so QApplication still consumes
  its GUI options (for example `-platformtheme gtk3` or `-style fusion`) before
  the application parser runs. This includes explicit Wayland requests and
  arguments after `--`. Unknown options retain the graphical parser's behavior.
  This conservative argument rule applies only to the GNOME route.

Rejected routes use the existing QApplication path. The GNOME adapter verifies
its session-bus peer and protocol independently: environment variables alone
are not proof of a working compositor. Its Core constructor checks the strict
GNOME environment; its graphical constructor retains the actual Qt Wayland
platform check. Calls to QGuiApplication are reached only on graphical paths.

## Reproduce the automated checks

Use the normal dependency setup from [Build](build.md), then:

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev --output-junit build/dev/Testing/results.xml
```

`native_session_guards` covers both the unchanged Cinnamon characterization and
the GNOME routing/argument table. `gnome_core_transport_integration` uses
`dbus-run-session`, QCoreApplication and a separate synthetic helper process.
Linux builds with testing enabled require `dbus-run-session` at configure time;
install the distro package providing it if configuration reports it missing.
The test is never silently skipped. Both the fixture and the Core transport
parent construct QCoreApplication, so the synthetic Wayland name needs no QPA
connection. CTest sets a synthetic GNOME environment; it does not connect to Wayland or
claim desktop acceptance. The test verifies actual D-Bus shortcut conversion,
clipboard import and chunked UTF-8 result copying, unsupported-session rejection,
UI-PID failure/recovery and helper-owner replacement. A separate graphical
`--adapter-transport` mode continues to require a real Qt Wayland connection.
`gnome_bridge_contract` retains bounded decode and composition checks.

The test-first Core transport run must fail against the earlier graphical-only
constructor and pass after the boundary change. Preserve XML and logs alongside
the exact source commit and build manifest; do not count the synthetic helper as
GNOME Shell, tray, capture, clipboard ownership or notification acceptance.

## Native acceptance and memory

First run the candidate's Cinnamon functional and memory regression gates.
Proceed to GNOME only after both pass. Use visible VNC, current installed package
hashes, native fixture/oracle checks and normal helper teardown. Verify the native
tray/menu and notifications as well as all capture, clipboard and view actions.
Keep native fonts and dialogs; do not disable theme integration to reduce memory.

Follow [Memory testing](testing-memory.md) with the frozen 5-second warmup,
300-second duration, 5-second interval and 61 samples. Retain cold and post-native
RSS, PSS, private memory, CPU, process identities and structured results. A useful
candidate needs repeatable saving over 1 MiB, two independent post-native runs
and the agreed cold/post-native 40 MiB RSS gates. Preserve any failed run and
identify its actual desktop and source version. Passing Cinnamon evidence stays
valid when a separate GNOME optimization misses its target.

Qt libraries remain dynamically linked. Skipping QApplication construction may
avoid GUI platform/theme initialization, but it does not establish that linked
Qt libraries were unmapped or predict the resulting resident RSS. Report measured
savings only after native acceptance and the frozen measurements.
