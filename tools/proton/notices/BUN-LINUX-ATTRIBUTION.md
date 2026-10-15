# Bun 1.4.2 Linux x64 notice closure

This is redistribution engineering evidence for the Proton helper's Bun runtime, not a legal opinion or a claim that every preserved source is linked. It uses Bun commit `744846f844374847c902b5e7fd59b4342a51ef99`, matching the runtime `1.4.2+744846f84` and the previously verified linux-x64 executable SHA256 `a83d263767d839e4d2649ca8e35d07159c7afc99afdc96d731ced29e056dda0c`.

## Deliverable

`BUN-THIRD-PARTY-NOTICES.txt` aggregates upstream text and exact copied-source legal comments, deduplicated by SHA256 with every input attribution retained. `notice-provenance.json` maps each text to its original file, source hash, line and URL when available. It is intentionally a conservative notice **superset**. Non-Linux and build/test-only attribution in the superset is not a statement that those components enter this executable. The exact Bun/WebKit/native/Cargo/npm sources and their archive hashes are preserved separately in the source bundle and prior notice corpus.

## Linux scope and attribution table

| Source family | Linux classification | Materials and boundaries |
| --- | --- | --- |
| Bun | Linux runtime itself | Exact root and docs MIT declarations, real per-file copyright/license notices, and a clearly labelled standard MIT reference. Upstream does not supply a Bun-wide copyright-bearing MIT form; this is not evidence that Bun lacks an MIT grant. No copyright holder or year has been invented. |
| JavaScriptCore/WebKit | Statically linked engine; mixed per-file licenses | Exact commit `2e2aa2290fac856d6f451ceacb58f7f5b44dd057`; prior 113 standalone legal files plus source-comment notices across JavaScriptCore/WTF/bmalloc/WebCore. Source superset includes code outside the selected build. LGPL source/relink material is separately required and documented in the source bundle. |
| TinyCC | Linux build dependency included in executable | Exact commit `05f0fafaa3be31e31d7b4b5c17dc60f62c991171`; LGPL text, per-file source notices, Bun patches and source are retained. The helper's lack of FFI calls does not remove the linked implementation. |
| Native recipe libraries | Source/recipe superset across platforms | Twenty-one exact-pinned native archives supplement root/legal inventory with per-file notices. Includes libarchive's controlling per-file statements, BoringSSL, compression, image libraries, HTTP libraries and headers. Some individual archives contain tests, utilities, optional implementations or platform code not linked here. |
| libarchive | Linux direct-build tar/gzip implementation | Recipe `libarchive.ts` lists the Linux source set and selected registration patches. Source comments are preserved from the entire upstream library source as a conservative superset, including BSD attributions, xxhash, public-domain notices and notices in compiled-but-unused sources. Disabled codecs are not asserted linked. |
| libuv | Full library Windows-only; POSIX imported API portions remain | Keep source-header attribution from Bun's copied libuv API headers and shim. A libuv source notice superset is retained; it does not assert Linux links the full Windows libuv library. |
| Cargo dependencies | Conservative Linux normal/build closure | `cargo-linux-scope.json` walks `bun_runtime` through 101 workspace/vendor and 94 registry packages. It excludes dev-dependencies and definitively false target conditions, but includes optional dependencies represented in the lock and host procedural/build code. This is **not** feature-resolved linker proof. 87 other registry packages remain in the explicitly labelled full lock notice/source superset. All visited manifests resolved. |
| Node browser fallback polyfills | Embedded feature material | Preserve complete notices from the 111-package browser fallback lock superset, including nine full MIT grants supplied in README files. Bun itself says these are embedded; their absence from the helper's execution path does not erase the distributed code. |
| Bun copied source and builtins | Runtime/embedded code, platform superset | Preserve legal comments from `src/` and `packages/`, including Node-derived builtin modules, libuv headers, glob, thread pool, clap, stack formatting and other copied portions. Tests/examples/benchmarks excluded from this scan; individual platform-conditional files remain labelled as a superset. |
| esbuild/Lightning CSS/Servo derivations | Explicit Bun upstream credits | Preserve exact Bun credits and per-file imported notices. Additional esbuild and Lightning CSS license references are identified as reference snapshots, **not an assertion of exact import versions**. MPL standard text supports selectors/Servo-derived notices. The Bun source archive supplies preferred source for Bun's actual ports. |

## Eleven crates previously lacking standalone license files

| Crate | Conservative Linux closure? | Resolution |
| --- | --- | --- |
| anes 0.1.6 | No | Exact VCS revision MIT and Apache texts recovered. Retained only in lock/source superset. |
| blake2b_simd 1.0.4 | Yes | Exact VCS repository root `LICENSE` supplies full MIT grant and copyright. |
| enum-map 2.7.3 | Yes | Exact Codeberg VCS revision root `LICENSES/MIT.txt` / `Apache-2.0.txt` plus source SPDX copyright lines. Crate subdirectory LICENSES is a symlink to root, explaining packaging omission. |
| enum-map-derive 0.17.0 | Yes, proc macro | Same resolution at its separate exact VCS revision. Build-host code does not automatically enter final executable. |
| r-efi 6.0.0 | No | Exact revision `AUTHORS` contains full MIT grant and named copyrights; retained in lock superset. |
| selectors 0.33.0 | Yes | Exact source has MPL-2.0 header and standard reference. Source archive preserves preferred source. No standalone license file invented. |
| wasip3 0.4.0+wasi-0.3.0-rc-2026-01-06 | No | Exact VCS root supplies MIT/Apache/Apache-with-LLVM-exception texts. |
| wasm-encoder/wasmparser/wit-component/wit-parser 0.244.0 | No | Shared exact wasm-tools VCS root supplies all three license texts. |

The graph result is source-based and intentionally conservative. It establishes why seven of the eleven are outside the Linux production graph without claiming all remaining normal/build packages are linked. All eleven have usable upstream legal context in the preserved superset.

## Reproduce and verify

`replay_additional.py --output NEW_DIR` fetches only successful exact URLs recorded in the additional manifests and rejects changed content. It does not execute package code.

Run `derive_cargo_scope.py --bun-source BUN_SOURCE --prior-corpus PRIOR_BUN_CORPUS` to regenerate the graph. It requires Python 3.11+ (`tomllib`), full pinned Bun source, prior registry Cargo manifests, and the two pinned vendor manifests in `additional-licenses/`.

Run `collect_webkit_headers.py --git-dir WEBKIT_GIT` against the exact source bundle repository, then `build_notices.py --bun-source BUN_SOURCE --prior-corpus PRIOR_BUN_CORPUS --source-archives SOURCE_BUNDLE/archives`. Archive scopes and URLs are in `source-input-manifest.json` in the source bundle. The comment scanner preserves matching upstream legal comments verbatim and source hashes; the output is an attribution superset rather than semantic binary analysis.

`SHA256SUMS` covers this closure directory's public inputs and generated deliverables. No account, credentials, remote user content or application service was accessed, and no S3 project paths, builds or services were used.

## Remaining release integration boundaries

The notice text collection gap is closed by a documented conservative source superset. The absent Bun-wide copyright-bearing form is a documented upstream packaging fact, not an independently inferred prohibition on redistribution: preserve upstream's MIT declaration and a standard MIT reference without manufacturing attribution.

The exact linked ELF closure has not been reproduced, and no native Bun/WebKit relink build has passed here. Package assembly must include this notice text, corresponding-source/rebuild materials, and their integrity manifests under the selected distribution arrangement. The source-bundle preflight documents any compiler/toolchain blockers. Do not equate this corpus with a built-and-tested release package or a legal certification.
