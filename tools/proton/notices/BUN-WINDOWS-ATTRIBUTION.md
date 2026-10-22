# Bun 1.4.2 Windows x64 notice closure

This is redistribution engineering evidence for the Windows Proton helper's Bun runtime, not a legal opinion or a claim that every preserved source is linked. It extends `BUN-LINUX-ATTRIBUTION.md`, whose method and aggregate it reuses. `BUN-WINDOWS-SCOPE.json` holds the machine-readable result.

## Identity

The helper embeds `bun.exe` from the integrity-locked npm package `@oven/bun-windows-x64@1.4.2` (`package-lock.json`, sha512 integrity; the manifest's `bunTargetRuntime` records the executable's SHA256). The executable carries the same identities as the Linux runtime: revision `1.4.2+744846f84`, Bun commit `744846f844374847c902b5e7fd59b4342a51ef99`, WebKit commit `2e2aa2290fac856d6f451ceacb58f7f5b44dd057`, TinyCC commit `05f0fafaa3be31e31d7b4b5c17dc60f62c991171` and ICU data `icudt78`. These were read from the executable's bytes on a Linux host; the executable was not run there.

Bun builds Windows from the same source tree and recipes as Linux. Its CI cross-compiles with LLVM 21 `clang-cl` and `lld-link` against an xwin-downloaded Microsoft C runtime (MSVC CRT 14.44.17.14, linked statically with `/MT`) and Windows SDK 10.0.26100 (`scripts/build/winsysroot.ts`, `scripts/build/flags.ts`). The WebKit Windows prebuild is cross-compiled the same way (`Dockerfile.windows` at the WebKit commit) and statically embeds ICU 78.3, the release the Linux prebuild also embeds (`icu4c-78.3-sources.tgz`, SHA256 `3a2e7a47604ba702f345878308e6fefeca612ee895cf4a5f222e7955fabfe0c0`).

## Windows scope and attribution table

| Source family | Windows classification | Materials and boundaries |
| --- | --- | --- |
| Bun, copied source and builtins | Runtime itself | As Linux. The copied-source scan covered all of `src/` and `packages/`, including the Windows-only files. |
| JavaScriptCore/WebKit | Statically linked engine | As Linux: the same commit, legal files and per-file header superset, including Windows port sources. |
| ICU 78.3 | Statically linked by the WebKit prebuild | `LICENSE.icu4c-78.3` is the release archive's `icu/LICENSE` (Unicode License V3 and ICU's third-party data and code notices). The Linux aggregate holds only WebKit's `WTF/icu/LICENSE`, an ICU 58-era text, so this file is added here. |
| libuv | Windows only: the full library is linked | Recipe `libuv.ts` is enabled for `cfg.windows`. The aggregate already preserves libuv's `LICENSE`, `LICENSE-docs`, `LICENSE-extra` and per-file notices from the exact pinned archive. |
| TinyCC | Linked (`cfg.tinycc`; Windows adds `tccpe.c`) | As Linux; the archive scan includes `tccpe.c` and the `win32/` headers. |
| SQLite | Linked (`cfg.staticSqlite`, every platform but macOS) | In-tree amalgamation, as Linux. |
| Other native recipes | Linked on Windows as on Linux | All 24 recipes at the pinned commit are enabled for Windows x64; libarchive adds its Windows sources and BoringSSL its NASM assembly, both inside the archived sources already scanned. |
| Cargo dependencies | Conservative Windows normal/build closure | The Linux derivation, rerun with `x86_64-pc-windows-msvc` cfg values, gives 192 packages (91 registry, 101 workspace/vendor), a subset of the Linux closure. The same script reproduces the Linux closure exactly. Every registry crate maps to preserved crate or recovered upstream legal text. Not a linker SBOM. |
| Node browser fallback polyfills, esbuild/Lightning CSS/Servo credits, Rust standard library source | As Linux | Platform-independent material, already in the aggregate. |
| Microsoft C runtime, STL and Universal CRT (static) | Linked by Bun's and WebKit's Windows builds | Microsoft components fetched by xwin with `--accept-license`; no source exists to deliver. Windows supplies the system DLLs the executable imports. Their terms are recorded as an open distribution question below, not resolved here. |
| LLVM `clang-cl`, `lld-link`, compiler-rt builtins | Build toolchain | Apache-2.0 WITH LLVM-exception, which needs no attribution for compiled forms. |

## Corresponding source

The six published source packets catalogued in `SOURCES.json` cover the Windows runtime as well: the Bun packet carries the same Bun and complete WebKit Git sources (with `Dockerfile.windows` and `build-icu.ps1`), `icu4c-78.3-sources.tgz`, the libuv archive, every Cargo crate including the `windows-*` crates, and the TinyCC source. `SOURCES-WINDOWS.json` pins the Windows helper and Bun package against those packets, and `REBUILD-WINDOWS.md` gives the Windows rebuild route. The packet hashes were re-verified on 2026-10-22 by streaming both published parts.

## Remaining boundaries

- The exact linked PE notice closure has not been derived; the aggregate stays a conservative source superset.
- No modified native Bun/JavaScriptCore/TinyCC Windows relink has been performed. The Linux relink proof exercised the same sources; a Windows relink needs the Microsoft runtime and SDK through xwin under Microsoft's terms.
- The statically linked Microsoft runtime is distributed under Microsoft's terms, not an open-source license. Whether those terms suit a public Windows release with the GPL-3.0 crypto source is for Patrick's review before publication.

## Reproduce

`derive_cargo_scope_target.py --target windows` and `windows_scope.py` (in the private evidence folder `work/verification/2026-10-22-windows-proton-ship-408/attribution/`) regenerate `BUN-WINDOWS-SCOPE.json` from the pinned Bun recipes, the 2026-10-15 prior corpus, the shipped provenance, the Windows `bun.exe` and the ICU archive. They read files only and fail when a Windows component has no preserved legal text.
