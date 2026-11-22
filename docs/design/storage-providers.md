# Storage provider contract

Implement `StorageProvider` and register it in `ProviderRegistry`. Settings validation
must be pure and must not read source content, unlock secrets or contact the network.
No profile is accepted without its registered validator. The production registry has
no test provider. S3 implementation belongs exclusively under `src/storage/s3/`,
Backblaze B2's under `src/storage/b2/` ([b2.md](../user/b2.md)), Cloudflare R2's under
`src/storage/s3/` as a provider over the S3 one (`R2Provider`, [r2.md](../user/r2.md)) and the custom HTTP
uploader's under `src/storage/http/` ([custom-uploader.md](../user/custom-uploader.md)) and the
SFTP destination's under `src/storage/sftp/` ([sftp.md](../user/sftp.md), [below](#sftp)).

All network methods complete through a callback exactly once, including cancellation,
permission failure, timeout and ambiguous outcomes. Callbacks may run on provider
threads; the caller waits only on a worker. Providers enforce their own bounded network
timeouts. `serializationKey` identifies the real destination and object across aliases,
profile edits and object versions, so upload/deletion mutations cannot bypass ordering
by changing a profile revision. It must map a proposed name and its returned opaque
object reference to the same identity.

`UploadRequest.progress` is optional informational telemetry. Providers may report
`phase` (`preparing`, `uploading`, `finalizing`) and nonnegative exact-integer
`bytesTransferred`, with `bytesTotal` only when it uses the same accounting.
Retries may reduce the counter. Stop reporting before the one completion callback;
progress cannot establish publication, identity, ownership or success. Shared
services coalesce active observations without per-tick durable journal writes,
and ignore late reports after the upload attempt closes. The final observation
may be retained with the final result. Providers without progress remain valid.
Providers with reliable upload-phase reporting override `reportsUploadProgress()`
to return true. The tray then waits for `uploading` or `finalizing` before turning
green, remaining blue through provider setup and `preparing`. Other providers keep
request-boundary activity tracking. S3, Backblaze B2 and Proton declare phase reporting.

