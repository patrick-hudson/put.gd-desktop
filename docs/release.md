# Linux releases

Release builds use C++20, Qt 6, the pinned official AWS SDK, CMake presets, Ninja
and native packaging tools. The application is MIT licensed. The first public
track is a prerelease. Flatpak is separate work in GitHub issue #14; its selected
host capture companion must preserve the current hotkey workflow.

## Stable and nightly channels

`main` is the stable source branch. `nightly` is the development integration
branch; ordinary feature PRs can target it, followed by an explicit promotion PR
into `main`. Promotion into `main` is never automatic. Every push to `main`
(including a merged PR) automatically fast-forwards `nightly` to current `main`
and starts nightly coordination. If `nightly` already contains `main`, its newer
work is retained. Divergent branches fail visibly and require a reviewed merge;
automation never force-pushes either branch. Keep both histories append-only and
fast-forward normal branch updates; do not force-push or move published tags.

| Channel | Tag | Publication |
| --- | --- | --- |
| Stable | `vBASE` | deliberate publication after exact-package acceptance |
| Candidate | `vBASE-rc.N` | prerelease after exact-package acceptance |
| Nightly | `vBASE-dev.COUNT.UTC+gSHORT` | automated, build-tested prerelease |

`UTC` is the source commit's 14-digit UTC committer time, not the runner clock.
`COUNT` is its full-history commit count; `SHORT` is its first 12 lowercase hash
characters. The tag, base, full commit, tree and count must match the clean source.
Nightlies never replace GitHub's Latest stable release. They retain all twelve
individual downloads listed below, including both raw DEBs, RPM and the exact
compiled curated source archive.

`.github/workflows/nightly.yml` checks pushes to `main` and `nightly` and has a daily
05:23 UTC catch-up schedule on default `main`. GitHub can delay scheduled work;
this is not a delivery-time guarantee. Every new run synchronizes from current
`main` before pinning `nightly`, including direct nightly pushes and manual runs
selected on either branch. This catches up main even when Actions coalesces
pending pushes. Intermediate push commits need not each produce a release.
The schedule skips an unchanged source
only after verifying its existing public nightly, source identity, complete
asset inventory and checksums. A tag, draft or failed run alone is not success.

The coordinator saves a preparation receipt before fan-out and pins its source
on retries. It dispatches `release.yml` on that immutable tag so the packaging
workflow definition comes from the same source as its code. Token-created tag
pushes and token-created branch updates do not start another push workflow.
The same coordinator run explicitly dispatches packaging after synchronization;
it does not wait for the branch update to trigger a second coordinator.
CI and package builders run concurrently;
publication waits for the verified exact-source producers and all package audits.
Existing successful CI is reused. Coordinated builds omit only the two generic
Linux production compiles already covered by native package builds. These rules
are shared by `main` and `nightly`, including coordinated candidate/stable runs.

Nightly notes state automated coverage: Linux test suites, portable compilation,
package audits and the extracted-source production build. They are development
releases, not new graphical, memory or live-S3 certification. Stable/candidate
publication retains the exact-package acceptance gate below.

### Bootstrap and manual recovery

Activate the branch only after these compatible workflows have merged into
`main`. If `nightly` already exists, inspect its history and use an ordinary
reviewed merge; do not reset it. For the first branch, resolve and retain the
actual merged main SHA, then create the reference and run the coordinator:

```sh
release_main_sha=$(gh api repos/patrick-hudson/put.gd-desktop/git/ref/heads/main --jq .object.sha)
gh api --method POST repos/patrick-hudson/put.gd-desktop/git/refs -f ref=refs/heads/nightly -f "sha=$release_main_sha"
gh workflow run nightly.yml --ref main --repo patrick-hudson/put.gd-desktop
```

The manual route uses the same preparation path as the schedule. Check existing
coordinator runs first to avoid a redundant manual dispatch. Retain the
coordinator run ID, `nightly-preparation-RUN_ID`, orchestration receipt and the
separate package run ID. Coordinator IDs trace the caller; independently verified
Build and package IDs establish the publication gates. The coordinator proves dispatch or unchanged-source
skip; the package run and verified public release prove publication.

