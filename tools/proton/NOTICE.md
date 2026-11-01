# Bundled Proton runtime

`putgd-proton-runtime` embeds official Bun 1.4.4 and calls the official Proton
Drive JavaScript SDK 0.23.0 with Proton Crypto 2.2.0 and OpenPGP 6.3.1.
Crypto is selected from official WebPackages commit
`68b93b7d2cadd043cd08203710af382706ed1458`, with matching npm 2.2.0
integrity-locked inputs and its explicit GPL grant and license text.
Account/authentication foundation files are copied from the official SDK tag
`js/v0.23.0` at `8a44654aece1609f0f7218be51c205103945abd8`.
The SDK repository's MIT notice is preserved in `LICENSE.proton`.

The application identifies itself as `external-drive-put_gd@0.1.1-alpha` with
authentication client `external-drive`. Its keychain service is exclusively
`ch.proton.drive/putgd`. Provisioning patches the CLI's credential constants in
private generated source, with exact-match guards, and applies the official
full-OpenPGP crypto patch with the same guards.

`package-lock.json` pins registry tarball integrity for runtime and build
dependencies, including the official Bun executable. Build dependencies stay
inside the ignored build directory. Users do not install Bun or Node. Linux
requires its normal unlocked Secret Service/keychain and a browser for login.

No console, file or network logging/telemetry handlers are initialized. The
SDK uses no persistent entity or crypto cache. It retains at most 64 crypto-key
entries within a request; entity metadata remains uncached. Each request starts
a fresh SDK session. Account credentials and share passwords remain in the keychain.

Provisioning exports supplied dependency license texts and exact declared
licenses/integrities to `THIRD-PARTY-NOTICES.txt`, which is hashed in the manifest.
Original files remain in the generated build tree. The compiled helper is
distributed under the applicable GPL terms, with original permissive and LGPL
component notices preserved. This does not relicense put.gd’s MIT source.
The accompanying `redistribution/` directory contains Bun native attribution,
OpenPGP prebundled notices, the selected GPL license and predecessor notices,
and a hashed corresponding-source catalog and rebuild instructions. Deliver
the catalogued source assets with the binary packages; notices alone are
insufficient. Consult SOURCES.json for the current native Bun and Argon2 WASM proof scope. The user-approved 2026-10-15
decision is to ship and maintain this experimental integration; upstream
endorsement is not a release gate. Current redistribution review, authenticated
recovery/resources, shipping-pipeline integration and final-package acceptance
follow [Proton verification](../../docs/proton-drive.md#building-and-verification)
and the [release runbook](../../docs/release-runbook.md). The
[2026-10-15 register](../../docs/history/proton-drive-release-gates.md) preserves the earlier audit.
Building or protocol testing alone does not establish those gates.

Sources:
- <https://github.com/ProtonDriveApps/sdk/tree/8a44654aece1609f0f7218be51c205103945abd8>
- <https://www.npmjs.com/package/@protontech/drive-sdk/v/0.23.0>
- <https://www.npmjs.com/package/bun/v/1.4.4>

- <https://github.com/ProtonMail/WebPackages/tree/68b93b7d2cadd043cd08203710af382706ed1458>

Crypto 2.2.0 incorporates jsmimeparser: its original MIT notice and the MPL 2.0
text accompany the retained source headers and complete preferred source.
The SDK search capability adapter is embedded; its optional Rust/WASM search
engine and @boltffi/runtime are excluded by a compiler-metafile build guard.
The installed, unbundled vendor WASM remains in the helper source packet with
its SDK provenance. This is not a claim to have rebuilt the search engine.
