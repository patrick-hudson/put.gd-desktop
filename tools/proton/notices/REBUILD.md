# Proton helper corresponding-source delivery

The helper uses the official JS SDK and an explicit GPL crypto source snapshot.
The 2.0.0 npm crypto archive is a build bootstrap only. The selected runtime source
is WebPackages 956a713c0823b365cc512018974eb4fcba47c9dd, version 2.0.1. Its source,
GPL grant, predecessor notices and source hashes accompany delivery.

SOURCES.json identifies accompanying source artifacts by exact hash. Ship those
artifacts beside the binary packages and retain availability. The installed
notices/catalog alone do not establish that source has been delivered.

The helper source archive contains build-source, patched public SDK/account
inputs and the fixed dependency graph. Use an existing compatible Bun executable (set proton_bun to its absolute path):

```sh
cd build-source
"$proton_bun" build --compile --target=bun-linux-x64 --minify runtime.ts --outfile=putgd-proton-runtime.new --compile-executable-path="$proton_bun"
./putgd-proton-runtime.new --version
"$proton_bun" test protocol.test.ts runtime.test.ts crypto-cache.test.ts progress.test.ts
```

The compile-executable-path argument selects a recipient-modified/relinked Bun;
no runtime download or vendor signing key is required for source reconstruction.
To install a changed artifact, update manifest.sha256 and build put.gd from its
source with the updated manifest. CMake binds the resulting executable hash.
The whole Qt application rebuild remains separate package acceptance work.

OpenPGP preferred source at 36198c3369ab8d4a02d70372d62f1aa8bc247dcb includes its
package-lock and Rollup config. Restore its supplied npm cache and run npm ci
--offline --ignore-scripts with Node 22/npm already installed. Apply the supplied
OPENPGP-REBUILD.patch and set SOURCE_DATE_EPOCH=1785456000 for the July 31 banner,
then run npm run build -- --config-build-only=node. The selected node library
reproduced byte-for-byte. A modified preferred-source library also passed a
compiled encryption/decryption roundtrip and helper checks. Copy its node outputs
into build-source/node_modules/openpgp/dist/node before rebuilding the helper.
The date-banner patch does not change algorithms.

The full Bun source packet contains Bun, complete pinned WebKit Git objects,
TinyCC/native/Cargo/npm inputs, exact ICU source, patches, offline reconstruction
and a native compiler preflight. Its REBUILD.md gives the modified-JSC release-local
build route. Offline source reconstruction and native identity checks passed.
Actual Bun/JSC/TinyCC compile/relink remains unverified: this host lacks LLVM 21.1
and pinned Rust nightly 2026-07-20, and project instructions prohibit assembling a
substitute toolchain. The wrapper never installs one. Source delivery and a helper
rebuild do not prove native Bun relinking.

The OpenPGP packet also supplies complete Argon2 C/WASM preferred source,
upstream build script, exact prebundled npm inputs and preferred source/notices
for packages whose npm archives omit it. Argon2 WASM recompilation is unverified;
the preserved upstream script uses an unpinned Emscripten container by default.
Do not treat the successful JavaScript rebuild as a WASM compiler proof.

The build-controls packet contains put.gd's Proton helper sources, locks,
provisioner, audit/tests, original MIT license and CMake integration. SOURCES.json
is delivered separately to avoid an archive/catalog self-hash cycle. These are
component materials; the full put.gd application source/rebuild handoff is P4.
The helper-source archive already contains the selected patched source graph;
rebuilding it does not require contacting a Proton account or any data service.