Use GitHub's rerun controls on the original failed run. A retry must recover its
original preparation; a missing or contradictory receipt stops it rather than
selecting a newer branch. Only marked nightly-publication runs are eligible package handoffs; manual
package-only runs are excluded. Reuse preserves the original coordinator and run
IDs. Failed runs, or completed runs without a verified public nightly, report
the original run ID for recovery. Rerun cancelled required CI before retrying
the original package workflow. Partial publication requires the original
audited artifacts: matching uploaded bytes are retained and only missing files
are uploaded. Missing handoffs or conflicting rebuilt bytes block recovery;
never replace public assets or allocate a new tag for the unchanged source.
Preparation/orchestration and final publication receipts are retained for 90 days; package handoffs
for 30 days. Durable public `release-builds.json`, checksums and release notes
remain the source of truth after Actions artifacts expire.

Record the first observed manual publication and unchanged-source skip separately
from the first real scheduled event. A manual run does not prove the scheduler
has fired. [CI coverage](ci.md) describes retained checks and evidence.

## Proton experimental production integration

The user approved shipping and maintaining Proton on 2026-10-15. Its
experimental designation records compatibility maturity. Upstream endorsement
or SDK general availability is not a publication prerequisite; the concrete
[evidence and release gates](proton-drive-release-gates.md) remain applicable.

Linux x86-64 Build and release jobs provision and test the pinned official JS
helper, enable its bundle explicitly, and require matching runtime, notices and
source catalog in native/AppImage audits. Windows/macOS Proton support remains
outside this release scope. The curated source archive includes Proton build
inputs and its production rebuild checks the extracted helper.

Every Proton-enabled release also delivers `proton-source-catalog.json` and both
`putgd-proton-corresponding-source.tar.gz.part1` / `.part2` files. The collector
verifies the complete transport and all six original source packets, then binds
the catalog/helper hashes to every installer. Concatenate the parts in numeric
order and verify the combined hash before extracting. These large source parts
are separate downloads; the combined application ZIP includes the catalog and
instructions. Preserve them while the corresponding binary release is available.

Final packaged functional/resource acceptance remains a separate receipt.
Previously tested private deployments do not establish package acceptance. P5
credits the actual Crypto source update and completed rebuild work; ongoing
compatibility checks remain in the maintenance procedure.

For each subsequent release carrying Proton, the release maintainer retains the
integration maintainer's dated compatibility/migration check, pinned SDK/Crypto/
Bun versions, runtime/manifest/notices hashes, supported asset list and current
acceptance evidence. Follow the [maintenance procedure](proton-drive-release-gates.md#maintenance-procedure).
An upstream required update or observed service incompatibility requires a
compatible put.gd release and preserved recovery state. Existing stable/candidate
and nightly coverage classes continue to apply.

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
receipts, the Proton source catalog and two source transport parts, and
`SHA256SUMS`. `putgd-VERSION-release.zip` contains the eight smaller underlying
files and an inner checksum list. The two large source parts stay separate; the
outer checksum list covers them and the combined ZIP. ZIP timestamps and modes are fixed from the source commit,
so repeating collection preserves the bytes needed to resume a draft. Extract the
combined ZIP into a new directory; its inner checksum list covers the contents,
while the separately downloaded outer list also covers the ZIP itself. Actions artifact ZIPs are the internal build handoff;
the final release assets are the raw files.

The curated source tarball exports committed files from a positive allowlist:
the application's `src/`, CMake files/presets, native packaging, GNOME integration,
licenses, dependency provisioning/verifier, the bounded `tools/proton/` build
inputs and build instructions. Helper tests required by provisioning are included;
application test/lab targets and outputs are excluded. It contains no `work/`,
builds, credentials, agent context, plans/reviews/evidence, lab images or run logs. Its generated `source-info.json` records the original
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
packages. Package builders retain read-only permissions. The nightly coordinator creates
immutable tags and dispatches workflows; only the publisher uploads release assets.

A successful exact-commit Build workflow is required before staging a GitHub
draft. Tags alone do not prove current compilation or package acceptance. Draft
staging uses `gh release create --verify-tag`; the first track is a prerelease.
A rerun can resume a partial draft: existing uploaded bytes must match exactly,
and only missing assets are uploaded. Incomplete/conflicting assets or an
already published release are rejected for inspection; existing assets are
never silently replaced.
For stable and candidate releases, public publication is the last step after
actual package-specific acceptance. Nightlies use the explicit automated class
described above; `--nightly` cannot publish stable or candidate tags.
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
