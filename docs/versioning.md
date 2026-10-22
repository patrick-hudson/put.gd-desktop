# Software versions

put.gd uses calendar versions `YY.MDD.N` (#373). They replaced semantic
versions before the first stable release.

- **`YY.MDD`** is the UTC committer date of the commit, with the month and day
  written as one number: month times 100 plus day. October 22, 2026 is
  `26.1022` and January 5, 2027 is `27.105`. No part has a leading zero.
- **`N`** is the commit's full-history count (`git rev-list --count`). It grows
  with every commit on a branch, so it orders commits made on the same day.

Both come from the commit itself, never from the clock. Building the same commit
on any machine, on any day, gives the same version. Every build carries the
version, its Git commit and the working-tree state:

| Build | Version | Package version |
| --- | --- | --- |
| Untagged (development, CI, local) | `26.1022.942-dev+g0123456789ab` | `26.1022.942~dev+g0123456789ab` |
| Untagged, uncommitted changes | `26.1022.942-dev+g0123456789ab.dirty` | `…~dev+g0123456789ab.dirty` |
| Nightly tag `v26.1022.942-nightly` | `26.1022.942-nightly` | `26.1022.942~nightly` |
| Stable tag `v26.1101.960` | `26.1101.960` | `26.1101.960` |

Each commit has a different version, including documentation-only and merge
commits. Run the ordinary build after committing: it refreshes identity even when
no C++ file changed. It does not require clearing the cache or configuring again.
Nothing is bumped by hand: `project(putgd ...)` in `CMakeLists.txt` carries no
version, and neither a stable release nor a hotfix changes a file to start the
next version. See [releases](release.md).

The date is UTC: a commit made at 23:39 on October 21 in Chicago (04:39 UTC) is
`26.1022`. For a GitHub merge, the committer date is the moment of the merge, so
a nightly cut the next morning from that merge carries the merge's date, not the
build's.

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

A release build requires the explicit CMake option `PUTGD_RELEASE_TAG`. A tag
is either:

- `vYY.MDD.N` for a stable release, or
- `vYY.MDD.N-nightly` for a nightly.

It must equal the version of the checked-out commit, as above. CMake and
`tools/releases/verify_tag.py` derive it the same way and reject:

- any other date or count, including the committer's local date;
- leading zeros, such as `v26.01022.942` or `v26.1022.0942`;
- dates that don't exist, such as `v26.229.942` in a non-leap year;
- the release-candidate forms, `-rc` and `-rc.N`, and a numbered `-nightly.N`;
- a shallow clone, whose count would be wrong;
- a dirty tree, a tag on another commit, and an archive source.

These fail configuration and every later identity refresh, rather than
relabeling the binary. Both annotated and lightweight tags work. Tags don't
change ordinary development builds by themselves.

**Tags cut before #373** keep their tags and stay readable everywhere a version
is compared. The updater, the release tools, the signer and the changelog all
read them:

- `v0.1.1-nightly.N` (and, before 2026-10-18, `v0.1.1-dev.COUNT.UTC+gSHORT`);
- the unpublished `v0.1.1-rc.1` draft.

Nothing creates them any more. Every calendar version sorts after every one of
them.

After committing the intended release source, an example nightly build is:

```sh
tag=v$(python3 -c 'import subprocess, sys; sys.path.insert(0, "tools/releases")
from release_identity import calendar_base
time = int(subprocess.check_output(["git", "show", "-s", "--format=%ct", "HEAD"]))
count = int(subprocess.check_output(["git", "rev-list", "--count", "HEAD"]))
print(calendar_base(time, count))')-nightly
git tag "$tag"
cmake --preset release -DPUTGD_RELEASE_TAG="$tag"
cmake --build --preset release
cpack --config build/release/CPackConfig.cmake
```

If the tag doesn't match, configuration names the one that does: "Release tag
must be v26.1022.942 or v26.1022.942-nightly".

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

Debian and RPM turn the version's `-` into `~`, so a nightly or development
build sorts before the stable release of the same `YY.MDD.N`. Package release
`1` is used, and there is no epoch.

**Order, as dpkg, rpm and put.gd's updater compare it:**

- every pre-#373 version comes first (`0.1.1~dev…`, `0.1.1~nightly.N`,
  `0.1.1~rc.N`, `0.1.1`);
