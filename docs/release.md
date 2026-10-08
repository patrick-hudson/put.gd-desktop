# Linux packaging and release evidence

The repository builds development DEBs through ordinary CMake, Ninja and CPack.
It has not completed U10 release acceptance. [verification.md](verification.md)
records observed checks; the [plan](plans/2026-10-07-1926-feat-qt-linux-share-plan.md)
defines the remaining contract. These instructions document existing tools and the
evidence still needed before distribution.

## Build a package

Provision the pinned official SDK and normal distro development packages using
[build.md](build.md). Verify the SDK source before rebuilding it:

```sh
python3 tools/verify_dependencies.py build/dependencies/aws-sdk-cpp
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Build on each target distro against its own Qt and runtime packages. A Mint DEB
does not establish Ubuntu or Kubuntu ABI/dependency compatibility. CMake defaults
Linux installation to `/usr`; the current project version is `0.1.0`.

For a release candidate, explicitly turn off development fixtures and benchmark
drivers, including when reusing a previously configured build directory:

```sh
cmake --preset release -DBUILD_TESTING=OFF -DPUTGD_LAB_FIXTURE=OFF -DPUTGD_PHYSICAL_BENCHMARKS=OFF
cmake --build --preset release
cpack --config build/release/CPackConfig.cmake -G DEB -B build/packages/release
```

`BUILD_TESTING=OFF` excludes QtTest; it does not run or replace the separate dev
checks. The release preset otherwise inherits testing enabled. CPack emits the
architecture/version-named DEB into the selected output directory. Record that
exact filename and SHA256, and retain the build inputs and CMake cache.

The graphical lab needs a separate package containing its independent synthetic
desktop fixture:

```sh
cmake --preset dev -DPUTGD_LAB_FIXTURE=ON
cmake --build --preset dev
cpack --config build/dev/CPackConfig.cmake -G DEB -B build/packages/lab
```

The fixture is a test application, not a capture backend. Never distribute that
package as the fixture-free release candidate. [vm-lab.md](vm-lab.md) documents
fresh overlays, installation, scenario execution and curated evidence export.
`lab.py install` uses apt dependency resolution with `--reinstall` and compares
installed app/CLI hashes to the requested package; this matters while development
rebuilds share version `0.1.0`. No release lifecycle runner or release publication
workflow is established by these commands.

## Contents and rebuild inputs

The package installs `putgd`, `putgd-cli`, `putgd-launcher`, an icon, the desktop
entry, an opt-in autostart template and the GNOME extension under the system
extension directory. The desktop entry names the actual installed resident and
declares KWin ScreenShot2 permission. The app has an installed private-library
RPATH; the launcher additionally scopes the AWS library search path to its process.

Qt, QtKeychain, XCB, Wayland and other system libraries stay dynamically linked
to distribution packages. CPack uses `dpkg-shlibdeps` and explicitly requires
`libqt6sql6-sqlite`, `qt6-qpa-plugins` and `qt6-wayland`; it does not copy Qt plugins
into the DEB. Plasma region selection additionally needs the current distro
LayerShellQt interface with Qt 6.5 or later. The minimum Qt 6.4.2 compatibility
build reports that region capability unavailable.

The official AWS SDK/CRT shared objects and s2n libraries are copied into the
private `putgd` library directory. Packaging records their SHA256 values in
`/usr/share/putgd/aws-runtime-manifest.json`, installs the dependency lock there,
and copies SDK/component licenses plus the protocol notice to
`/usr/share/doc/putgd`. See [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).
`PUTGD_AWS_RUNTIME_DIR` and `PUTGD_AWS_SOURCE_DIR` are CMake cache paths for the
provisioned official runtime and source; neither downloads dependencies.

Before distributing a candidate, inspect the actual DEB inventory and installed
files. Record distro package versions, compiler/CMake/Ninja versions, source
revision and dirty state, SDK/submodule identities, configuration options,
runtime hashes, package SHA256 and native integration versions. Verify every
bundled shared object has its required source/license notice and that plugins
resolve from installed distro packages. Complete license inventory, security
maintenance ownership and the dependency update/rebuild record remain gates;
the existing notice copies alone do not satisfy them. The optional Python MCP
helper is separate and uses [its locked setup](mcp.md); it is not in the native DEB.

## Installed lifecycle evidence

Use fresh disposable runs on Mint 22.3 Cinnamon/X11, Ubuntu 26.04 GNOME
50/Wayland and Kubuntu 26.04 Plasma 6.6/Wayland. Preserve immutable golden disks.
Keep native interactions visible on each localhost VNC listener and the display
wall. Restore a locked/blanked session visibly before acceptance. SSH builds,
offscreen widgets and QMP socket fixtures are separate evidence layers.

Each profile needs the following recorded outcomes, using only synthetic payloads
and an isolated destination:

- Fresh install: install with distro dependency resolution, launch without a
  source checkout, verify binary/runtime/plugin identity, and complete setup.
  Fresh configuration must require destination setup or explicit local-only
  selection; it must not activate the development bucket template. Prove region,
  window, desktop and all clipboard types through global hotkeys, saved/uploaded
  bytes and paste into a second native application.
- Upgrade: retain customized non-secret settings, secret-store references,
  retained operations, populated history, original receipts and deletion states.
  Compare these before and after upgrading distinct package versions, restart,
  then prove offline history and retained-operation recovery. Configuration is
  currently schema version 1 with no predecessor migration; unsupported versions
  remain untouched. Existing contract tests do not prove package upgrade or a
  future schema migration.
- Login startup: enable the GUI preference, inspect the managed per-user XDG
  autostart entry, log out/in through the visible session and prove exactly one
  resident and actual bindings. Disable it and repeat login to prove absence.
  Startup code refuses to overwrite an unmanaged entry. The system template is
  opt-in and does not enable the preference itself.
- GNOME lifecycle: record the installed extension location/version, enable it
  once discovered by Shell, then disable/re-enable during idle and acquisition.
  Prove selectors, bindings, signals and IPC disappear and recover once; repeat
  after a fresh Wayland login. Test extension removal and package upgrade, including
  a stale per-user extension shadowing the system copy. See [GNOME setup](desktops/gnome.md).
- Plasma lifecycle: verify the desktop entry matches the running executable,
  test permission rejection for absent/mismatched entries, restart after install,
  and prove bindings and data-control recover across session/service restart.
  Do not bypass permission checks. See [KDE setup](desktops/kde.md).
- Removal: disable login startup and the extension, stop the resident, remove the
  package using the distro manager, then log in again and inspect remaining hooks.
  Preserve user-owned outputs, history and configuration. Current packaging has
  no maintainer removal hooks for per-user autostart or extension enablement;
  automatic clean removal and stale-hook handling remain implementation/acceptance
  work. Do not describe manual preparation as proof of automatic uninstall cleanup.

Retain per-attempt results/JUnit, scenario parameters, package and runner hashes,
native session/Qt/backend versions, display layouts, QMP witnesses, expected/actual
pixel and byte hashes, operation/history/deletion IDs and independent paste results.
Remote checks require unauthenticated public GET byte comparisons and the exact
prepared deletion effects followed by individual confirmed outcomes. Never alter
bucket policy/versioning or pre-existing objects. Export only curated redacted
records; run directories contain private VM material.

## Open release gates

Single-head native local workflows have partial recorded evidence, but the full
three-profile matrix still needs geometry/scaling/rotation/removal, normal GTK/file
manager interop, ownership/conflict/failure flows, GUI history/delete/configuration
flows, installed visual/focus/error states and lifecycle checks. Physical Cinnamon
workflow evidence, the full fresh-agent/U9 walkthrough and successful live S3
publication/public bytes/guarded deletion are still required. Live permission
failures do not establish publication or cleanup acceptance. Windows/macOS native
support is future work; authored compile CI is not an observed pass.

The retained five-minute physical Cinnamon background run peaked at 55.95 MiB
and 0.00333% of one CPU core before first view/S3 use. It passes the original
100 MiB/0.5% idle budgets but misses the approximately 40 MiB requested target.
The physical 30-sample painted-overlay p95 is 235.821 ms and fails the 150 ms
budget. Post-use, 10,000-entry history, upload-burst and slow-network measurements
remain open. [testing.md](testing.md) documents the existing supervisors; sanitizers
and virtual/TCG runs are excluded from performance acceptance.

The current 25-target dev pass and four affected ASan/UBSan and instrumented-Qt
TSan passes cover their stated contracts, not release acceptance. Package checksums,
installed inventory/notices, complete lifecycle records and the remaining native,
publication and performance gates must be completed before claiming U10 readiness.
