# Linux releases

Release builds use C++20, Qt 6, the pinned official AWS SDK, CMake presets, Ninja
and native packaging tools. The application is MIT licensed. The first public
track is a prerelease. Flatpak is separate work in GitHub issue #14; its selected
host capture companion must preserve the current hotkey workflow.

## Formats and build environments

| Asset | Builder | Qt distribution |
| --- | --- | --- |
| Ubuntu 24.04 / Mint 22.3 DEB | pinned Ubuntu 24.04 container | distro Qt 6.4.2 minimum API |
| Ubuntu 26.04 / Kubuntu DEB | pinned Ubuntu 26.04 container | distro Qt and LayerShellQt |
| Fedora 44 RPM | pinned Fedora 44 container | distro Qt and LayerShellQt |
| x86_64 AppImage | Ubuntu 24.04, official Qt 6.8.3, locked ECM/LayerShellQt | dynamically bundled Qt/plugins |

AppImage requires glibc 2.39 or newer; it does not substitute the host libc.
DEB/RPM continue using system Qt. AppImage's normal bundled runtime is the
explicit distribution exception added by the user's October 12 release request.
The GTK platform theme must remain available for native dialogs/fonts. Never
substitute a newer-glibc build or omit KDE region support to produce an asset.

GNOME capture currently targets Shell 50 and requires the host extension.
Release collection also emits the matching extension ZIP. Installing files inside
an AppImage does not install a host extension. See [GNOME setup](desktops/gnome.md).
Minimum-Qt DEB compatibility compilation is separate from GNOME/KDE compositor
qualification. Windows/macOS CI compilation is future-platform evidence, not a
native support claim.

## GitHub downloads and source

The GitHub Release page publishes each installer as an individual download:
Ubuntu 24.04 and 26.04 DEBs, the Fedora 44 RPM, and the x86_64 AppImage. It also
publishes `putgd-VERSION-source.tar.gz`, the GNOME extension ZIP, full build
receipts and `SHA256SUMS`. `putgd-VERSION-release.zip` contains these seven
underlying files and an inner checksum list; the outer checksum list also covers
the combined ZIP. ZIP timestamps and modes are fixed from the source commit,
so repeating collection preserves the bytes needed to resume a draft. Extract the
combined ZIP into a new directory; its inner checksum list covers the contents,
while the separately downloaded outer list also covers the ZIP itself. Actions artifact ZIPs are the internal build handoff;
the final release assets are the raw files.

The curated source tarball exports committed files from a positive allowlist:
the application's `src/`, CMake files/presets, native packaging, GNOME integration,
licenses, dependency provisioning/verifier and build instructions. It contains
no `work/`, builds, credentials, agent context, plans/reviews/evidence, lab images,
run logs or test artifacts. Its generated `source-info.json` records the original
commit/tree/count and every included file hash. Archive timestamps, modes and
ownership are deterministic.

`README_SOURCE.md` gives normal CMake/Ninja commands for the extracted source.
No Git history or third-party SDK binary prefix is bundled; provision the official
pinned AWS SDK separately. Extracted builds use the existing supplied-provenance
CMake flags, retaining the original commit/count with a development version.
They do not claim locally observed clean Git or an exact release-tag build.
Official binary release assets continue to come from a clean tagged checkout.
The Ubuntu 24.04 packaging job also compiles an extracted export with the normal
release preset, verifying the archive contains the required compilation inputs.
The collector requires that successful compilation receipt and publishes the exact
archive bytes that were tested.

## Version identity

Normal builds remain `0.1.1-dev.N+gHASH`; each commit changes their identity.
An explicit clean exact `v0.1.1-rc.1` tag can produce `0.1.1-rc.1`. Stable labels
use `vMAJOR.MINOR.PATCH`, matching the project base. Dirty, missing, mismatched and
arbitrary archive release labels are rejected. [versioning.md](versioning.md)
explains package ordering and the archive provenance boundary.

Every product executable exposes `--version-json` before GUI initialization.
Package installation checks the entire embedded manifest, not only the version
string: equal-version binaries from different commits must be rejected. Module
runtime hashes and the pinned SDK/component license snapshot remain mandatory.

## Build and audit locally

Use the standard packages and SDK provisioning in [build.md](build.md). For a
fixture-free native package:

