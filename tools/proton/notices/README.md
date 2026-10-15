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

The [follow-up receipt](../../../docs/evidence/2026-10-15-proton-p3-materials.md)
records exact helper reconstruction, offline source replay and modified OpenPGP
proof. P3 still requires native Bun/JSC/TinyCC and Argon2 WASM compiler/relink
proof. Material availability is distinct from a completed native rebuild.

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
