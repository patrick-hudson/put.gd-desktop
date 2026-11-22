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
`LC_ALL=C`, put.gd's own variables and any askpass removed and the AppImage's library
overrides undone (`host::environment()`), and these options:

```
-T -x -a -o ClearAllForwardings=yes -o RequestTTY=no -o RemoteCommand=none
-o PermitLocalCommand=no -o EscapeChar=none -o ControlMaster=no -o ControlPath=none
-o UpdateHostKeys=no -o NoHostAuthenticationForLocalhost=no -o VerifyHostKeyDNS=no
-o CheckHostIP=no -o KbdInteractiveAuthentication=no
-o StrictHostKeyChecking=yes -o NumberOfPasswordPrompts=1 -o BatchMode=no
-o ServerAliveInterval=15 -o ServerAliveCountMax=4 -o ConnectTimeout=20
-o UserKnownHostsFile=<see Host keys>
-s [-p port] [-l user] -- host sftp
```

`BatchMode` stays off because it disables askpass. Keyboard-interactive is off so a server's
own prompts never reach askpass. A check of a recorded upload adds
`GlobalKnownHostsFile=/dev/null` (see Receipts). `ssh -G` with the same destination arguments gives the resolved host, port,
user, `HostKeyAlias`, the key files that exist (tilde-expanded), `IdentityAgent`, whether a
ProxyJump or ProxyCommand is set (never its value), and the user's `UserKnownHostsFile` and
`GlobalKnownHostsFile`, which are split on spaces, so a path with a space in it isn't
supported. A known_hosts path that still holds a `%` token, a `${VARIABLE}` or a `~user` after put.gd's own
`~/` expansion can't be read the way ssh reads it: put.gd fails closed with
`ssh_known_hosts_unreadable` and nothing connects. The agent is `SSH_AUTH_SOCK`, else the first socket of
`$XDG_RUNTIME_DIR/{gcr/ssh, keyring/ssh, ssh-agent.socket, openssh_agent,
gnupg/S.gpg-agent.ssh}`. Only stable startup failures in ssh's stderr map to codes (unknown or
changed host key and in which file, permission denied, refused, timed out, host not found,
subsystem refused); a server whose only remaining method is `keyboard-interactive`, or a prompt
askpass doesn't answer, is `ssh_needs_one_time_code`; a stream that starts with something other
than an SFTP packet (shell startup output) is `ssh_no_sftp`; anything else is `ssh_failed` with
ssh's exit status. No error, test journal or diagnostics package carries ssh's stderr or the
server's SFTP status text: messages are put.gd's own words, the SFTP status named
(`permission denied`, `failure`, …), and the known_hosts file put.gd itself chose to check.

**Askpass.** ssh closes inherited descriptors, so a secret can't travel on a pipe. Before each
connection the worker opens a Unix socket in a new 0700 directory under `$XDG_RUNTIME_DIR`
(else the temporary directory), with a 32-byte random token, and sets `SSH_ASKPASS` to
`putgd-sftp-askpass`, `SSH_ASKPASS_REQUIRE=force` and the socket and token variables. The
askpass sends the token, ssh's `SSH_ASKPASS_PROMPT` kind and the prompt, one line each; the
worker checks the peer's UID, compares the token in constant time and answers `ok` and the
secret, or nothing. A secret goes only to the ssh process the worker started: the asking
process's parent (`SO_PEERCRED`, then `/proc/<pid>/stat`) must be that ssh, so the askpass of a
jump host's ssh or of a ProxyCommand's child gets nothing. Answers go only to OpenSSH's own
local prompts, compared as whole strings, the trailing `: ` included, with no trimming,
truncation or prefix match (`sftp::matchPrompt`): the password to `<user>@<host>'s password: `
for the resolved user and host or `HostKeyAlias`, the passphrase to
`Enter passphrase for key '<file>': ` for a key file the destination's settings name, each at
most once per connection. The key path is cut to its first 100 bytes, as OpenSSH's `%.100s` cuts
it; when two configured key files share those 100 bytes the prompt can't name one, nothing is
answered, and the error says so. A ProxyJump hop's password prompt, a server's keyboard-interactive
text or a PIN prompt gets nothing. A touch notice is recorded (`touched` in the error details)
and sent to the resident as a `notice` event; during Test it becomes the setup job's event and
Settings shows "Touch your security key". When sign-in ends, either way, the socket and its
directory are removed and the worker wipes both secrets; the request's own credential fields are
taken out of it as it arrives and dropped once read. Only requests that sign in carry the
password and passphrase from the resident; Settings' address check (`inspect`) gets only
whether a password is set. `putgd-sftp-askpass` is C and links only the C library, because ssh
runs it under the host's library path even when put.gd runs from its AppImage.

