# Software versions

Every build carries a release base, Git commit and working-tree state. The base
is `project(putgd VERSION ...)` in `CMakeLists.txt`, for example `0.1.1`.
Development builds look like `0.1.1-dev.123+g0123456789ab`; an uncommitted
checkout adds `.dirty`.
Each commit has a different Git suffix, including documentation-only and merge
commits. Run the ordinary build after committing: it refreshes identity even when
no C++ file changed. It does not require clearing the cache or configuring again.

The base in `project(... VERSION ...)` follows semantic versioning. Increment its
patch for a compatible bug-fix release, minor for compatible functionality and
major for incompatible changes. Do not bump it merely to allocate a commit ID;
the generated commit identity does that. A build remains a development version
unless the builder explicitly selects a verified release tag.

On `nightly` the base is always the next release, because package versions order
`BASE~dev` and `BASE~rc` before `BASE`. Feature pull requests leave it alone.
After a stable `vBASE` is tagged, a pull request into `nightly` raises it, normally
to the next patch; nightly publication refuses to run until that change merges.
A deliberate minor or major bump is its own one-line pull request into `nightly`.
A hotfix pull request into `main` raises the patch itself. See [releases](release.md).

Inspect the compiled build through:

```sh
./build/dev/putgd --version
./build/dev/putgd-cli --version
./build/dev/putgd-ui --version
./build/dev/putgd --version-json
./build/dev/putgd-cli diagnostics
cat build/dev/build-identity/build-info.json
```

All product executables support `--version` and `--version-json` without a
graphical session, control pipes or running resident. The JSON command prints the
embedded build manifest, including the full commit, without starting services.
The tray's **About put.gd** action shows the version; its details contain the
manifest. About is modeless, so it leaves capture input available. Opening it again
reuses the visible dialog. The UI helper exits when its last primary window closes;
the resident tray remains available. About reports the connected resident's build.
Diagnostics add a `build` object without changing the existing protocol's
`schemaVersion`. The installed manifest is `share/putgd/build-info.json` under the
installation prefix, normally `/usr/share/putgd/build-info.json` on Linux.

The manifest records the full commit, its committed tree, commit count, version,
package version, provenance and dirty state. `gitTree` identifies the committed
baseline, not the effective tree of a dirty build. Dirty state includes tracked
edits and non-ignored untracked files. Ignored build outputs and local credentials
do not enter the identity. No source contents, paths, user identity or timestamps
are exported. A dirty version is not a unique fingerprint of every working edit;
retain binary/archive SHA-256 values for experiments as before.

## Tagged releases

A release build requires the explicit CMake option `PUTGD_RELEASE_TAG`. Supported
tags are `vMAJOR.MINOR.PATCH` for stable releases and
`vMAJOR.MINOR.PATCH-rc.N` for release candidates, and
`vMAJOR.MINOR.PATCH-nightly.N` for nightlies, for example `v0.1.1-nightly.312`.
A nightly's `N` must equal the full-history commit count of the tagged commit, so
it grows with every commit on `nightly` and needs no date or hash to stay unique.
The release title spells it out ("put.gd 0.1.1 nightly 312"), and the notes and
`--version-json` carry the full commit. Nightlies published before 2026-10-18 used
`vBASE-dev.COUNT.UTC+gSHORT`; they keep their tags and still upgrade cleanly.
The ordinary untagged development identity remains unchanged. Components have no leading zeros;
`N` is positive. The tag's base must match `project(... VERSION ...)`, the working
tree must be clean, and that exact tag must resolve to the checked-out commit.
Both annotated and lightweight tags work. A missing tag, another commit's tag,
a dirty tree or an archive source fails configuration and subsequent identity
refresh rather than relabeling the binary. Tags do not change ordinary development
builds automatically.

After committing the intended release source, an example prerelease build is:

```sh
git tag -a v0.1.1-rc.1 -m "put.gd 0.1.1 release candidate 1"
cmake --preset release -DPUTGD_RELEASE_TAG=v0.1.1-rc.1
cmake --build --preset release
cpack --config build/release/CPackConfig.cmake
```

The compiled version is `0.1.1-rc.1`; a stable tag produces `0.1.1`.
The full commit, committed tree, commit count and observed clean Git provenance
remain in the manifest. GitHub release assets must retain that manifest and their
checksums; the release name alone does not identify their binary bytes. Changing
or advancing Git while this option remains set fails the next build. To return
that build directory to development numbering, configure with
`-DPUTGD_RELEASE_TAG=` and rebuild all product executables. Release publishing
and package acceptance are described in the release workflow documentation.

## Packages and ordering

CPack and `cmake --install` use the built manifest and check `putgd`, `putgd-cli`
and `putgd-ui`, plus `putgd-kde-region` when that target is built, before installing
files. They compare both the printable version and the full embedded manifest,
so a reused release label cannot hide executables compiled from different commits.
The selector identity commands also work without a display.
Advancing Git without rebuilding leaves the old binaries and their old package
identity intact. Project runtime assets, including the GNOME extension, and
packaged metadata, notices and licenses are copied into the build directory during
configuration. Installation uses these snapshots, so an unbuilt checkout change
does not substitute newer assets into an older package. A normal build refreshes
changed inputs through CMake's configuration dependencies.
Rebuilding only one executable can leave a mixed build; packaging
rejects it and asks for all product executables to be built. Rebuilding the resident and CLI
while leaving the UI helper at an older version is also rejected. On KDE builds,
updating those three while leaving the region selector stale is rejected before
any installation writes. Configuring
after Git changes and then installing without rebuilding also fails before
writing any installed files.
Do not copy a generated manifest between
build directories. The manifest and generated C++ object come from one CMake target.
An unchanged identity does not rewrite the object source or recompile it.

