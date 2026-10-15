# Runtime notices

`LICENSE.proton` preserves Proton's SDK/account MIT source notice.
`THIRD-PARTY-NOTICES.txt` is the historical installed-package inventory, including
build-only and optional packages. It is bound to the existing runtime manifest;
it is not a completed redistribution review and has not been replaced by a
notice superset assembled during research.

The [P3 audit](../../../docs/evidence/2026-10-15-proton-p3-redistribution.md)
separates 10 compiler-retained JS package locations from the remaining installed
and absent packages, and inventories OpenPGP's prebundled dependencies and Bun's
native components separately. The exact helper rebuild and exported source hash
index are retained with the public upstream license/source corpus outside the
release worktree. That corpus is review material, not installed release content.

P3 remains open for crypto 2.0.0 terms/notice provenance, Bun attribution and
native source/relink materials, full preferred-source rebuild evidence, and
reviewed materials in final packages. Crypto's ISC metadata lacks a supplied
notice; later upstream GPL metadata does not resolve that earlier release.
OpenPGP's npm distribution and source maps alone omit its complete build recipe.
Bun's root license inventory is not a complete aggregate notice. Do not invent
missing copyright notices or substitute a different release's license.

Collect a fresh compiler-backed review bundle, without changing the runtime:

```sh
python3 tools/proton/test_redistribution.py
python3 tools/proton/audit_redistribution.py --runtime build/dependencies/proton-runtime --output work/proton-p3/NEW-OUTPUT
```

The output directory must be new. The collector requires an exact runtime rebuild
and emits `inventory.json`, supplied npm texts, a source archive/index and helper
rebuild instructions. It deliberately reports `reviewComplete:false`. It neither
fetches native source nor approves redistribution. The source archive selects
public generated inputs; it is not a general secret sanitizer. Keep account/run
state outside those build-source directories.