**Sessions.** A worker serves one destination: request lines carry an `id`, and events
(`progress`, `checkpoint`, `notice`) and the one completion carry it back; `{"cancel":id}` and
`{"ack":…,"id":id}` are the control lines. A cancel holds from the moment it arrives, also for
a request still queued, which then starts no ssh. The worker keeps its ssh connection between
requests and exits after 60 seconds with none. The provider keeps a worker that answered in
the last 50 seconds for the next request to the same destination (profile, settings,
credential reference and credential generation), so one connection serves an operation's name
check and upload and a burst of uploads; a reaper thread retires idle workers every 10 seconds
whether or not their destination is asked again. `anticipate` connects one ahead when an
upload action is invoked; the provider joins those threads before it is destroyed. A worker
whose request was cancelled is not reused. A worker that sends nothing for 200 seconds, or
doesn't finish a cancel within 30, is killed and a mutation's outcome is unknown.

**Client.** Every reply must answer a request still outstanding, exactly once; a reply put.gd
didn't ask for, or a second one, is a protocol error that ends the connection. One read pass
takes at most 1 MB before it parses. At end of file the replies already read are still
delivered; only requests without one fail. The server must answer version 3 exactly.
Extensions are used only at the versions put.gd implements (`fsync@openssh.com` 1,
`statvfs@openssh.com` 2, `limits@openssh.com` 1); `limits@openssh.com` bounds read and write
chunks by its read, write and packet sizes (with the packet header), capped at 64 KiB, else
32 KiB.

**Host keys.** When the user's own files (`UserKnownHostsFile` and `GlobalKnownHostsFile`, each
looked up with `ssh-keygen -F` for the host token: `HostKeyAlias`, the host, or `[host]:port`)
have any key for the server, `UserKnownHostsFile` is only the user's files, and their entry
decides alone. Otherwise put.gd's own file is added after them. A lookup that fails (ssh-keygen
missing, a file that can't be read, an exit status other than found or not found) fails closed
with `ssh_known_hosts_unreadable`. `UserKnownHostsFile none` is passed on as `none`. put.gd's
file is `<config root>/ssh/known_hosts`, in its own 0700 folder; before it is handed to ssh an
`lstat` must show a regular file of this user's with mode 0600 in a folder of this user's that
isn't a symbolic link (a file not there yet is fine). put.gd never writes the user's. Keys never come from DNS or the server, localhost isn't exempt,
and there is no second check by IP address. A ProxyJump hop's own host key follows the user's
ssh settings, as in a terminal.

