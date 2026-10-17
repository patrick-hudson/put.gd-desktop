# Credentials

Enter credentials through the saved profile in **Destinations**, following
[Destinations and credentials](user-guide.md#destinations-and-credentials). CLI/MCP configuration accepts credential
references only; neither interface accepts raw keys. The C++ methods below are
internal APIs, not CLI commands. A session-mode profile needs explicit credential
entry again after the resident exits.

S3 never uses the environment/profile/IMDS credential chain. Each effective request
uses an explicit cached access key, secret key and optional session token. Credentials
are resolved by an explicit setup action; uploads and hotkeys cannot trigger unlock.
Missing credentials return `credential_setup_required` for the original destination.

`CredentialStore::unlock` reads a referenced OS secret with QtKeychain.
`CredentialStore::save` writes it and caches it only after backend success.
Both jobs explicitly disable insecure fallback. Locked, unavailable or denied stores
return stable setup errors; backend diagnostic strings and secret data are excluded.
QtKeychain may display the OS unlock prompt during that explicit setup call.
`setSession`/session-mode `save` retain credentials only in process memory. Restart
requires explicit entry again. `forget` revokes future cache access. Already-sent
requests drain using their original client/credential generation.

Configuration stores `{"mode":"keychain","reference":"putgd/profile-name"}` or
`{"mode":"session","reference":null}`. Receipts preserve this reference to restore
access to an original destination. They never preserve keys or tokens.
No secret is written to configuration, operation journals, payload spools or exported
diagnostics. Keys do live in memory and the SDK client until drained/destructed;
this implementation does not promise locked or securely wiped process memory.

The explicit GUI **Show** action can read an unlocked saved S3 secret access key through `setup.revealCredentialSecret`. It returns only that secret from the resident cache for the saved profile and accepted/observed revision; it does no keychain IO. If the cache is empty, unlock saved credentials explicitly first. The CLI rejects this method and the MCP helper does not expose it. The UI holds the revealed value read-only until Hide, a destination/provider change, page hide, close, reload, or credential clearing. Revealed saved values never become credential drafts, setup job records, snapshots, diagnostics, or checkpoints.

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
