# Linux releases

Release builds use C++20, Qt 6, the pinned official AWS SDK, CMake presets, Ninja
and native packaging tools. The application is MIT licensed. The first public
track is a prerelease. Flatpak is separate work in GitHub issue #14; its selected
host capture companion must preserve the current hotkey workflow.

## Branches and channels

`nightly` is the default branch and the integration trunk. Every feature, fix,
documentation and tooling pull request targets `nightly`. `main` is the release
branch. It changes only through a promotion pull request from `nightly`, or a
hotfix pull request. Rulesets on both branches require a pull request and the
`CI` check, and block force-pushes and deletion. Nobody pushes to either branch
directly, and published tags are never moved or deleted.

| Channel | Tag | Cut by | Publication |
| --- | --- | --- | --- |
| Nightly | `vBASE-dev.COUNT.UTC+gSHORT` | daily schedule on changed `nightly` | automated, build-tested prerelease |
| Candidate | `vBASE-rc.N` | every push to `main` | draft; published after exact-asset acceptance |
| Stable | `vBASE` | manual Release run on a candidate commit | draft; published after exact-asset acceptance |

`BASE` is `project(putgd VERSION ...)`. On `nightly` it is always the version
being prepared, because packages order `BASE~dev` and `BASE~rc` before `BASE`.
`UTC` is the source commit's 14-digit UTC committer time, not the runner clock.
`COUNT` is its full-history commit count; `SHORT` is its first 12 lowercase hash
characters. The tag, base, full commit, tree and count must match the clean source.
Nightlies never replace GitHub's Latest stable release. Every channel publishes
all twelve individual downloads listed below, including both raw DEBs, the RPM
and the exact compiled curated source archive.

### Nightly

`.github/workflows/nightly.yml` runs at 05:23 UTC on `nightly` and on demand
(`gh workflow run nightly.yml --ref nightly`). GitHub can delay scheduled work;
this is not a delivery-time guarantee. Its `plan` job derives the nightly tag for
`nightly` HEAD. When that exact source is already a complete, verified public
nightly, the run stops there: nothing is built or published. Otherwise it
creates the immutable tag, runs the full CI and package matrix on that tag in
the same run, and publishes the collected assets as a prerelease. A tag, draft
or failed run alone is not a published nightly, so unchanged source whose last
attempt failed is retried.

`plan` refuses to start once a stable `vBASE` tag exists for the current base.
Merge the pull request that starts the next version first (see below).

Nightly notes state automated coverage: Linux test suites, Windows portable
compilation, package audits and the extracted-source production build. They are
development releases, not new graphical, memory or live-S3 certification.

### Candidate and stable

1. Run `python3 tools/releases/promote.py` to preview, then `--open` to create
   the pull request from `nightly` into `main` titled `release: vBASE`. Merge it
   with a merge commit once `CI` passes, so every `nightly` commit keeps its SHA.
2. The push to `main` starts `.github/workflows/release.yml`. It tags the next
   `vBASE-rc.N`, or reuses the candidate already on that commit when rerun. It
   runs full CI and packaging on the tag and stages a verified draft prerelease.
   The run summary lists the next steps.
3. Run exact-package acceptance on the draft's assets, as described under
   [Acceptance boundary](#acceptance-boundary). Publish the candidate with the
   acceptance receipt:

   ```sh
   git checkout vBASE-rc.N
   gh release download vBASE-rc.N -D build/publish
   python3 tools/releases/publish.py --tag vBASE-rc.N --directory build/publish --publish --acceptance /private/current-package-acceptance.json
   ```

   If acceptance fails, fix it on `nightly` and promote again. The next push to
   `main` cuts `rc.N+1`; nothing is retagged.
4. Cut stable on the accepted candidate's commit:
   `gh workflow run release.yml --ref main -f channel=stable`. It requires a
   candidate tag on `main` HEAD, tags `vBASE`, rebuilds every asset with the
   stable identity and stages a draft. Accept and publish it the same way; a
   published stable becomes GitHub's Latest release.
5. Open a pull request into `nightly` that raises `project(putgd VERSION ...)`
   to the next version, normally the next patch. Nightlies resume once it merges.

A hotfix is a pull request into `main` that also raises the patch version. Its
merge cuts that version's first candidate; follow steps 3 to 5. Then merge `main`
into `nightly` through a pull request so the fix is not lost at the next
promotion. Promotion does not need `nightly` to contain `main`'s merge commits.

### Recovery

Use GitHub's "Re-run failed jobs" on the original run. A rerun reuses the
successful package and collection artifacts of that run, and the publisher
resumes a partial draft: matching uploaded bytes are kept and only missing
assets are uploaded. Conflicting bytes, an ambiguous draft or an already
published release stop with an error for inspection; assets are never silently
replaced. A new run for the same source rebuilds packages, so it cannot resume a
partially uploaded draft from another run. Inspect that draft, and delete it only
when nothing has been published, before retrying.

Plans, tag decisions and publication receipts are retained as Actions artifacts
for 90 days, package handoffs for 30 days and the collected `release-assets` for
14 days. The public `release-builds.json`, checksums and release notes remain the
source of truth after Actions artifacts expire.

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
qualification. Windows CI compilation is future-platform evidence, not a
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

`.github/workflows/package.yml` builds and audits the three native packages and
the AppImage. CI calls it on pull requests that touch packaging, CMake, Proton
or CI inputs, on every push to `nightly`, and from the nightly and release runs
with an exact tag. Builders are ABI-separated, keep read-only permissions and
use the normal presets rather than an alternate build system.

With a tag, the `collect` job requires the three native assets and the AppImage
from one clean tagged commit/tree/count/version. It recalculates each artifact's
SHA256 against its build receipt and assigns distro-specific filenames so the
two DEBs cannot overwrite each other. It uploads the verified set as the
`release-assets` artifact. Only the nightly `publish` job and the release
`draft` job write releases. Both run after every CI and package job in the same
run passed, and both record that run in the release notes and their receipt.

Draft staging uses `gh release create --verify-tag`. A rerun can resume a
partial draft: existing uploaded bytes must match exactly, and only missing
assets are uploaded. Incomplete or conflicting assets and an already published
release are rejected for inspection; existing assets are never silently replaced.
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