An unknown key fails the connection with `ssh_host_unknown_key`. Test then probes, after
askpass has stopped and with a fresh environment that has none: ssh with `BatchMode=yes`,
`StrictHostKeyChecking=accept-new`, `HashKnownHosts=no`, a new temporary `UserKnownHostsFile`,
`GlobalKnownHostsFile=/dev/null`, `PreferredAuthentications=none` and `IdentityAgent=none`
over the same route, so the key ssh accepted is written to that file and no credential is
offered. Its fingerprint (key type and unpadded base64 SHA-256 of the key blob, as
`ssh-keygen -l` prints it) goes into the error details. `trustHostKey` refuses when the user's
settings turn known_hosts off (`ssh_known_hosts_off`), when the host token isn't one literal
host (a `HostKeyAlias` pattern, list or negation: `ssh_host_alias_pattern`), and when the
user's files already know the host (`ssh_host_key_known`). Otherwise it probes again and keeps
the line only when it is recorded under that exact token with exactly the fingerprint given.
Writing holds an exclusive lock (`known_hosts.lock` in the same folder) across workers,
refuses a folder or file that is a symbolic link or not a regular one (checked with `lstat`),
reads the whole file and refuses one over 16 MB rather than drop lines, replaces this host's earlier line only while
it still holds the fingerprint the destination recorded (else `ssh_host_key_mismatch`), writes
through a temporary file and checks the result is a regular 0600 file of this user. The
resident then records the fingerprint in the destination's `hostKeyFingerprint` itself,
conditional on the configuration revision the trust began with; the record changes the
destination's check fingerprint, so an earlier Test no longer vouches. A changed key is in the
user's file when their files had an entry for the server, `ssh_host_key_changed`, which names
that file and `ssh-keygen -R`; otherwise only put.gd's file can hold it, `ssh_trusted_key_changed`
(old and new fingerprints, re-trust). Neither repeats ssh's own lines. A trusted
key stays in put.gd's file after its last destination is removed, a harmless leftover.