- then calendar versions, by date, then count. The date is a number, so
  `26.105` (January 5) comes before `26.1022` (October 22);
- on one `YY.MDD.N`: `~dev+g…` before `~nightly` before the stable release.

So the first calendar nightly upgrades every installed `0.1.1~nightly.N` with
apt and dnf, without an epoch. `tests/releases/test_verify_tag.py` checks the
whole sequence with real `dpkg --compare-versions` and `rpm.vercmp`. In the lab, the published `0.1.1-nightly.925` upgraded through dnf on Fedora 44
and apt on Mint 22.3 to a calendar nightly from `pkg.put.gd/lab`. The AppImage
updated in place, from an old-format image with appimageupdatetool and between
calendar versions in-app ([evidence](evidence/2026-10-22-calver-373/README.md)).

**Order across channels:**

- Because versions sort by date, a nightly is usually newer than the latest
  stable release, and moving from nightly to stable is a downgrade. put.gd's
  channel switch asks for it explicitly ([updates.md](updates.md)).
- Main's merge commit counts the commits it carries, so a stable release's `N`
  is a little higher than the `N` of the nightly it was promoted from.
- On the stable release's own UTC day, a later nightly can therefore sort just
  below that stable release. From the next day on, the date decides.
- The same `YY.MDD.N` on `main` and on `nightly` would name two commits. The
  suffix keeps the tags and package versions apart, and the nightly sorts lower.

SemVer's `+g...` metadata distinguishes development builds but doesn't affect
precedence. Shallow history changes the count, so use full-history clones; CI
checks out full history. Keep the full commit as the authority: version equality
alone does not prove binary equality, so preserve the compiled build manifest
with every build artifact.

## Building a source archive

Source archives have no `.git`. Supply the exact commit, tree, commit count and
committer time (Unix seconds) from the repository that produced the archive. This is explicit provenance supplied by
the builder, recorded as `sourceProvenance: "supplied"` with `dirty: null`; it is
not claimed as locally observed clean Git state. Archive builds keep the `-dev`
label and reject `PUTGD_RELEASE_TAG`; tagged release publishing uses a full Git
checkout instead of an archive assertion.
The four values must be supplied together, and hashes must be full lowercase Git
object IDs. The version is the commit's calendar version with `-dev+g…`, the
same as a Git checkout of that commit. A Git checkout rejects archive overrides.

```sh
revision=$(git rev-parse HEAD)
source_tree=$(git rev-parse 'HEAD^{tree}')
revision_count=$(git rev-list --count HEAD)
revision_time=$(git show -s --format=%ct HEAD)
git archive --format=tar "$revision" > /tmp/putgd-source.tar
mkdir -p /tmp/putgd-source
tar -xf /tmp/putgd-source.tar -C /tmp/putgd-source
cmake -S /tmp/putgd-source -B /tmp/putgd-source-build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/absolute/path/to/sdk \
  -DPUTGD_AWS_RUNTIME_DIR=/absolute/path/to/sdk/lib \
  -DPUTGD_AWS_SOURCE_DIR=/absolute/path/to/pinned/aws-sdk-cpp \
  -DPUTGD_SOURCE_COMMIT="$revision" -DPUTGD_SOURCE_TREE="$source_tree" \
  -DPUTGD_SOURCE_COMMIT_COUNT="$revision_count" -DPUTGD_SOURCE_COMMIT_TIME="$revision_time"
cmake --build /tmp/putgd-source-build --parallel 2
```

Transfer those values and the archive checksum together when building in a VM.
The overrides assert the archive's origin; they cannot detect edits made after
extraction. Recreate the archive from the intended commit for repeatable builds.
An archive with no supplied provenance builds as `0.0.0-dev.unknown`, with empty
commit/tree and unknown dirty state. It never silently presents itself as a clean
release. Ordinary `git archive HEAD` exports only committed files, so commit the
intended code first rather than expecting it to include local edits.

## Verification

`version_contract` creates small temporary Git repositories and uses normal
CMake/Ninja builds. It checks all three executable versions and embedded manifests
immediately after a commit-only rebuild. It also covers release binaries from different commits, explicit clean stable
and nightly calendar tags, malformed tags, a tag with the local date or another
count, a tag on another commit, dirty/archive release rejection, package ordering, dirty state,
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
