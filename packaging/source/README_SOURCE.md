# Curated source archive

The release exporter generates a root `README_SOURCE.md` with exact provenance
arguments and a `source-info.json` manifest. It exports committed Git blobs through
a positive allowlist: production source, CMake inputs, Linux packaging, GNOME
integration, licenses/notices, the build guide, and SDK verification/provisioning
scripts. It excludes working-tree modifications, untracked/ignored files, agent
context, test/lab artifacts, plans, evidence, and credentials.

Use the normal system C++20 toolchain, Qt 6.4.2+, QtKeychain, CMake presets, Ninja,
and the pinned official AWS SDK described in `docs/build.md`. Provision the SDK
separately; no dependencies or substitute compiler/Qt distribution are bundled.

An extracted archive has no Git checkout. Configure it with `BUILD_TESTING=OFF`
and the generated `PUTGD_SOURCE_COMMIT`, `PUTGD_SOURCE_TREE`,
`PUTGD_SOURCE_COMMIT_COUNT` and `PUTGD_SOURCE_COMMIT_TIME` arguments. Leave lab/physical benchmark options off.
For an existing SDK, also provide `CMAKE_PREFIX_PATH`, `PUTGD_AWS_RUNTIME_DIR`,
and `PUTGD_AWS_SOURCE_DIR`, including the verified source/component licenses.

These are supplied provenance assertions. Local archive builds receive the
normal development identity, not an official binary release label. Do not pass
`PUTGD_RELEASE_TAG`; official binaries come from the verified exact clean Git tag.
`source-info.json` retains the original release or explicitly identified prototype
identity. A prototype's dirty modifications are never exported as committed source.

The manifest hashes every other archived file; the external receipt pins the
manifest itself, every member, and the compressed archive. Archive metadata,
ownership, modes, and timestamps are deterministic for a given identity and tree.
