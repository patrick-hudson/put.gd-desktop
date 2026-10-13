# Bundled Proton runtime

`putgd-proton-runtime` embeds official Bun 1.4.2 and calls the official Proton
Drive JavaScript SDK 0.22.2 with Proton Crypto 2.0.0. Account/authentication
foundation files are copied from the official SDK repository at
`8ffae007b0fbb8bcef53514719d6f093bb32dcf9` during explicit provisioning.
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
SDK uses no persistent entity or crypto cache. Each request starts a fresh SDK
session. Account credentials and share passwords remain in the keychain.

Provisioning exports supplied dependency license texts and exact declared
licenses/integrities to `THIRD-PARTY-NOTICES.txt`, which is hashed in the manifest.
Original files remain in the generated build tree. A complete
redistribution license inventory, Proton third-party/service readiness, live
account acceptance and desktop acceptance remain release gates. Building or
protocol testing alone does not establish those gates.

Sources:
- <https://github.com/ProtonDriveApps/sdk/tree/8ffae007b0fbb8bcef53514719d6f093bb32dcf9>
- <https://www.npmjs.com/package/@protontech/drive-sdk/v/0.22.2>
- <https://www.npmjs.com/package/bun/v/1.4.2>
