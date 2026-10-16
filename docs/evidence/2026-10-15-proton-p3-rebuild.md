# Proton P3 rebuild follow-up — 2026-10-15

P3's notice/source/rebuild work is complete. The user approved a narrow exception
to use pinned toolchains in disposable build environments. Both remaining native
rebuild checks pass. The [previous materials receipt](2026-10-15-proton-p3-materials.md)
and its sealed source packets remain unchanged as historical evidence.

The production choice remains the official JavaScript helper, SHA256
`eeffe97c9c7f473895f790434dabafb13a018b8909a2f5215c3b769e7b7bb539`.
No native trial or marked rebuild binary was adopted. No account, credentials,
project S3 path/test/service or full application build was used. The separate
native SDK trial remains clean at `e1f20ef75126ff0cf5a5cf8d48837744757a679a`.

## Native Bun, JavaScriptCore and TinyCC

Exact Bun `744846f844374847c902b5e7fd59b4342a51ef99`, WebKit
`2e2aa2290fac856d6f451ceacb58f7f5b44dd057` and TinyCC
`05f0fafaa3be31e31d7b4b5c17dc60f62c991171` were rebuilt from source.
Three harmless source changes add marker functions to Bun Rust, JavaScriptCore
C++ and TinyCC C. The Rust revision handler calls both linked C/C++ functions.
The resulting runtime reports all three markers and compiles/executes a synthetic
C function with TinyCC.

The existing helper source graph was compiled with that rebuilt runtime selected
explicitly by `--compile-executable-path`. All 41 helper tests, eleven no-account
stdio cases and the complete 1,048,337-byte stdout regression pass. The compiled
helper itself reports all three native markers and runs TinyCC under Bun's
`BUN_BE_BUN=1` mode. This establishes which native runtime was embedded.

- Source-built Bun SHA256: `b1f7f292666e5cb85a1c6fb46bbaec17868c63a8e913f741eea313c9845b401a`.
- Marked proof helper SHA256: `5ad6597e8225f39a693595fdf86f516b88dfc76cc3931a9364e421b482fbf028`.
- Native compiled crypto fixture SHA256: `9cc676dd5008a2bbeac753235eea9cb5736511ca2176adf5b0dc1e083320de80`.

These binaries are proof artifacts. The route uses upstream `release-local`,
local JSC and system ICU 74.2, rather than reproducing the original prebuilt
JSC/ICU artifact. Native byte identity is not claimed. This is not a performance,
service, GUI or final shipping-package acceptance result.

LLVM 21.1.8 and Rust nightly 2026-07-20 stayed inside the disposable environment.
The actual build began in a three-component toolchain image and received the
exact hash-verified rust-src component inside that container. The corrected
four-component image was freshly built and independently identity-checked; it
is the recipient recipe. Both direct compiler hashes/commits and standard-library
source identity are retained. No host toolchain was installed.

The container had no network or host mounts, dropped all capabilities, used
no-new-privileges and UID1000, and was limited to six CPUs/10GiB/no swap/2048PIDs.
Rust used two jobs. C++ began at two jobs and resumed at four, then six after
Rust completed. Both intentional scheduler interruptions are retained separately
from failures. The resolved Docker socket was16 UTF-8 bytes; no app socket was
created.

## Argon2 WASM and exercised crypto

The full preferred C source at `6fc76bcf69fbe07b5ec532a7912772d28f1ea43a`
compiled scalar and SIMD variants with digest-pinned Emscripten 3.1.45. Published,
rebuilt and harmless source-marker-modified artifacts each passed eight vector
checks: RFC9106, longer segments,64MiB recommended settings and128MiB memory growth.
The rebuilt bytes differ from npm's original WASM; its original compiler was not
recorded upstream, so original byte identity is not inferred.

Preferred OpenPGP source was freshly rebuilt offline with the marker-modified
WASM. A real Argon2 S2K packet, rebuilt roundtrip and bidirectional decryption with
the original library pass under Node, the official Bun-compiled fixture and the
source-built native Bun-compiled fixture. The actual instantiated WASM exposes
the C source marker; this is not a byte search or export-only test.

Independent review found a replay defect in upstream's shell script: scalar
failure could leave stale output while SIMD success returned zero. The corrected
route clears dist/, uses `bash -e` and requires both outputs. Four clean real
builds pass all 16 replay vector checks and exactly reproduce the earlier rebuilt
hashes. Separate fake-compiler fixtures prove scalar failure and omitted output
are rejected; they are not crypto acceptance evidence. The original packet and
its 102-file checksum manifest remain sealed; the correction is a separate packet.

## Source delivery and final checks

[The current catalog](../../tools/proton/notices/SOURCES.json) lists six exact
source/build-input assets. Original large source packets remain unchanged. The
new supplement supplies corrected Cargo/std-source preflight, source patches,
pinned compiler recipe, full Rust preferred standard-library source, offline
vendor integration, bootstrap Bun, Argon clean replay and portable proof fixtures.
Generated fixture libraries are labelled outputs and do not replace preferred
source. Refreshed build controls match the current checkout and omit the separate
catalog to avoid a self-hash cycle. The supplement has 113 indexed files plus its
manifest; all match the archive.

The Bun notice aggregate additionally preserves 77 original legal source files
from the pinned standard-library archive and its 30 vendor directories. Each
source path/hash is retained. This is a build/platform source superset; it does
not add 77 claimed unique licenses or establish exact ELF linkage. The original
23,143 source mappings/6,105 unique-text corpus remains preserved.

The official helper was reprovisioned fully offline. Its fingerprint is unchanged,
with 41 tests, eleven compiled stdio checks and the full stdout drain passing.
All 7 crypto-source tests, 8 inventory tests and 3 actual-Proton-CMake install-rule
fixture tests pass. The fixture verifies all twelve bound redistribution files
and tamper rejection without building the app or linking a storage SDK.
Full application presets were excluded under the Proton-only boundary. The four
previously recorded upstream TypeScript diagnostics remain a stated limit.

Wrong Cargo commit and standard-library source lock are rejected. The initial
missing-rust-src build failure, corrected installer retry, preflight failures,
controlled negative tests and intentional interruptions are retained with the
successful commands. Independent artifact review covers the scripts, recipient
route, source packet and added legal texts; final Git review is retained beside
it. No Anthropic review is claimed.

Full raw logs, hashes, proof executables, pinned source/rebuild assets and review
receipts are indexed at:
`/home/patrick/work/put.gd/work/evidence/proton-p3-native-rebuild-20261015`.
The [machine-readable receipt](2026-10-15-proton-p3-rebuild.json) records exact
proof identities and points to the index/completion receipt.

## Remaining release gates

P4 must publish every catalogued source asset with final binaries and finish the
full application source handoff. P5's update rehearsal remains open. P6 must
exercise the final helper live, measure its resources, obtain current native
desktop evidence and inspect each actual shipping package. Historical P1/P2
passes belong to their original fingerprint; no pass is transferred silently to
the changed GPL-source helper. Local materials and fixture passes are not public
source delivery or final package acceptance.
