# Proton P3 source and notice materials — 2026-10-15

The user approved replacing crypto's unresolved historical npm 2.0.0 source with
the official explicit GPL WebPackages source at
`956a713c0823b365cc512018974eb4fcba47c9dd`, package version 2.0.1.
The source selection, notices and corresponding-source materials are implemented.
**P3 remains open for native compiler/relink proof and final delivery integration.**
No Proton account or project S3 source, configuration, tests or service was accessed.
The [earlier audit](2026-10-15-proton-p3-redistribution.md) is retained as history.

## Source selection and artifact identity

The public history did not establish a missing ISC grant for the exact 2.0.0
release. The approved alternative has its own explicit GPL license and scope
statement. This is a deliberate source selection, not a retroactive license
interpretation. Actual pmcrypto and SRP predecessor MIT notices were recovered
and preserved. See [provenance](../../tools/proton/notices/CRYPTO-PROVENANCE.md).

The npm 2.0.0 archive remains an integrity-locked dependency bootstrap. Provisioning
replaces its source/package metadata with the selected 2.0.1 source before the
existing full-OpenPGP patches. Nested locked dependencies remain intact. The
63-file comparison found 58 identical files, four changed files and one removed
unused wrapper: metadata, erased types and SRP import routing account for the
changes; no algorithm change was found. The compiler graph changes from 231 to
230 modules, and the executable changes. The explicit GPL selection has the
user's approval; the bundled helper carries the applicable GPL terms and source.
put.gd's original MIT notice is preserved.

- Source baseline: `55daa86ce65fb046be1fefac0c0ca1b547a73094` plus this follow-up.
- New helper SHA256: `eeffe97c9c7f473895f790434dabafb13a018b8909a2f5215c3b769e7b7bb539`.
- Previous helper: `93fc77e63913dd39075a7ede049289325fb7c16a3aaf20985bc62adbc25b0e99`, retained separately.
- Bun remains 1.4.2, SDK remains 0.22.2, SDK/account pin remains `8ffae007b0fbb8bcef53514719d6f093bb32dcf9`.
- New complete runtime-lock SHA256: `419547e7a3b703b4dd88c03aa9db2fec7e98a52898ace75caf8a09bf53deffd6`.

Historical P1/P2 passes belong to the prior fingerprint. This receipt does not
claim fresh live upload, recovery, memory or graphical/package acceptance for
the new helper. Those checks remain required on the final P6 candidate.
The separate native trial and its indexed evidence remain unchanged.

## Notices and source delivery

Compiler output classifies ten JS package locations as bundled, nine as installed
but unbundled, six as build-only, ten as absent and one as the embedded runtime.
All ten retained JS locations now have supplied notice text. Crypto and SDK
original license texts omitted from npm packaging are preserved from their exact
selected source. OpenPGP's prebundled dependencies have a separate notice/source
inventory; npm top-level metadata alone is not treated as complete attribution.

Bun's aggregate preserves 23,143 source mappings to 6,105 unique legal texts,
including copied-source/WebKit headers and the eleven previously incomplete crate
contexts. The conservative Linux normal/build graph has 94 registry and 101
workspace/vendor locations, plus 87 registry packages retained only in the lock
superset, with zero unresolved contexts. The collection includes platform/build
supersets and does not claim exact linked ELF attribution. Bun's actual MIT
declarations, standard MIT reference and real upstream copyrights are retained;
no missing copyright holder/year or historical ISC text was invented.

The helper manifest binds all twelve redistribution files. Actual Proton CMake
rules install those files with the exact executable, manifest and package notice
inventory, and reject source, lock or notice tampering. This is an install-rule
fixture result, not a complete shipping-package acceptance result.

The five catalogued assets below are preserved under
`/home/patrick/work/put.gd/work/evidence/proton-p3-materials-20261015/release-source-assets/`.
They must accompany binary releases; local retention and an installed catalog do
not establish public source delivery. The catalog is intentionally separate from
the build-controls archive to avoid a self-hash cycle. It distinguishes prepared
materials from distribution approval and incomplete native proof.

| Accompanying artifact | Bytes | SHA256 |
| --- | ---: | --- |
| `putgd-proton-build-controls.tar.gz` | 3,200,271 | `f5888717ebeecec985005eb83333c60e0bcdf3a6344235819a8b285c66484162` |
| `putgd-proton-helper-rebuild.tar.gz` | 15,261,507 | `5f6060a5b744db2d69ac858c310c667d1ea1fea3918c438bce1f172fb2012b96` |
| `putgd-proton-crypto-source-and-provenance.tar.gz` | 326,295 | `7e726a9974c73616d8c47c60a84939238970066088e47b8e93eb4d1fc2aa506a` |
| `putgd-proton-openpgp-preferred-source.tar.gz` | 86,740,453 | `b980431b29868350e6b09cee3782db60cfaffe9ffba4b74d722e0b97984a621c` |
| `putgd-proton-bun-sources-1.4.2.tar` | 2,352,957,440 | `754765f1f38e6e57e369806ac8e19b190a6eea812696848003bd40df9f06df27` |

