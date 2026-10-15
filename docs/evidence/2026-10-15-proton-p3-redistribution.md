# Proton P3 redistribution audit — 2026-10-15

P3 remains open. This audit identifies the code in the exact official-JavaScript
helper, preserves source/rebuild inputs and recovers upstream notice material.
It does not establish completed redistribution requirements or final-package
acceptance. No S3 source, configuration, build, test or service was used.

## Product decision and native evidence retention

The user selected the official JavaScript helper for the production path on
2026-10-15. The native experiment remains separate and unadopted on
`codex/proton-native-trial`, commit
`e1f20ef75126ff0cf5a5cf8d48837744757a679a`. It was not reset, removed or merged.

Verified every indexed file in the retained native archives under the primary
checkout's ignored `work/evidence/`: original trial 327 files; latency follow-up
702 files; protected receipt metadata one file. All checks passed. Their index
hashes are retained in the adjacent JSON. Protected receipt metadata was not
printed or copied into this review. Existing native SDK sessions remain retired;
no login or service operation was needed.

## Exact runtime and inventory

Release-worktree starting point: `2aa56a8aa7f0944b9bfc3a96e499311514640480`.
The runtime remains unchanged:

- Helper SHA-256: `93fc77e63913dd39075a7ede049289325fb7c16a3aaf20985bc62adbc25b0e99`.
- Bun 1.4.2 executable SHA-256: `a83d263767d839e4d2649ca8e35d07159c7afc99afdc96d731ced29e056dda0c`.
- Bun revision: `744846f844374847c902b5e7fd59b4342a51ef99`.
- Official SDK npm 0.22.2; crypto npm 2.0.0; account source pin `8ffae007b0fbb8bcef53514719d6f093bb32dcf9`.

The new `tools/proton/audit_redistribution.py` compiles with a Bun metafile,
requires a byte-identical match against the existing runtime manifest, separates
compiler-retained npm inputs from other installed packages and exports an
explicitly selected source archive plus hash index. It does not change the
production runtime, manifest, lock, notices or provisioning. Its
`reviewComplete:false` is deliberate; running the collector is not a release pass.

The lock has 36 package locations: 10 bundled JS, one embedded Bun runtime,
six build-only, nine installed but not bundled, and 10 not installed. Aliases use
the actual package name; nested dependency versions remain separate. Byte counts
attribute JS contributions, not total ELF size or runtime memory. The graph
cannot see dependencies already bundled inside OpenPGP or Bun's native runtime.
Those require separate source-level inventories.

SDK npm 0.22.2 ships its own `src` and `dist`; these exact installed contents are
preserved. The account-source checkout is not an exact substitute for the SDK npm
release: 263 source files match, 24 differ or are missing; the npm gitHead did not
resolve publicly. Its MIT notice matches the preserved Proton notice, but that
does not establish source equality.

## Notice and preferred-source findings