Capabilities explicitly describe conditional create/replace/delete, version addressing,
stat, removal, URL availability, naming byte limits and deletion effects. Unsupported
required safeguards fail explicitly when the service refuses them (an S3 501). An S3
endpoint that accepts the condition headers and ignores them (Garage, Zenko CloudServer,
and for `DeleteObject` also MinIO Community and R2) can't be detected that way; its race windows
are documented in [S3-compatible services](../user/s3.md#s3-compatible-services).
`checkName` returns absent, present or inconclusive, an opaque target reference and an
opaque precondition. Denied/time-limited checks are never treated as available names.
The conditional publishing request (`PutObject`, or `CompleteMultipartUpload` for a
multipart upload) is the final race protection on a service that enforces it.

A destination whose server names each upload declares `serverNamesObjects` (#1361).
It has no name to check, so `checkName` answers absent with a fresh nonce and contacts
nothing, and the required conditional-create safeguard does not apply. `stat` and
`reconcile` answer unknown with `unsupported_capability`: an upload whose outcome is
unknown stays unknown, History says a retry may leave a second copy, and only an
explicit retry sends the file again. A resend that fails or is cancelled leaves the
item unknown with that attempt's error, because the first request's copy may still
be there (#1572); only a confirmed upload clears it. `referenceDelete` means deletion addresses exactly
what the upload returned (a deletion URL), so there is nothing to stat first.
`receiptCapabilities(receipt)` answers for one recorded upload rather than for today's
settings: an upload that came back without a deletion URL reports no removal, with the
reason in `removeUnavailable`. `safeRemoval()` is true for a provider that can delete
by such a reference or under the conditional and versioned safeguards above.
`diagnosticSettings` is what a diagnostics package may show of a profile's settings;
the custom uploader reduces rows to names and addresses to scheme and host.

A version-1 receipt preserves provider/profile/revision, original non-secret destination,
credential-store/session reference, versioned opaque object reference (including link
expiry when present), concurrency token, publication marker and direct URL as separate fields. Core/history/CLI never parse the
URL, assume bucket/key/ETag fields, or reroute through current profiles. Upload results
are uploaded, collision, failed or unknown. Reconciliation is own, foreign, missing or
unknown and must inspect the saved publication identity before any retry/rename.

`refreshLink` is an explicit copy-time operation. S3 and B2 verify the saved
publication and use only its original destination and cached credential reference;
they never unlock credentials. History commits a changed URL and `linkExpiresAt`
only after checking that every other receipt field still matches. `stat` leaves an expiring link’s saved URL and expiry unchanged, so deletion
identity remains stable. B2 also checks that its filename-based download URL still serves the recorded file version before issuing another token.
`unlockLinkCredentials` is a separate explicit user action on the credential
store's owning thread; it may ask the desktop keyring to unlock the original
saved reference even when that profile no longer exists in current settings.

Deletion receives the exact receipt, persistent deletion operation ID and versioned
opaque progress. Before each driver network step call `DurableCheckpoint`, then await
its successful acknowledgement. A failed checkpoint means no associated destructive
request may be sent. Store intent and returned step identities separately. Never replace
a saved delete precondition with a replacement object's token. Report removed,
already-missing, changed, failed, unknown, hidden-version-retained, version-retained or
unsupported-safe-removal outcomes honestly. An ambiguous mutation is reconciled only
on explicit retry, with the recorded progress; it must not be silently reissued.

The synthetic opaque provider in `tests/fixtures/U2Fixtures.h` uses unrelated file and
revision identifiers and a URL that cannot reconstruct them. Its integration tests
exercise upload, SQL persistence, CLI-compatible JSON, deletion identity and durable
step checkpoints. These are provider-neutral headless evidence, not live S3 evidence.

Native source callbacks may arrive on any thread or after cancellation. The source
adapter outlives the services; cancellation need not drain callbacks synchronously.
Services lifetime-guard and marshal acquisition/copy completions to their owner thread.
Clipboard-revision inspection must not read payloads.

Explicit retry can carry an optional execution `requestTimeoutMs` separately
from the accepted destination. Providers opt in through `retryRequestTimeout`;
the default preserves existing behavior. Proton selects the longer budget only
for a matching provider/profile/account reference/root/folder. This budget never
changes saved receipt identity or accepted sharing/collision settings. A
`collision` with `upload_draft_conflict` is evidence of an unfinished reservation,
not publication or ownership; rename/skip can proceed only after reconciliation
proves `missing`. A generic collision with missing/unknown proof remains unknown.

## SFTP

The `sftp` provider (#1871) uses the user's own OpenSSH client as its transport and speaks
SFTP version 3 over it in `putgd-sftp-worker`. It links no SSH library, so `~/.ssh/config`
(aliases, ProxyJump, `IdentityFile`, `IdentityAgent`, FIDO keys, crypto policy) applies as in
a terminal, and the packages depend on the OpenSSH client instead of bundling one. The
resident links only the settings rules (`SftpCommon`) and the provider; the worker is the
only SFTP target with Qt Network, for Test's link fetch.

**Transport.** Every connection runs `ssh` in a new session (no controlling terminal) with
`LC_ALL=C`, put.gd's own variables removed and the AppImage's library overrides undone
(`host::environment()`), and these options:

```
-T -x -a -o ClearAllForwardings=yes -o RequestTTY=no -o RemoteCommand=none
-o PermitLocalCommand=no -o EscapeChar=none -o ControlMaster=no -o ControlPath=none
-o UpdateHostKeys=no -o StrictHostKeyChecking=yes -o NumberOfPasswordPrompts=1 -o BatchMode=no
-o ServerAliveInterval=15 -o ServerAliveCountMax=4 -o ConnectTimeout=20
-o UserKnownHostsFile="<the user's files, as ssh -G names them>" "<config root>/known_hosts"
-s [-p port] [-l user] -- host sftp
```

`BatchMode` stays off because it disables askpass. `ssh -G` with the same destination
arguments gives the resolved host, port, user, key files that exist, `IdentityAgent`, proxy
and the user's known_hosts files, for the Settings line and for error messages. The agent is
`SSH_AUTH_SOCK`, else the first socket of `$XDG_RUNTIME_DIR/{gcr/ssh, keyring/ssh,
ssh-agent.socket, openssh_agent, gnupg/S.gpg-agent.ssh}`. Only stable startup failures in
ssh's stderr map to codes (unknown or changed host key and in which file, permission denied,
refused, timed out, host not found, subsystem refused); a stream that starts with something
other than an SFTP packet (shell startup output) is `ssh_no_sftp`; anything else is
`ssh_failed` with the last line.

**Askpass.** ssh closes inherited descriptors, so a secret can't travel on a pipe. Before each
connection the worker opens a Unix socket in a new 0700 directory under `$XDG_RUNTIME_DIR`
(else the temporary directory), with a 32-byte random token, and sets `SSH_ASKPASS` to
`putgd-sftp-askpass`, `SSH_ASKPASS_REQUIRE=force` and the socket and token variables. The
askpass sends the token, ssh's `SSH_ASKPASS_PROMPT` kind and the prompt, one line each; the
worker checks the peer's UID, compares the token in constant time and answers `ok` and the
secret, or nothing. A prompt naming a passphrase gets only the key passphrase, a password
prompt only the password, each at most once per connection; any other prompt (a one-time
code) gets nothing and makes the failure `ssh_needs_one_time_code`. A touch notice is
recorded (`touched` in the error details) and sent to the resident as a `notice` event; during
Test it becomes the setup job's event and Settings shows "Touch your security key". The socket and its directory are removed once the
connection is up. `putgd-sftp-askpass` is C and links only the C library, because ssh runs it
under the host's library path even when put.gd runs from its AppImage.

**Sessions.** A worker serves one destination: request lines carry an `id`, and events
(`progress`, `checkpoint`) and the one completion carry it back; `{"cancel":id}` and
`{"ack":…,"id":id}` are the control lines. The worker keeps its ssh connection between
requests and exits after 60 seconds with none. The provider keeps a worker that answered in
the last 50 seconds for the next request to the same destination (profile, settings,
credential reference and credential generation), so one connection serves an operation's name
check and upload and a burst of uploads; `anticipate` connects one ahead when an upload action
is invoked. A worker whose request was cancelled is not reused. A worker that sends nothing for
200 seconds, or doesn't finish a cancel within 30, is killed and a mutation's outcome is
unknown.

**Host keys.** The user's known_hosts files are read first, put.gd's own
`<config root>/known_hosts` second; put.gd never writes the user's. An unknown key fails the
connection with `ssh_host_unknown_key`. Test then probes: ssh with `BatchMode=yes`,
`StrictHostKeyChecking=accept-new`, `HashKnownHosts=no`, `CheckHostIP=no`, a new temporary
`UserKnownHostsFile`, `GlobalKnownHostsFile=/dev/null`, `PreferredAuthentications=none` and
`IdentityAgent=none` over the same route, so the key ssh accepted is written to that file and
no credential is offered. Its fingerprint (key type and unpadded base64 SHA-256 of the key
blob, as `ssh-keygen -l` prints it) goes into the error details. `trustHostKey` probes again and
writes the line to put.gd's file (replacing that host's earlier lines, through a temporary file,
mode 0600) only when the fingerprint is exactly the one given; the client records it in
`hostKeyFingerprint`, which changes the destination's check fingerprint. ssh's "Offending key in
FILE:LINE" says which file a changed key is in: put.gd's file is `ssh_trusted_key_changed` (old
and new fingerprints, re-trust), the user's `ssh_host_key_changed` (`ssh-keygen -R`). A key
that matches in either file is accepted, as ssh does. A ProxyJump host is checked by the user's
own ssh settings.

**Uploads.** The folder is checked first (`stat`, following a link to a folder); a missing one
is `sftp_folder_missing` with its absolute path, resolved from the login folder for a relative
path, and only `createFolder` (an explicit setup step for the saved folder) creates it, level by
level. The file opens under its final name with `WRITE|CREAT|EXCL` and no attributes, so the
server's umask applies; a refused open followed by an `lstat` that finds the name is a
collision. Writes are pipelined, up to 64 in flight, in chunks of the server's
`limits@openssh.com` write length capped at 64 KiB, else 32 KiB; progress counts acknowledged
bytes. A packet is never cut short by a cancel. The bytes sent are hashed and must match the
artifact's SHA-256. Then `fsync@openssh.com` when offered, close and `lstat`, which must show
the size sent. Once every byte is acknowledged a cancel is ignored and the upload finishes. A
cancel or failure before that closes and removes the file; a dropped connection can't, and the
upload is unknown. A write refused with the generic `SSH_FX_FAILURE` is `sftp_disk_full` only
when `statvfs@openssh.com` shows less free space than the file needs.

**Receipts.** `objectReference` holds `path` (absolute), `location` (`host:/path`, the notice and
Copy path), `size`, `mtime`, `uid` and `sha256`; `concurrencyToken` is the SHA-256 and
`directUrl` is Link address plus the percent-encoded name, or empty. `reconcile` reads a file of
the artifact's size back, up to 256 MB, and compares its SHA-256; a larger one is unknown.

**Deletion and capabilities.** `stat` and the check before removal need a regular file with the
recorded size, mtime and uid and, up to 32 MB, the recorded SHA-256; then `remove`. SFTP has no
conditional remove, so a file replaced between that check and the removal is removed too; the
window is one round trip, documented in `deletionEffect` as R2's ignored `If-Match` on DELETE
is. Capabilities: conditional create, conditional delete in that sense, stat, remove, 255-byte
names, and URLs only with a Link address; no replace, so a `condition` other than `absent` is
refused. `diagnosticSettings` shows only whether server, user, folder and Link address are set.

**Test.** connect, hostKey, signIn, folder, upload, link and cleanup steps, each with what it
proved. The test file `putgd-test-<uuid>.txt` is recorded in a checkpoint before it is created
and after; its link is fetched anonymously and compared byte for byte (skipped without a Link
address); cleanup uses the deletion check above. The test writes its journal on its own path,
as the custom uploader's does. Without a Link address the test passes without the link step.