```sh
cmake --preset release -DBUILD_TESTING=OFF -DPUTGD_LAB_FIXTURE=OFF -DPUTGD_PHYSICAL_BENCHMARKS=OFF -DCMAKE_INSTALL_PREFIX=/usr -DPUTGD_RELEASE_TAG=v0.1.1-rc.1
cmake --build --preset release
python3 tools/releases/native.py --build build/release --format DEB --output /tmp/new-release-deb
```

Run `--format RPM` on Fedora with its matching system libraries. Never convert
a DEB into an RPM or reuse another distro's SDK binary prefix. The output must
be a new absent directory. The wrapper uses stock CPack/dpkg-deb or RPM/cpio,
then checks package ownership/modes, helper identities, licenses, SDK/module
SHA256 values and ELF resolution. RPM dependency detection excludes private
SDK SONAMEs while retaining system requirements; post-install stripping is
suppressed because it would invalidate recorded runtime digests.

Outputs include the package, extracted inventory, native-audit.json,
build-info.json, SHA256SUMS and CPack logs. [AppImage assembly](../packaging/appimage/README.md)
uses locked official deployment tools around the same CMake install tree.
It has a separate inventory because Qt/plugins are bundled and ELF RPATHs may
change. Its receipt explicitly records desktop acceptance pending after assembly.

## GitHub build and publication

`.github/workflows/release.yml` builds packages for exact tags and manual runs.
Packaging-related pull requests build/audit development assets without publishing.
Ordinary application PRs retain the main Build matrix; package builds are not
added to every unrelated edit. Release builders are ABI-separated, and all
application builds use normal presets rather than an alternate build system.

The release collector requires three native assets and the AppImage from one
clean tagged commit/tree/count/version. It recalculates each artifact SHA256
against its build receipt and assigns distro-specific filenames so the two
DEBs cannot overwrite each other. Full receipts and SHA256SUMS accompany the
packages. Only the final staging job has contents-write permission.

A successful exact-commit Build workflow is required before staging a GitHub
draft. Tags alone do not prove current compilation or package acceptance. Draft
staging uses `gh release create --verify-tag`; the first track is a prerelease.
A rerun can resume a partial draft: existing uploaded bytes must match exactly,
and only missing assets are uploaded. Incomplete/conflicting assets or an
already published release are rejected for inspection; existing assets are
never silently replaced.
Public publication is the last step after actual package-specific acceptance.
The publisher downloads the exact draft assets, checks every accepted checksum
and the checksum file, and rejects missing/extra assets, a different remote tag,
or assets changed during verification. GitHub does not offer an atomic lock
across verification and publication; coordinate other release writers. The
publication receipt retains verified remote asset identities:


```sh
python3 tools/releases/publish.py --tag v0.1.1-rc.1 --directory build/publish --publish --acceptance /private/current-package-acceptance.json
```

The acceptance receipt must identify the exact tag/commit and every collected
asset checksum, with actual passed results. It is a summary of observed tests,
not a replacement for their evidence. Preserve the commands, parameters, source
and package hashes, desktop/backend versions, JUnit/logs, screenshots, operation
IDs, public byte comparisons, exact guarded cleanup and quiet-family memory
results. Never include credentials, private login input or payloads in release
artifacts, logs or review material.

## Acceptance boundary

Hosted packaging/ELF checks and saved source114 evidence replay establish their
stated contracts. They do not prove the new packages' compositor integration.
Cinnamon functionality and memory come first for a new candidate; after passing,
GNOME/KDE checks may proceed independently on the exact package. Keep graphical
interactions visible through the VNC wall and use immutable-base overlays.

AppImage format overhead is accepted as a documented exception to the native
40 MiB target. Preserve the measured strict-gate result and report the entire
family; do not count only the application or label an over-budget sample as
passing. See the measured compression tradeoff in [AppImage assembly](../packaging/appimage/README.md).

For AppImage, include the outer launcher/mount processes in family memory and
prove GTK dialogs/fonts, persistent-image autostart, KDE permission registration
and cleanup, all capture/clipboard actions, local archives, live S3/history and
production CLI/MCP behavior. Native packages need install/upgrade/removal and
host integration checks appropriate to the target distro. Full U9/U10 release
certification remains separate; [verification.md](verification.md) and the
implementation contract list remaining gates. A prerelease must describe its
observed coverage honestly, without relabeling historical source114 tests.