The Bun packet contains full pinned Bun and WebKit Git objects, 21 native source
archives including TinyCC, 181 Cargo archives, 171 npm lock entries, Node headers,
ICU source, patches, identity checks, offline reconstruction and fail-closed native
build/preflight scripts. It also includes optional published build-tool binaries;
it is labelled source/build inputs rather than a text-only archive.

The OpenPGP packet supplies complete preferred source, lock/build config, offline
npm inputs, original prebundled inputs/notices, complete Argon2 C/WASM source and
its upstream build script. The helper archive preserves 6,298 public selected
source files; the build-controls packet supplies the project provisioner,
locks/tests, notices and CMake integration. These are component materials;
the full application source/rebuild handoff remains P4.

## Verification and limits

- 7 crypto-source tests and 8 inventory tests pass. New ignored-dependency and
  wrong-bootstrap regressions failed before their fixes. Dirty/incorrect source
  and license inputs are rejected before replacement, including ignored nested
  packages and source-root symlinks.
- All 3 actual-Proton-CMake install fixture tests pass: exact runtime plus twelve
  files installed; modified bundled Bun notice rejected; changing only the
  approved crypto license fingerprint invalidates the helper. The fixture builds
  no application and does not link Qt or any storage SDK.
- Final provisioning passes 41 helper tests / 253 assertions, eleven compiled
  no-account stdio cases and all 1,048,337 bytes of the stdout regression.
  Recompilation from the preserved selected source graph matches the new helper
  exactly. No live account test was run.
- The relocated OpenPGP source packet installs its public cache fully offline
  with scripts disabled and rebuilds its selected Node library byte-for-byte:
  `70a6e1a013779d79ee8c47a19d23da37f5458918ecbb216efbd6a4ae49e67e58`.
  The supplied patch makes only the release-date banner reproducible via
  `SOURCE_DATE_EPOCH=1785456000`.
- A modified preferred-source OpenPGP build was substituted into a full compiled
  helper using the selected crypto 2.0.1 source and passed its 41/11 checks.
  That isolated proof helper hashes
  `943387ceecda79e12a3e26b0b6611b707f8bec6e781e7ba7c6215e7fdd7328a6`;
  it is not the shipping artifact. A separate compiled fixture asserts the
  modified configuration and performs a real encrypt/decrypt roundtrip. This
  proves selected library replacement, not live upload or native Bun relinking.
- Bun offline reconstruction passes all 21 exact native fetch-identity checks,
  three npm groups with offline installation/scripts disabled, and restored
  WebKit Git integrity checks. Source reconstruction is distinct from compilation.
- TypeScript checking still fails with the four previously recorded upstream
  Crypto/Bun stream declaration diagnostics; no clean type-check is claimed.
  Python checks and `git diff --check` pass. Full application presets were not run
  under the explicit Proton-only boundary.
- Focused code review and independent same-family adversarial review found two
  provenance guard issues, both fixed and regression-checked. Five simplification
  passes retained all proof tests and reused existing notice discovery. The
  cross-model runner failed before egress; no Anthropic review is claimed.

Earlier failed prerequisite attempts are summarized in `checks/ATTEMPTS.md`;
the superseded OpenPGP packet is retained. All five source-asset checksums, 40
build-control files against the checkout and 388 Bun delivery entries pass
streamed integrity checks.
The full check/review logs, previous and final runtime identities, public notice
corpus and assets have an indexed durable receipt beside the release checkout.

## Remaining gates

1. **Native Bun/JSC/TinyCC modification and relink proof.** Source and build
   scripts are present, but this host lacks LLVM 21.1.x and the pinned Rust nightly
   2026-07-20. Distro Rust 1.91 is installed and does not satisfy that nightly.
   Ruby/NASM/ICU development inputs are also missing. No substitute toolchain was
   installed, consistent with AGENTS.md; the preflight stops before compilation.
2. **Argon2 WASM preferred-source compiler proof.** Full source is supplied;
   its upstream script defaults to an unpinned Emscripten container. No WASM build
   was run. Select and record a reproducible compiler version before crediting it.
3. **P4/P6 source and actual package delivery.** Publish every catalogued asset
   with the final binary packages, finish the full application source handoff,
   and inspect each advertised package. Fresh final-helper functional/resource
   and native desktop evidence remains required. No old-fingerprint pass is
   silently transferred to this candidate.

The materials work is complete within these stated proof limits. P3 is not marked
closed, and this receipt makes no legal certification or final release claim.