```sh
cmake --preset release
cmake --build --preset release
cpack --config build/release/CPackConfig.cmake
dpkg-deb --field build/release/putgd_*.deb Version
```

Debian and RPM use `0.1.1~dev.123+g0123456789ab`, with the same optional `.dirty`
suffix. Tagged candidates map to `0.1.1~rc.1`; stable releases map to `0.1.1`.
Tagged nightlies map to `0.1.1~nightly.N`; the commit count orders successive
nightlies on the append-only branch. Debian and RPM order untagged `~dev` builds,
then the earlier `~dev.COUNT.UTC+gSHORT` nightlies, then `~nightly`, `~rc` and the
stable base. Actual Debian and RPM comparisons cover that whole sequence.
A same-base stable-to-nightly switch is an explicit package downgrade; bump the next
development base deliberately after a stable release. No updater or automatic downgrade
policy is implemented here. RPM uses package release `1`. The tilde places development packages and release
candidates before final `0.1.1`, and numeric candidate components order
`rc.2` before `rc.10`; the count orders
ordinary successive commits in a full-history lineage. SemVer's `+g...` metadata
distinguishes builds but does not affect SemVer precedence. Hashes do not give a
chronological order across branches, and shallow history can change the count.
Use full-history clones for comparable development numbering and retain the full
commit as the authority. CI checks out full history and preserves the compiled
build manifest with its build artifacts. Version equality alone does not prove
binary equality.

## Building a source archive

Source archives have no `.git`. Supply the exact commit, tree and commit count from
the repository that produced the archive. This is explicit provenance supplied by
the builder, recorded as `sourceProvenance: "supplied"` with `dirty: null`; it is
not claimed as locally observed clean Git state. Archive builds keep the `-dev`
label and reject `PUTGD_RELEASE_TAG`; tagged release publishing uses a full Git
checkout instead of an archive assertion.
The three values must be supplied together, and hashes must be full lowercase Git
object IDs. A Git checkout rejects archive overrides.

```sh
revision=$(git rev-parse HEAD)
source_tree=$(git rev-parse 'HEAD^{tree}')
revision_count=$(git rev-list --count HEAD)
git archive --format=tar "$revision" > /tmp/putgd-source.tar
mkdir -p /tmp/putgd-source
tar -xf /tmp/putgd-source.tar -C /tmp/putgd-source
cmake -S /tmp/putgd-source -B /tmp/putgd-source-build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/absolute/path/to/sdk \
  -DPUTGD_AWS_RUNTIME_DIR=/absolute/path/to/sdk/lib \
  -DPUTGD_AWS_SOURCE_DIR=/absolute/path/to/pinned/aws-sdk-cpp \
  -DPUTGD_SOURCE_COMMIT="$revision" -DPUTGD_SOURCE_TREE="$source_tree" \
  -DPUTGD_SOURCE_COMMIT_COUNT="$revision_count"
cmake --build /tmp/putgd-source-build --parallel 2
```

Transfer those values and the archive checksum together when building in a VM.
The overrides assert the archive's origin; they cannot detect edits made after
extraction. Recreate the archive from the intended commit for repeatable builds.
An archive with no supplied provenance builds as `0.1.1-dev.unknown`, with empty
commit/tree and unknown dirty state. It never silently presents itself as a clean
release. Ordinary `git archive HEAD` exports only committed files, so commit the
intended code first rather than expecting it to include local edits.

## Verification

`version_contract` creates small temporary Git repositories and uses normal
CMake/Ninja builds. It checks all three executable versions and embedded manifests
immediately after a commit-only rebuild. It also covers same-version release binaries from different commits, explicit
clean stable/RC tags, malformed tags, mismatched base and
commit, dirty/archive release rejection, package ordering, dirty state,
embedded/installed manifest agreement, package identity and runtime asset snapshots
after Git advances,
mixed executable rejection (including stale UI and KDE region helpers in ordinary
install and CPack), absent-KDE builds, and supplied/unknown archive provenance. Every run
keeps its isolated fixture under the test build's `Testing/version-contract-*`
directory and prints its path. Expected install/package rejection logs remain
in that fixture directory. `version_integration` checks the production
executables without a display and the resident diagnostics object. `gui_version`
checks About's version, nonmodal behavior, reuse and close/teardown cleanup
offscreen; native visual acceptance is separate.

```sh
ctest --preset dev -R '^(version_contract|version_integration|gui_version)$' \
  --output-junit build/dev/Testing/version-results.xml
```

Keep version/manifests and binary hashes alongside desktop and memory evidence.
Do not relabel an older measurement with a later commit's version. Versioning does
not replace the S3 module's existing source/toolchain/transport compatibility check.
