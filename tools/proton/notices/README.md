# Proton runtime redistribution materials

The selected helper uses official SDK 0.22.2, Bun 1.4.2 and the explicit GPL
WebPackages crypto source 2.0.1 at `956a713c0823b365cc512018974eb4fcba47c9dd`.
The user approved this source change on 2026-10-15. Historical npm crypto 2.0.0
ISC notice provenance remains unresolved; it is used only as a dependency
bootstrap before replacement. No later grant is applied retroactively.

`THIRD-PARTY-NOTICES.txt` records actual installed versions, bootstrap integrity,
source overrides and compiler classifications. The compiler retains ten JS
package locations. Bun attribution and OpenPGP prebundled code are separate:

- `BUN-THIRD-PARTY-NOTICES.txt.gz` preserves upstream texts as a labelled source
  superset. `BUN-NOTICE-PROVENANCE.json.gz` maps every text to its source.
  `BUN-LINUX-ATTRIBUTION.md` explains the conservative Linux graph and limits.
- `OPENPGP-PREBUNDLED-NOTICES.txt` preserves original component notices.
- `LICENSE.crypto-GPL-3.0`, `LICENSE.pmcrypto-MIT` and `LICENSE.srp-MIT` preserve
  selected-source and actual predecessor texts. `CRYPTO-PROVENANCE.md` explains
  their history. `LICENSE.boltffi-MIT` covers installed, unbundled source.
- `SOURCES.json` identifies the required accompanying source assets by SHA256.
  `REBUILD.md` and `OPENPGP-REBUILD.patch` give the reconstruction route.

The manifest hashes all twelve delivery files. CMake rejects changed source,
locks, notices or runtime copies and installs them together. This verifies
install rules; actual shipping packages and source publication remain P4/P6.

The [materials receipt](../../../docs/evidence/2026-10-15-proton-p3-materials.md)
records the approved source choice and original prepared assets. The later
[rebuild receipt](../../../docs/evidence/2026-10-15-proton-p3-rebuild.md) completes
native Bun/JavaScriptCore/TinyCC modification/relink and Argon2 WASM compiler
proof using disposable pinned toolchains. P3's notice/source/rebuild work is
complete. The marked native proof binaries are not production replacements.
The current catalog adds the corrected rebuild supplement and refreshed build
controls; the original source packets stay unchanged. P4 source publication and
P6 final-helper functional/resource/package acceptance remain required.

The Windows helper (`provision_runtime.py --target windows-x64`, #408) ships sixteen
files: the same Bun aggregate and JavaScript notices, `BUN-WINDOWS-ATTRIBUTION.md`
and `BUN-WINDOWS-SCOPE.json` (the Windows scope checked against that aggregate),
`LICENSE.icu4c-78.3` (ICU, embedded by the WebKit prebuild), and the Windows catalog
`SOURCES-WINDOWS.json` with `REBUILD.md` and `REBUILD-WINDOWS.md`. The catalog shares
the six packets and the overlay with `SOURCES.json` and pins the Windows helper and
Bun package; its manifest records `redistributionComplete: true` only when they match
the build. `tools/releases/audit_proton.py` checks the Windows folder layout.

Collect a fresh compiler-backed bundle into a new output directory:

```sh
python3 tools/proton/test_crypto_source.py
python3 tools/proton/test_redistribution.py
python3 tools/proton/audit_redistribution.py --runtime build/dependencies/proton-runtime --output work/proton-p3/NEW-OUTPUT
python3 tools/proton/test_install.py
```

The collector requires an exact runtime rebuild and reports `reviewComplete:false`;
it is an evidence collector, not a distribution approval. Its source selection
contains public generated inputs and is not a general secret sanitizer. Keep
account and run state outside build-source directories.
