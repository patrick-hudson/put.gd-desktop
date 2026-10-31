# Proton helper corresponding-source delivery

SOURCES.json pins the six accompanying packets and their two-part transport.
Verify both part hashes, concatenate in numeric order, verify the combined hash,
then extract and verify every packet against the catalog. The updated transport
has its own permanent release URLs; earlier published packets remain available.

The current helper-rebuild packet supplies the patched SDK/account and approved
GPL crypto 2.2.0 source graph plus locked dependencies for Bun 1.4.4.
The application source archive supplies the applicationSourceOverlay files listed
in SOURCES.json. Verify their hashes before copying them over build-source/.
The overlay includes package.json and package-lock.json; use the matching packet's
dependencies. The older Bun 1.4.2 dependency tree is not the current rebuild input.

Obtain the official Linux x64 Bun 1.4.4 release archive identified in
tools/proton/runtime-lock.json. Verify its SHA-256, extract the executable and
verify bunSha256. Set proton_bun to that executable's absolute path:

```sh
cd build-source
"$proton_bun" build --compile --target=bun-linux-x64 --minify runtime.ts --outfile=putgd-proton-runtime.new --compile-executable-path="$proton_bun"
./putgd-proton-runtime.new --version
"$proton_bun" test protocol.test.ts runtime.test.ts crypto-cache.test.ts progress.test.ts
```

Retain the .new output basename: Bun embeds it. Compare its SHA-256 with
helperSha256 in SOURCES.json before renaming. A provision from the current
application source runs the same compile operation, compiled stdio checks and
large-output regression. CI independently reproduces the helper hash with
audit_redistribution.py. No Proton account is needed for these checks.

A recipient-modified Bun may also be selected with compile-executable-path.
To install a modified helper, update manifest.sha256 and rebuild put.gd from
source with that manifest; CMake binds its hash. A modified artifact is not
expected to match the official production helper hash.

The Bun 1.4.4 packet preserves its complete Bun source, exact WebKit Git source
objects, current native recipes/patches, Cargo/npm source inputs, ICU 78.3 source,
Rust nightly 2026-09-15 identity metadata and preferred standard-library source.
Its manifests bind every input. Run assemble_offline.py --destination NEW_DIR
with Python 3.12+ and Git to verify/extract sources, apply native patches and
prepare offline Cargo/npm inputs. Keep the packet in place because the prepared
tree links to its verified archives. Consult the included REBUILD.md and Bun's
upstream build requirements for a native build with local JavaScriptCore.

The native rebuild supplement and OpenPGP/Argon2 proofs retain their original
bytes and historical scope. Its Bun 1.4.2 compiler selection and modifications
must not be mistaken for current Bun 1.4.4 native verification. A fresh native
modification/relink check is supplied by tools/proton/native_relink/ in the
current application source. SOURCES.json binds its result to the exact current
helper and records desktop/resource/package acceptance separately.

The unchanged OpenPGP preferred source uses commit
36198c3369ab8d4a02d70372d62f1aa8bc247dcb. Restore its supplied offline npm cache,
apply OPENPGP-REBUILD.patch, set SOURCE_DATE_EPOCH=1785456000, then use its Rollup
build. The original node library's byte identity and modified crypto roundtrip
proofs are historical and remain unchanged. The native supplement's corrected
Argon2 scalar/SIMD replay is authoritative over the older upstream script.
The crypto source/provenance packet contains complete WebPackages 2.2.0 preferred source and its
matching registry input, plus the historical provenance packet. The optional SDK
search vendor WASM is retained as an unbundled build input; a compiler-metafile
guard excludes the search engine and BoltFFI runtime from the shipping helper.

Deliver the complete source transport and current application source archive
together. Installed notices alone do not establish source publication, legal
approval, live-service acceptance or desktop acceptance.
