# Runtime notices

`LICENSE.proton` preserves Proton's SDK/account MIT source notice.
`THIRD-PARTY-NOTICES.txt` records the installed integrity-locked package inventory
and their supplied license texts for the helper verified in `../VERIFICATION.md`.
The runtime build also generates an identical notice file and binds its SHA-256
in `build/dependencies/proton-runtime/manifest.json`.

The inventory includes build packages and optional installed packages; it does
not assert that every listed package is embedded. Some published npm packages
declare a license without shipping its text. Bun's embedded third-party notices
and those missing supplied notices require review before redistribution. The
LGPL notice for Proton OpenPGP is included; the pinned source/dependency recipe
allows rebuilding the helper with modified dependencies.