**Uploads.** The folder is resolved first (`realpath`, then `stat` must show a folder); a
resolved path that isn't valid UTF-8, or doesn't come back byte for byte, is refused before
anything is written (`sftp_path_unsupported`); a
missing one is `sftp_folder_missing` with the path as the settings name it, resolved from the
login folder for a relative path, and only `createFolder` (an explicit setup step for the saved
folder, up to 4096 bytes, given with the path its Test reported) creates it, level by level, and
only while the folder still resolves to that path. The canonical (realpath) folder plus the
name is the path every name check, receipt, lock key and deletion uses. The path and the
verified server are checkpointed before the open. The file opens under its final name with
`WRITE|CREAT|EXCL` and no attributes, so the server's umask applies; once that request is sent
its reply is awaited for up to 20 seconds whatever a cancel says, and a handle that comes back
is closed and the file removed; a lost reply leaves the outcome unknown. Right after the open an `fstat`
records the new file's owner. After a cancel or failure the CLOSE and REMOVE get 3 seconds on the
upload's own connection; on a slow link they wait behind the writes already in ssh's buffers
(#1888), so ssh is then ended and a new connection removes the file, only while it is still a
regular file of that owner and no larger than the upload. An upload keeps its password and
passphrase until it ends for that reconnect. The resident gives a cancelled request 45 seconds
before it stops the helper; cleanup that can't be confirmed is reported as unconfirmed, with the
path. Without the `fstat` answer, CLOSE and REMOVE share one 8-second budget on the upload's own connection. A REMOVE answered "no such file" is reported as already gone, never as removed. A refused open followed by an `lstat` that finds the name is a collision, never
reconciled into ownership. Writes are pipelined, up to 64 in flight; progress counts only
acknowledged bytes, and no acknowledgement for two minutes ends the upload. A packet is never
cut short by a cancel. The bytes sent are hashed and must match the artifact's SHA-256. Then
`fsync@openssh.com` when offered, `fstat` of the handle (which must report size, owner, times
and permissions), a checkpoint of those plus the SHA-256, close, and an `lstat` that must show
the same regular file. Once every byte is acknowledged a cancel is ignored and the upload
finishes. A cancel or failure before that closes and removes the file; its message says the
partial file was removed only when the server confirmed it, and otherwise names the path to
check. A dropped connection can't remove it, and the upload is unknown. A write refused
with the generic `SSH_FX_FAILURE` is `sftp_disk_full` only when `statvfs@openssh.com` shows less
free space than the file needs.

**Receipts.** `objectReference` holds `path` (canonical, absolute), `location` (`host:/path`,
the notice and Copy path), the server (`host`, `port`, `user` as `ssh -G` resolved them, and
`knownHosts`: the known_hosts lines ssh would accept for it, found with `ssh-keygen -F` in the
files that connection checked, the user's, global and put.gd's, hashed names, patterns and
`@cert-authority` lines included), `hostKeyFingerprint` (shown only, computed from the recorded
line), and `size`, `mtime`, `uid` and `sha256` from the upload's own handle. An upload whose
lines can't be found fails closed before anything is sent. ssh's own output is never read for the
server's identity: a ProxyCommand or jump host could forge or displace it; `concurrencyToken` is the SHA-256 and `directUrl` is Link address plus the
percent-encoded name, or empty. The lock key and History's identity are the resolved host, port,
user and canonical path. `reconcile` claims a file only with the attempt's own journaled
evidence: the path checkpointed before the open, then the handle's size, mtime and uid and the
SHA-256 checkpointed before the close, an `lstat` that matches them and, up to 256 MB, a read-back
with that SHA-256. Without it the result is unknown with the path (`sftp_reconcile_unknown`); with
nothing journaled the attempt never opened a file, so the name is `missing` for this upload.

**Deletion and capabilities.** `stat`, the check before removal and `reconcile` connect with
`UserKnownHostsFile` set to a private temporary file holding only the recorded lines,
`GlobalKnownHostsFile=/dev/null`, `KnownHostsCommand=none` and `StrictHostKeyChecking=yes`, so
ssh itself proves the server still holds the recorded key (certificate authorities come only from
known_hosts files, so the recorded lines are all of them); any host-key failure there is `sftp_server_changed`, as is a
destination that now resolves to another host, port or user. A session connection verified by
exactly those lines (a test checking its own upload) is reused. A record whose server is known
only through a certificate authority has no display fingerprint and can still be deleted: the
recorded lines are what counts, in History and the worker alike. Then a
regular file with the recorded size, mtime and uid and, up to 32 MB, the recorded SHA-256. The
intent is checkpointed through the deletion's durable checkpoint before that file check, so
waiting for the journal never makes the check stale, an
`lstat` right before `REMOVE` must still match, and an unclear outcome keeps the intent as the
deletion's progress. A receipt without the server identity and file fields can't be deleted
(`receiptCapabilities` turns removal off, `sftp_delete_unavailable`). SFTP has no conditional
remove, so a file replaced between that last check and the removal is removed too; the window is
one round trip, documented in `deletionEffect` as R2's ignored `If-Match` on DELETE is. Not
guarded against: another writer's byte-identical file with the same size, time and owner, and a
file swapped in at the path while put.gd's own exclusive handle is open. Capabilities:
conditional create, conditional delete in that sense, stat, remove, 255-byte names, and URLs only
with a Link address; no replace, so a `condition` other than `absent` is refused.
`diagnosticSettings` shows only whether server, user, folder and Link address are set. Without a
Link address, capture summaries say "Upload only", never "Shared link".

**Test.** connect, hostKey, signIn, folder, upload, link and cleanup steps, each with what it
proved. The test file `putgd-test-<uuid>.txt` is recorded in a checkpoint before it is created;
`fsync`, `fstat`, close and the `lstat` after it are all checked, and the uploaded receipt is
recorded only when that `lstat` confirmed it. A name that holds another file by then gets no
`REMOVE`, and a lost reply to the test file's open leaves the file unknown: both report cleanup
`unconfirmed` with `cleanupPath`. Its link is fetched anonymously and compared byte for byte
(skipped without a Link address); cleanup uses the deletion check above, issues no `REMOVE` for a
file that isn't the test's, and is `removed` or `missing` only when the server confirms, else
`unconfirmed` with `cleanupPath`, which fails the test with the file's path. The result's
`summary` is given only when every step passed. The test writes its journal on its own path, as the custom
uploader's does. Without a Link address the test passes without the link step.
