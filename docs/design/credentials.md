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
A Store of new credentials advances the credential revision when it starts and again
when it succeeds, for the destination and for every destination that names the same
keychain entry, so a passed test made with the earlier keys no longer counts
([destination checks](configuration.md#destination-checks)); Unlock does not.

`CredentialStore::unlock` reads a referenced OS secret with QtKeychain.
`CredentialStore::save` writes it and caches it only after backend success.
Both jobs explicitly disable insecure fallback. Locked, unavailable or denied stores
return stable setup errors; backend diagnostic strings and secret data are excluded.
QtKeychain may display the OS unlock prompt during that setup call, including the
startup read.
`setSession`/session-mode `save` retain credentials only in process memory. Restart
requires explicit entry again. `forget` revokes future cache access. Already-sent
requests drain using their original client/credential generation.

Nextcloud and WebDAV (#1872) store a key pair: the access key is the login name (WebDAV: the
user) and the secret key the app password (WebDAV: the password). A Nextcloud sign-in
(`setup.connectAccount`, Login Flow v2) saves the app password Nextcloud hands over to the
profile's reference, keychain or session, and the job's result never holds it. The DAV user
in request paths is the account ID the server reports (`/ocs/v2.php/cloud/user`), which can
differ from the login name. `setup.signOut` tries to revoke the app password on the server
(`DELETE /ocs/v2.php/core/apppassword`) and then erases the entry. A refusal (401, 403 or 404:
not an app password, or already revoked) gives `revoked` false with no error, so it reads as
signed out; after the Server's origin changed, the binding check stops the old app password
from being sent to revoke it, so users sign out first and remove leftovers under Devices &
sessions; a keychain entry not read yet is read first so
it can be revoked. Removing a Nextcloud destination in Settings waits for that sign-out and says
when the server didn't confirm the revocation, then removes it. Sign-in and sign-out run one at a
time per destination. A sign-in that finishes after it was cancelled, after the destination was
removed or changed its server, or whose account lookup or keychain save fails, revokes the new
app password instead of keeping it, in a helper of its own, and says when the server didn't
confirm it. Signing in again reads the earlier app password from the keychain first and revokes
it only once the new one is saved; when it can't be read or the revocation isn't confirmed, the
result says so and where to remove it. An earlier app password bound to another server address isn't
revoked (put.gd never sends it there), so sign out before changing servers. The worker keeps no cookies, so every request authenticates with the app
password alone.

The key pair is bound to where it was given: beside it, in the entry's session-token field, put.gd
keeps non-secret JSON naming the origin (scheme, host and port) and, for Nextcloud, the account
ID. `setup.connectAccount` writes it; `setup.storeCredentials` writes the origin for these
providers, and the first `setup.inspectAccount` adds the account ID, for the credential it
inspected only: one changed meanwhile is left unbound. A Nextcloud app password stored by hand
does nothing but that inspection (and sign-out) until it is bound; Test runs the inspection
first. Storing credentials, signing in, signing out, the inspection and Test (including the
check a diagnostics package upload runs) run one at a time per destination. Signing in again reads any earlier app password from the keychain first (an entry
that isn't there is no earlier grant). Binding is by origin, as browsers scope Basic authentication: another folder on the
same server keeps the same credentials. Before any request that
carries the credentials (upload, Test, History stat, delete and links, sign-out) the provider
checks the bound origin against the destination and answers `credential_setup_required`
(`details.reason` `binding`) without sending anything when it differs. The account ID can only
be checked after the authenticated `/ocs/v2.php/cloud/user` request: a mismatch there answers
`credential_setup_required` and stops that operation before its next request. An app password made by hand is entered in
Settings (Use an app password instead) or through the session socket; `putgd-cli` and the MCP
helper never carry it, as for every service.

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

An SFTP destination (#1871) stores two named values, `password` and `passphrase`, in one
entry, and either may be empty. `setup.storeCredentials` takes
`{"values":{…}}` with one or both names; a name left out keeps its saved value (#1890). Sending both names
skips the keychain read. With a name left out, a keychain entry that isn't loaded is read first.
If that read fails, the store is refused (`credential_incomplete`) and the saved values are left
alone. The exception is a "not found" answer for an entry with no recorded successful save; a
locked entry can answer "not found" too. The destination check records keep those saves across
restarts for as long as a destination names the entry or owns it as `putgd/sftp/<profile id>`,
in either credentials mode. Records from before this rule count every keychain entry a
destination names as saved. A left-out name with no value
to keep (nothing saved, or a session-mode destination with nothing loaded) is stored empty.
Stores to one keychain entry run one at a time, and a cancel while the entry is read stores
nothing. An empty value clears that one (Settings' **Clear**, which keeps
whatever is typed into the other field). Settings sends only what was typed, so storing a
passphrase keeps a stored password. A value holds no
line break, since ssh reads its askpass answer up to one, and is at most 4096 characters.
`setup.credentialHint` lists in `storedValues` only the values that aren't empty. The values
reach `putgd-sftp-worker` only with requests that sign in (Settings' address check learns only
whether a password is set), and from there only a private askpass socket for each connection that signs in, which gives
the password and the key passphrase at most once each, answering ssh's own exact prompts
([storage providers](storage-providers.md#sftp)): never ssh's command line or environment. The
worker wipes both once sign-in ends, except that an upload keeps them until the request ends,
since cleaning up a cancelled upload may need a new connection. A keychain-mode SFTP destination whose entry isn't loaded still connects,
with the agent and key files. Its keychain reference is `putgd/sftp/<profile id>`; a new SFTP
destination starts in session mode, so startup unlock never asks the keychain about one that
has nothing stored.

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
Operation journals and the History database can retain bearer download links in
receipts; treat those private records as sensitive. **Test upload and link** opens
its expiring S3 link or authorized B2 link but writes it to the destination-test
journal and result without its query or fragment, so they hold no signature, access
key ID or download token (#1680). The resident strips these links at its storage
test journal boundary, from every checkpoint of every storage destination test it
writes (S3 and B2, and any later provider whose test goes through that writer), whatever the worker sends and
whatever provider a receipt names. The Custom uploader test writes its journal on
its own path, which this boundary doesn't touch. During journal
recovery, under the resident lock and before any S3 or B2 destination test starts
(History recovery is not waited for), it also rewrites the same links in journals
written by earlier builds. That pass covers only the S3 and B2 receipt links (the
receipt, a cleanup target, and both inside a finished result), and only in a
journal whose own records show an S3 or B2 test: every destination saved by the
records that save one (an S3 upload intent and the multipart check's records) names
that same provider, or, when none was saved, there is at least one receipt and every
receipt carries non-empty B2 file and bucket IDs. A journal is not rewritten when its
saved destinations disagree, when it saves an S3 destination but has receipts that all
carry B2 IDs, or when anything in it looks like a Custom uploader's records: an
`upload_intent` record, a `link` in the result, a receipt labelled `custom-http`, or an
`objectReference` with `url`, `deletionUrl`, `deletionUrlLeftOut` or `nonce`. It is
best effort: a file
that is not a regular `<uuid>.json` of the journal schema, that is a symbolic
link, too large, unreadable or unparseable, or whose records don't show which
test it was, stays as it is, so treat older journals as sensitive too. Destination-test
journals are kept, one per test; put.gd does not remove them (#1690). Each step that
can create something at the destination is journaled, and written to disk with the
file and folder synced, before it is sent. A Custom uploader test journal is
never rewritten by this pass; it keeps the receipt the test accepted, the same
receipt History keeps for an upload.
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
