# Credentials

Enter credentials through the saved profile in **Destinations**, following
[Destinations and credentials](../user/user-guide.md#destinations-and-credentials). CLI/MCP configuration accepts credential
references only; neither interface accepts raw keys. The C++ methods below are
internal APIs, not CLI commands. A session-mode profile needs explicit credential
entry again after the resident exits.

S3 never uses the environment/profile/IMDS credential chain. Each effective request
uses an explicit cached access key, secret key and optional session token. Credentials
are resolved by a setup action; uploads and hotkeys cannot trigger unlock. Unless
`preferences.unlockCredentialsAtStartup` is `false`, the resident starts one
`unlockCredentials` job for each saved S3 keychain profile about three seconds after
it starts. A locked desktop keyring may show its own unlock prompt for that read. A
dismissed prompt leaves the cache empty until the user chooses Unlock.
Missing credentials return `credential_setup_required` for the original destination.

`CredentialStore::unlock` reads a referenced OS secret with QtKeychain.
`CredentialStore::save` writes it and caches it only after backend success.
Both jobs explicitly disable insecure fallback. Locked, unavailable or denied stores
return stable setup errors; backend diagnostic strings and secret data are excluded.
QtKeychain may display the OS unlock prompt during that setup call, including the
startup read.
`setSession`/session-mode `save` retain credentials only in process memory. Restart
requires explicit entry again. `forget` revokes future cache access. Already-sent
requests drain using their original client/credential generation.

A custom HTTP uploader (#1361) stores named values instead of a key pair: one entry
holds a map from credential-store name to value, where a name is `headers/<name>`,
`parameters/<name>`, `arguments/<name>`, `secrets/<name>` or `data` (the request body).
One entry holds at most 160 values and 128 KB in all, names included; setup and the
store apply the same limits. `setup.storeCredentials` takes `{"values":{name:value}}`
for such a destination, checks each name against the saved settings, and merges: a
value typed now replaces the stored one and the others are kept from the loaded entry.
A missing value answers `credential_incomplete` with the names. `setup.credentialHint`
answers `storedValues` and `missingValues`, names only. An imported `.sxcu` file's
values are held in the Settings window, or stored straight to the chosen mode by
`settings.importUploader`, and reach no file on the way
([custom uploader](../user/custom-uploader.md#what-an-import-does-with-secrets)).
Its keychain reference is `putgd/http/<profile id>`. Imports are serialized: a second
`settings.importUploader` that arrives while one is between its first check and its
saved profile answers `import_busy`, so two files can never store their values under
one reference. When the profile could not be saved, `CredentialStore::erase` removes
what the import stored, from the cache and from the secret store; `credentialsStored`
in the error's details is set only if that removal itself failed. Three edges:

- The keychain entry of an ID is the import's to write and to remove only while no
  destination names it. A destination can name another ID's entry, so an import whose
  entry is already named is refused with `credential_reference_in_use` (a derived ID
  moves on to the next free one) before anything is stored.
- `SettingsService` reports a completed write in `Result::written`. An error with
  `written` set means the profile is in `config.json` and only the read-back failed:
  the import keeps the stored values and answers with `destinationWritten` in the
  error's details.
- The secret store can wait on a prompt that nobody answers. The import waits
  `importStoreDeadline` (two minutes) for the save, and again for a cleanup erase, then
  answers `credential_store_timeout` and gives its turn back. The entry stays reserved
  until the store answers after all; what it wrote then is erased.

What is looked for as a credential in a server's replies and addresses is narrower
than what is stored: see
[What counts as a credential](../user/custom-uploader.md#what-counts-as-a-credential).

Configuration stores `{"mode":"keychain","reference":"putgd/profile-name"}` or
`{"mode":"session","reference":null}`. Receipts preserve this reference to restore
access to an original destination. They never preserve access keys, secret keys or session tokens. Expiring S3/B2
download URLs are bearer secrets retained as History links; they do not serve as
object identity and their query values never enter logs or diagnostics.
Access keys, secret keys and session tokens are never written to configuration,
operation journals, payload spools, the persistent log or exported diagnostics.
Operation journals, destination-test journals and the History database can retain
bearer download links in receipts; treat those private records as sensitive.
The log redacts authorization headers, session
tokens, presigned query strings, share-link fragments and secret-shaped values when it
writes each record, and the AWS SDK never logs above Warn (Info with debug logging),
so request headers are not logged ([diagnostics.md](diagnostics.md#redaction)).
A diagnostics package reduces each credential reference to its `mode` and never
includes keychain data ([diagnostics.md](diagnostics.md#diagnostics-package)). Keys do live in memory and the SDK client until drained/destructed;
this implementation does not promise locked or securely wiped process memory.

The explicit GUI **Show** action can read an unlocked saved S3 secret access key through `setup.revealCredentialSecret`. It returns only that secret from the resident cache for the saved profile and accepted/observed revision; it does no keychain IO. If the cache is empty, unlock saved credentials explicitly first. The CLI rejects this method and the MCP helper does not expose it. The UI holds the revealed value read-only until Hide, a destination/provider change, page hide, close, reload, or credential clearing. Revealed saved values never become credential drafts, setup job records, snapshots, diagnostics, or checkpoints.

Copying an expired S3/B2 History link reads only cached credentials. A missing
cache returns `credential_setup_required` and offers **Unlock saved credentials**;
History never unlocks as part of copying or browsing. The separate explicit
`history.unlockLinkCredentials` action (also available through CLI/MCP) reads only
the saved receipt's original keychain reference. A later copy performs renewal.
Session credentials must be entered again through Destinations after restart.

A GUI setup form can use `save`, `unlock`, `cached` and `forget`, followed by
`S3Provider::readiness` and explicit `testDestination`. Callbacks from the provider
run on its worker and must be marshalled by the calling GUI service. CredentialStore
setup methods run on its owning Qt thread; `cached`/`forget`/`setSession` are mutex
protected. Session tokens expire normally; expiry requires explicit credential renewal.

Offline tests use visibly synthetic keys and an isolated loopback S3 server. They
never read user credentials or access the real secret store. The unavailable-store
proof directs DBus at a nonexistent session bus and scans its private config directory
for absence of fallback secret bytes.
The locked/denied/success fixture uses the installed QtKeychain library against an
isolated KWallet DBus protocol peer, following the
[official backend protocol implementation](https://github.com/frankosterfeld/qtkeychain/blob/0.14.2/keychain_unix.cpp).
It is a secret-store protocol proof, not a desktop unlock/prompt acceptance claim.
