# Crypto redistribution provenance resolution candidate

This investigation concerns public official Proton JavaScript sources only. It made no canonical repository edits, no account requests and no other-provider operations.

The exact npm `@protontech/crypto@2.0.0` release and its exact official source tag (`1184183ec0f83c677c1af1ab006846c7d5d5b14b`) declare ISC in package.json but supply no ISC grant or copyright notice. Full public source history did not establish an authoritative missing ISC notice. Neither a predecessor MIT license nor a later GPL correction is proof of a historical ISC grant for all 2.0.0 content.

A concrete alternative exists without taking a modern crypto upgrade: deliberately acquire the official source snapshot at `956a713c0823b365cc512018974eb4fcba47c9dd`. Its crypto package.json explicitly declares GPL-3.0. Its repository README grants GPL version3-or-later over code/data in the distribution, and root LICENSE supplies GPLv3. The commit says `Use consistent licenses in each package`. Root GPL text and the scope statement first appeared at `f8158b296e9b76d73e4af4aaaab32e2010c72c9c`; crypto's ISC metadata remained inconsistent until the correction.

This is an explicit alternate-source/license selection. It does not establish that 2.0.0 was licensed GPL retroactively. It requires distribution under the selected GPL terms and preservation of actual earlier notices. This is provenance evidence and an engineering path, not a legal opinion on the complete application's licensing.

The new source archive contains the complete official repository snapshot, including source, tests, root build configuration and dependencies lock. Its gzip representation was produced by git archive locally; it is not claimed to match GitHub codeload's gzip representation.

## Exact scope of source change

Of the63files in npm2.0.0,58are byte-identical in the explicitGPL source snapshot. Four differ and one is removed:

- package.json: version2.0.1, licenseGPL-3.0, export/build/dependency metadata changes; runtime dependency locks must remain explicitly reviewed.
- src/proxy/endpoint/api.ts: two removed TypeScript `as PublicKey` casts. No runtime algorithm change in this diff.
- src/proxy/sentry.ts: TypeScript-only logger type change.
- src/srp/srp.ts: import path `../bigInteger.ts` becomes `../pmcrypto/bigInteger.ts`; imported operations are the same underlying re-exports.
- src/bigInteger.ts: removed wrapper re-export and bigIntToNumber; the exact current helper compiler metafile attributes0outputbytes to this file. The helper does not use that function.

The new snapshot also adds a worker-pool bundler interface file. All crypto runtime source deltas are therefore import routing or erased types; no cryptographic algorithm change was found. Source equality is insufficient to promise compiled-byte identity. A candidate helper rebuild and existing no-account tests must establish that independently. The stock new package dependency ranges are not instruction to upgrade the existing locked graph blindly.

## Predecessor notices recovered

The WebPackages commits explicitly import the crypto package from WebClients (`abb81b1aa10d64d8a46a909490d5c0fe97770405`), pmcrypto (`1768a212499cab2065a1440dfe047f37766e462d`) and SRP (`1e226caeb1f042b85c98699505adb0ed32c7b091`).

- Historical WebClients snapshot `7f8b7db8788520c6dd8e8f30cbdc66bc72505fcd` root README grants GPL3-or-later generally. Its SRP package separately includes MIT text with actual Proton Technologies A.G. copyright2013-2019.
- Official pmcrypto snapshot `9a9e9d065621a8054ff4d5e5a7a284a0236bb408` supplies MIT text with actual Proton Technologies A.G. copyright2013-2017.

Those actual notices should remain available alongside GPL materials for incorporated code. They should not be relabeled as an ISC notice or treated as a single grant over all new WebPackages crypto code.

## Primary references

- Exact2.0.0: https://github.com/ProtonMail/WebPackages/tree/1184183ec0f83c677c1af1ab006846c7d5d5b14b
- RootGPL introduction: https://github.com/ProtonMail/WebPackages/commit/f8158b296e9b76d73e4af4aaaab32e2010c72c9c
- ExplicitGPL crypto selection: https://github.com/ProtonMail/WebPackages/commit/956a713c0823b365cc512018974eb4fcba47c9dd
- HistoricalSRP grant: https://github.com/ProtonMail/WebClients/blob/7f8b7db8788520c6dd8e8f30cbdc66bc72505fcd/packages/srp/LICENSE
- Historicalpmcrypto grant: https://github.com/ProtonMail/pmcrypto/blob/9a9e9d065621a8054ff4d5e5a7a284a0236bb408/LICENSE

Exact copies are under snapshots/ and predecessors/. The full diff, public commit records, file SHA256s and compiler-attribution comparison are under history/ and crypto-resolution.json. No upstream message was sent.

## Selected production dependency

The user approved this explicit GPL source on 2026-10-15. The production recipe
overlays the selected source onto an integrity-locked 2.0.0 registry bootstrap,
preserving the locked transitive dependencies. The bootstrap is not the bundled
crypto source. The runtime manifest records selected version 2.0.1, its exact
source commit and every modified source hash. Distribute the bundled helper under
the applicable GPLv3 terms with corresponding source, preserving original
project and third-party notices. No retrospective ISC/GPL assertion is made
about npm 2.0.0.
