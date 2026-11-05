# Proton runtime redistribution materials

The selected helper uses official SDK 0.23.0, Bun 1.4.4, OpenPGP 6.3.1 and GPL
WebPackages crypto 2.2.0 at `68b93b7d2cadd043cd08203710af382706ed1458`.
The crypto registry bootstrap and selected source versions both say 2.2.0.
Historical 2.0.0/2.0.1 provenance is preserved separately.

`THIRD-PARTY-NOTICES.txt` records actual installed versions, bootstrap integrity,
source overrides and compiler classifications. Compiler classification records the actual embedded inputs. Bun attribution and OpenPGP prebundled code are separate:

- `BUN-THIRD-PARTY-NOTICES.txt.gz` preserves upstream texts as a labelled source
  superset. `BUN-NOTICE-PROVENANCE.json.gz` maps every text to its source.
  `BUN-LINUX-ATTRIBUTION.md` explains the source scope and limits.
- `OPENPGP-PREBUNDLED-NOTICES.txt` preserves original component notices.
- `LICENSE.crypto-GPL-3.0`, `LICENSE.pmcrypto-MIT` and `LICENSE.srp-MIT` preserve
  selected-source and actual predecessor texts. `CRYPTO-PROVENANCE.md` explains
  their history. `LICENSE.jsmimeparser-MIT` and `LICENSE.MPL-2.0`
  preserve the new MIME parser notices. `LICENSE.boltffi-MIT` covers installed, unbundled source.
- `SOURCES.json` identifies the required accompanying source assets by SHA256.
  `REBUILD.md` and `OPENPGP-REBUILD.patch` give the reconstruction route.

The manifest hashes each required redistribution file. CMake rejects changed source,
locks, notices or runtime copies and installs them together. This verifies
install rules; actual shipping packages and source publication remain P4/P6.

The [materials receipt](../../../docs/evidence/2026-10-15-proton-p3-materials.md)
records the approved source choice and original prepared assets. The later
[rebuild receipt](../../../docs/evidence/2026-10-15-proton-p3-rebuild.md) records
native Bun 1.4.2/JavaScriptCore/TinyCC modification/relink and Argon2 WASM compiler
proof using disposable pinned toolchains. P3's notice/source/rebuild work was
completed for that earlier runtime. This proof does not establish Bun 1.4.4
native relink; consult SOURCES.json for the current proof and remaining gates.
The marked native proof binaries are not production replacements.
The current catalog updates the Bun 1.4.4, helper and build-controls packets.
OpenPGP and the historical native rebuild supplement retain their original
bytes; the crypto packet now includes complete 2.2.0 source and earlier evidence. SOURCES.json records source publication and P6 final-helper
functional/resource/package acceptance separately.

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