**Crypto 2.0.0 remains unresolved.** Its npm metadata and exact public source
tag declare ISC; neither supplies a license notice. The source tag resolves to
`1184183ec0f83c677c1af1ab006846c7d5d5b14b`; all 62 non-package.json npm files
match. Package.json differences are publication substitutions/formatting. A later
[upstream correction](https://github.com/ProtonMail/WebPackages/commit/956a713c0823b365cc512018974eb4fcba47c9dd)
changed the declared license from ISC to GPL-3.0. The later declaration does not
resolve the earlier release's missing notice. No copyright text was invented and
no different version's license was substituted. Closing this needs authoritative
terms/notice provenance for the release, or a separately reviewed dependency and
licensing change with fresh verification. No upstream message was sent.

**OpenPGP source and hidden dependencies were recovered.** The npm alias is
`@protontech/openpgp@6.3.1`, source commit
`36198c3369ab8d4a02d70372d62f1aa8bc247dcb`. Its full source archive, source lock,
Rollup configuration and LGPL/GPL texts are retained. The published npm archive
contains distribution files and source maps, without the complete build recipe.
The selected node prebundle includes nine external package names at 10 locations;
51 mapped JS source files match exact integrity-verified npm archives. Notices and
additional preferred sources for fflate and Argon2 C/WASM are retained. Both
inline WASM blobs match the locked Argon2 distributions. This is a conservative
upstream-prebundle inventory, not a claim every member survives final Bun
tree-shaking. Full OpenPGP/Argon2 source rebuild and modified-library regeneration
remain unverified; upstream WASM build tooling includes an unpinned SDK image.

**Bun's npm notice list is insufficient.** Pinned upstream license/build sources
identify statically included WebKit/JavaScriptCore and ordinary-Linux TinyCC.
WebKit pin: `2e2aa2290fac856d6f451ceacb58f7f5b44dd057`; TinyCC pin:
`05f0fafaa3be31e31d7b4b5c17dc60f62c991171`. The root Bun LICENSE is an inventory,
not an aggregate notice or full copyright-bearing MIT grant. Pinned docs and the
root inventory have different omissions. Source-level copied code also matters.
The retained corpus includes native component texts, 181 checksum-verified Cargo
package inventories, 111 checksum-verified fallback package inventories, and 113
WebKit legal files. These are labelled supersets, including build/test/platform
members; they are not falsely counted as all linked into Linux. Full Linux
attribution closure, missing authoritative notices and corresponding native
source/relink materials remain open. No Bun/WebKit/TinyCC relink was attempted.

Primary evidence: [crypto npm metadata](https://registry.npmjs.org/@protontech/crypto/2.0.0),
[exact crypto source](https://github.com/ProtonMail/WebPackages/tree/1184183ec0f83c677c1af1ab006846c7d5d5b14b),
[OpenPGP source](https://github.com/ProtonMail/openpgpjs/tree/36198c3369ab8d4a02d70372d62f1aa8bc247dcb),
[pinned Bun license](https://github.com/oven-sh/bun/blob/744846f844374847c902b5e7fd59b4342a51ef99/LICENSE.md),
[WebKit build recipe](https://github.com/oven-sh/bun/blob/744846f844374847c902b5e7fd59b4342a51ef99/scripts/build/deps/webkit.ts),
[TinyCC build recipe](https://github.com/oven-sh/bun/blob/744846f844374847c902b5e7fd59b4342a51ef99/scripts/build/deps/tinycc.ts).

## Verification and retained artifacts

The helper source archive contains 6,294 files, excluding Bun executables and
unrelated run/configuration files. It snapshots generated public build inputs;
it is not a general secret sanitizer for arbitrary contamination inside a source
package. Extracting it into a new directory and compiling with the pinned Bun
reproduced the shipping SHA-256 exactly. All 41 helper tests / 253 assertions and
11 compiled no-account stdio cases passed. This proves helper reconstruction from
the archived distributions, not rebuilding OpenPGP's preferred source or native
Bun. No whole-application build/test or shipping package was run.

Six focused audit tests pass. They cover nested/aliased package attribution,
absent/build-only/tree-shaken distinctions, unknown compiler packages, archive
selection and dependency/top-level source symlink rejection. A new top-level
symlink regression failed before the fix and passed afterward. Five
simplification passes found one implementation docstring to remove and no code
changes worth applying. Independent code-review outcome is retained separately.

Preserved failures: the initial extracted-source compile ran from `/tmp` and
could not resolve `runtime.ts`; rerunning from `/tmp/pg-p3-replay/build-source`
succeeded. No source or runtime was changed to obtain the successful result.
The first audit output is preserved alongside the corrected collector output.

Checksummed cold archive outside the release worktree:
`/home/patrick/work/put.gd/work/evidence/proton-p3-20261015/`.
It contains both audit outputs, the rebuild archive/index, exact public research
sources/notices/provenance, the collector and its tests, and replay results. All
1,118 indexed files were verified. The archive index hash is recorded in the JSON.
No account credentials, capability URLs or private payloads are included.

Reproduce the collector using a new output name:

```sh
python3 tools/proton/test_redistribution.py
python3 tools/proton/audit_redistribution.py --runtime build/dependencies/proton-runtime --output work/proton-p3/NEW-OUTPUT
```

## Release disposition

P3 stays open for exact crypto terms/notice provenance; complete Bun attribution
and native corresponding-source/relink materials; full preferred-source and
modified-library rebuild evidence; and reviewed-material inclusion in the final
packages. P4 pipeline/assets, P5 compatibility maintenance and P6 final acceptance
remain open. The official JS helper and prior P1/P2 runtime identity are unchanged.
Do not interpret the source archive or collected notice superset as final
redistribution approval.
