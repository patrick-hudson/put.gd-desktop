# Login-startup uninstall contract (#382)

Patrick approved preserving the per-user **Launch at login** preference across
uninstall, adding native `TryExec`, and documenting the explicit user-scoped
removal route. [Code PR #544](https://github.com/patrick-hudson/put.gd-desktop/pull/544)
implements that decision. [Configuration](../../configuration.md#launch-at-login-and-uninstall-linux)
documents Settings, CLI and MCP use. No package script searches user homes.

## Exact source and execution scope

Tested on physical `mint-builder`, `patrick@192.168.68.74`, Linux Mint 22.3,
through SSH only. The build uses distro Qt **6.4.2**, GCC 13.3.0, C++20, CMake
presets/Ninja and the existing official AWS SDK 1.11.907. The SDK source and its
15 submodules passed `tools/verify_dependencies.py` before configuration.

- Clean source: `91864c41bcc4c430d6ca967593fc2420c6dc706a`.
- Tree: `8978e584b61233c42032c50d3a1eb191b0fd047a`.
- Version: `26.1024.1182-dev+g91864c41bcc4`.
- Tested production resident SHA-256:
  `99cbf822a8ee563f62e1f88c433d768e2f2dc6da73b67666d91670c956b58de8`.

[Build identity](build-identity.json), [host/source](host-source.json) and
[commands](commands.json) preserve the exact identities and invocation. Builds
used `nice -n 10` with two jobs in the private checkout
`/home/patrick/work/pg382`. Optional recorder development libraries were absent;
the recording helper was not built or tested. No package, desktop entry or
credential in the host's real graphical profile was changed.

These are file/startup contracts, desktop-entry parser checks and an isolated
offscreen production CLI check. They do not certify a desktop compositor,
actual package removal, a real login, memory, or a stable release.

## Results

- `cmake --preset dev`, the focused preset build and
  `ctest --preset dev -R '^login_startup_contract$'` passed. The contract covers
  native `TryExec` pathname escaping, repair of an older retained managed entry,
  managed-only removal, foreign-entry/script preservation, and AppImage startup
  independent of a temporary mount. [CTest](ctest.log), [JUnit](ctest.xml).
- All **18** `tests/unit/test_appimage_login_start.py` tests passed. Missing
  images, a missing image directory and a missing development image leave a
  quiet successful preference. An existing non-executable image still fails;
  channel, ownership, permission, symlink and update-selection tests also pass.
  [Script output](appimage-script.log).
- The [production resident/CLI check](cli-result.json) passed with private XDG
  folders and no display or real session bus. GLib decoded the generated
  `TryExec`; `desktop-file-validate` accepted the entry. GIO rejected that same
  retained entry while its executable was temporarily renamed, then recognized
  it after restoration. The entry bytes and enabled configuration survived
  unchanged while the executable was absent.
- An expected-revision production `settings.patch` setting
  `preferences.launchAtLogin` to `false` removed the managed entry.
  `ui.snapshot` reported `loginStartup: {enabled:false, applied:true}`.
  This verifies the asynchronous CLI confirmation documented for uninstall.
  The build-tree resident uses its current executable as the launcher fallback;
  it is not an installed-package launcher check. MCP schema/forwarding was
  inspected; no fresh MCP stdio integration run is claimed.

Every resolved test session/bus socket was checked before launch:
[unit preflight](unit-socket-preflight.log), [CLI preflight](cli-socket-preflight.log).
The resident used a new process group and only its owned group was terminated
after the check. [Cleanup](cli-cleanup.json) records the stopped process; exit
`-15` is the harness's explicit SIGTERM, not a product crash finding. Private
synthetic state and logs remain on the builder for inspection.

## Failed setup/probe attempts retained

The first configure located the shared SDK through `CMAKE_PREFIX_PATH` but lacked
the packaging runtime path. The second supplied that path but lacked the SDK
source location required for licence inventory. The corrected command supplies
all three: `CMAKE_PREFIX_PATH`, `PUTGD_AWS_RUNTIME_DIR` and
`PUTGD_AWS_SOURCE_DIR`. [Configure attempts](configure-attempts.json) preserve the
failed commands and exit codes; their full logs remain under
`work/i382-check/` and `work/i382-check2/` in the private builder checkout.
The successful run is `work/i382-check3/`.

The first GIO absence probe assumed PyGObject returned `None` for a rejected
entry. It instead raises `TypeError: constructor returned NULL`, which is the
expected GIO rejection exposed by the binding. The corrected
[harness](cli-check.py) handles that return form and passed the complete test.
Both attempts restored the renamed executable and stopped their resident;
[first cleanup](first-probe-cleanup.json) is retained. The original probe and
logs remain at `/home/patrick/work/pg382-cli-check.py` and
`work/i382-cli-check/`; the corrected run is `work/i382-cli-check2/`.
Local copies and original driver source are retained under the code worktree's
private `work/i382/`.

## Remaining acceptance

#382 remains open. Current-build package removal followed by real login still
needs the required Mint, Ubuntu, Kubuntu, Debian and Fedora evidence under
#382/#384. The earlier [native campaign](../2026-10-23-native-full-acceptance/README.md)
records per-desktop observations with and without `TryExec`; it is historical
evidence for its own build. In particular, `TryExec` does not remove every
GNOME/Cinnamon warning or startup-settings row. No waiver or graphical pass is
inferred from the headless results here.
