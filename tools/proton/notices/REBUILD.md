# Proton helper corresponding-source delivery

The helper uses the official JS SDK and an explicit GPL crypto source snapshot.
The 2.0.0 npm crypto archive is a build bootstrap only. The selected runtime source
is WebPackages 956a713c0823b365cc512018974eb4fcba47c9dd, version 2.0.1. Its source,
GPL grant, predecessor notices and source hashes accompany delivery.

SOURCES.json identifies accompanying source artifacts by exact hash. The six
original packets travel inside `putgd-proton-corresponding-source.tar.gz`, split
into `.part1` and `.part2` to respect the hosting service's per-file limit. Verify
the part hashes, concatenate in numeric order, then verify the combined hash before
extracting and verifying each original packet. The catalog's `delivery` section
records every size, hash and public source URL. Binary releases deliver both
parts and the current catalog beside their installers; retain availability. The installed
notices/catalog alone do not establish that source has been delivered.

The six sealed packets retain their original bytes. The current application source
archive supplies the required `applicationSourceOverlay` listed in SOURCES.json.
For issue #40, copy its `tools/proton/runtime.ts` and `runtime.test.ts` over the
matching files in the extracted `build-source/` directory. Verify each overlay
file against the catalog's SHA-256 before compiling. The catalog pins repository
commit `1016b1bb0348cd01b044dae9ae50fc63a3ed27d2` as another source of those
same files. This overlay returns the connected account email from existing SDK
metadata; SDK, crypto, Bun and dependency inputs remain unchanged. The current
helper reproduced exactly from the original source packet plus these two files.
The application source archive and original source packets must travel together.

The helper source archive contains build-source, patched public SDK/account
inputs and the fixed dependency graph. Use an existing compatible Bun executable (set proton_bun to its absolute path):

```sh
cd build-source
"$proton_bun" build --compile --target=bun-linux-x64 --minify runtime.ts --outfile=putgd-proton-runtime.new --compile-executable-path="$proton_bun"
./putgd-proton-runtime.new --version
"$proton_bun" test protocol.test.ts runtime.test.ts crypto-cache.test.ts progress.test.ts
```

Keep the `.new` output basename shown above: Bun embeds that name in the compiled
executable. Rename it only after comparing its hash to SOURCES.json.

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
TinyCC/native/Cargo/npm inputs, exact ICU source, patches and offline reconstruction.
Use the accompanying putgd-proton-native-rebuild-supplement.tar.gz for the corrected
native recipe. It supersedes the original packet's preflight and toolchain route;
the original sealed sources remain unchanged. Its README.md gives the disposable
image build, numeric-UID staging, once-only source modifications, offline native
build and executable proof commands. No host compiler installation is needed.

The tested native route uses LLVM 21.1.8, Rust nightly 2026-07-20 including rust-src,
local JavaScriptCore and system ICU 74.2 under upstream's release-local profile.
Rustc and Cargo have separate exact source commits; the supplement pins both,
the compiler component archives and the standard-library source lock. It supplies
Rust's preferred standard-library source and its 30 offline vendored packages.
The installed Bun notice aggregate includes their original legal texts as a
labelled build/platform source superset.

Modified Bun Rust, JavaScriptCore C++ and TinyCC C are compiled and linked. The
resulting runtime calls all three source markers; it compiles and executes a
synthetic C function with TinyCC. A helper rebuilt using the explicit native
compile-executable-path passes the existing helper/stdio/stdout checks. The same
compiled helper exposes the three markers and executes the TinyCC fixture in
BUN_BE_BUN mode, establishing which runtime was embedded. Marked executables are
proof artifacts only. The production helper still uses the official Bun binary.
This verifies source modification/relinking, not native byte identity or package
acceptance. Supporting distro package versions and actual toolchain identities
are retained with the proof; the image recipe is not a hermetic distro snapshot.

The OpenPGP packet supplies full Argon2 C/WASM preferred source and prebundled
inputs/notices. The supplement's argon-replay-correction/ builds scalar and SIMD
variants in digest-pinned Emscripten 3.1.45, clears dist/ first, invokes the
upstream script with bash -e and requires both outputs. Use this corrected route:
the old upstream shell command can retain stale scalar bytes after a failure.
Both original and marker-modified C builds pass the existing vectors. Preferred
OpenPGP source rebuilt with the modified WASM passes an actual Argon2 S2K crypto
roundtrip and bidirectional interoperability, including a fixture compiled with
the rebuilt native Bun. The instantiated WASM must expose the C source marker.
The original published WASM compiler is unknown and the bytes differ; no original
WASM byte-identity claim is made. The supplement includes the full source/replay
instructions and portable fixtures; generated fixture libraries do not replace
preferred source in the original packet.

The build-controls packet contains put.gd's Proton helper sources, locks,
provisioner, audit/tests, original MIT license and CMake integration. SOURCES.json
is delivered separately to avoid an archive/catalog self-hash cycle. These are
component materials; the full put.gd application source/rebuild handoff is P4.
The helper-source archive already contains the selected patched source graph;
rebuilding it does not require contacting a Proton account or any data service.
