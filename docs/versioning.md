# Software versions

Every build carries a release base, Git commit and working-tree state. The current
base is `0.1.1`. Development builds look like
`0.1.1-dev.123+g0123456789ab`; an uncommitted checkout adds `.dirty`.
Each commit has a different Git suffix, including documentation-only and merge
commits. Run the ordinary build after committing: it refreshes identity even when
no C++ file changed. It does not require clearing the cache or configuring again.

The base in `project(... VERSION ...)` follows semantic versioning. Increment its
patch for a compatible bug-fix release, minor for compatible functionality and
major for incompatible changes. Do not bump it merely to allocate a commit ID;
the generated commit identity does that. These builds remain development versions;
this change does not create release tags or rewrite old commits.

Inspect the compiled build through:

```sh
./build/dev/putgd --version
./build/dev/putgd-cli --version
./build/dev/putgd-ui --version
./build/dev/putgd-cli diagnostics
cat build/dev/build-identity/build-info.json
```

All three version commands work without a graphical session or running resident.
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

## Packages and ordering

CPack and `cmake --install` use the built manifest and check `putgd`, `putgd-cli`
and `putgd-ui`, plus `putgd-kde-region` when that target is built, before installing
files. The selector version command also works without a display.
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

Debian uses `0.1.1~dev.123+g0123456789ab`, with the same optional `.dirty` suffix.
The tilde places a development package before final `0.1.1`; the count orders
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
not claimed as locally observed clean Git state. All builds keep the `-dev` label.
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
immediately after a commit-only rebuild. It also covers dirty state,
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
