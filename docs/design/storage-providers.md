# Storage provider contract

Implement `StorageProvider` and register it in `ProviderRegistry`. Settings validation
must be pure and must not read source content, unlock secrets or contact the network.
No profile is accepted without its registered validator. The production registry has
no test provider. S3 implementation belongs exclusively under `src/storage/s3/`,
Backblaze B2's under `src/storage/b2/` ([b2.md](../user/b2.md)), Cloudflare R2's under
`src/storage/s3/` as a provider over the S3 one (`R2Provider`, [r2.md](../user/r2.md)) and the custom HTTP
uploader's under `src/storage/http/` ([custom-uploader.md](../user/custom-uploader.md)), Nextcloud's and
plain WebDAV's under `src/storage/dav/` ([nextcloud.md](../user/nextcloud.md)), and the SFTP
destination's under `src/storage/sftp/` ([sftp.md](../user/sftp.md), [below](#sftp)).

Nextcloud and WebDAV (#1872) are two provider IDs over one `DavProvider` class and one helper,
`putgd-webdav-worker`, which reuses the custom uploader's plain-HTTP rule
(`http::transportAllowed`). Uploads are a `PUT` with `If-None-Match: *` (a 412 is a collision);
Nextcloud files above 50 MiB use chunked upload v2 (`MKCOL` an upload folder, `PUT` chunks of 16 MiB by default, larger to stay within 10,000,
then `MOVE …/.file` with `Overwrite: F`); a `MOVE` with no answer is reconciled by the target's
size and content read back at the target's strong ETag (a failed probe leaves it unknown; a 412
stays a collision), and an abandoned upload folder is deleted through its journal, which keeps it
until the deletion is confirmed. The `MOVE` has its own 30-minute deadline and no stall limit, and
the helper sends a heartbeat while it waits. A file's identity is what the write itself returned:
the `PUT` or `MOVE` reply's strong ETag and Nextcloud's `OC-FileId` (its number, without the
padding). A server whose write returns no strong ETag (Apache returns none, and reports a weak
ETag for a file written within the last second) is asked for the file's strong ETag, and the bytes
at that ETag are read back (`GET` with `If-Match`) and compared; only then is the ETag recorded,
for files up to 256 MiB. A validator is exactly one quoted strong ETag (`"…"`, no `W/`, no `*`, no
list); anything else is neither recorded nor sent. Otherwise the receipt has no concurrency token and records
`identity: unconfirmed`: History can't delete it and Nextcloud makes no link for it. Receipts also
record the canonical DAV target (scheme, host, port, path) and, for Nextcloud, the account ID;
requests serialize on that target. `stat` compares the target and the ETag exactly as recorded (weak and strong never equal),
and the file ID and size when both the receipt and the server have them; deletion sends `If-Match` with the recorded ETag
unchanged, and the first 412 is `changed`, with no second attempt. Nextcloud has
`capabilities.sharing`: the link is a public read-only share (`shareType=3`, `permissions=1`, no
password) made in the two-step sharing pipeline, with the date an enforced expiry allows, and its
page URL is the `directUrl`. Every form value is percent-encoded, `+` included. put.gd's share
carries the label `put.gd <publication ID>`. Inspection, Finish share link and revoke find that
share by the share ID a step journaled (it stays the sharing record's progress whatever the
outcome, and History's link actions and retries pass it on), else by the recorded link's token (a
revoke from History passes a SHA-256 of the token, never the link, and the helper compares it
with each listed link's token locally and never sends it; only the share ID goes to Nextcloud,
and neither appears in History detail or share replies), else by the label, which must match exactly one share; a
lookup by ID must name exactly that share, and an ID it confirms gone is forgotten before the next
step. A confirmed absence means no share; several matches, an unreadable entry, or a link
listed without its token when put.gd's isn't found is unknown, never a guess, and revoke deletes
only a share found this way. A share that comes back as anything but a public read-only link of
the uploaded file (its `item_source`) is deleted again, and the file counts as without a link only
once that deletion is confirmed; otherwise the outcome is unknown and the journaled ID stays. A
create that can't go ahead (links off, an enforced password, a definite refusal, a share lookup
that failed before any create) answers `private` with the error and its HTTP and OCS statuses, so
History offers Finish share link. Capabilities are on, off or
unknown (an empty or malformed `files_sharing` is unknown); upload is refused beforehand only when
they confirm public links off. Upload and reconcile check the name check's and receipt's target
and account against the bound account, and write or claim nothing on a mismatch. Removing an
unfinished upload's chunk folder counts only when its `DELETE` succeeds or a 207 multistatus shows
every member removed. Test journals the Nextcloud folder it may create and the probes it sends
before them, and reports its file removed only once that is journaled. Share tokens are
secrets in a path: test journals keep receipts without the link, the journal boundary redacts
the whole `/s/<segment>` in link fields (DAV targets and identities stay), log redaction replaces
every `/s/<segment>`, diagnostics keep only the scheme and host of the server, folder and link
addresses, and server reply text never reaches a message,
journal or log (errors carry `httpStatus` and `ocsStatus`). TLS uses the system trust
store only; a refused certificate is described by issuer, expiry or host name, with its SHA-256
when one was received.
Credentials are bound to the origin they were given for and, for Nextcloud, the account ID
([credentials.md](credentials.md)); the provider refuses before sending anything when the
destination names another origin. Login Flow v2's page and poll endpoint must have the server's
scheme, host and port and a path below its base once dot segments are resolved, without user
information (Settings checks again before Open browser again); only a 404 while
polling means "not yet". Test fetches the link as a visitor through the custom uploader's
returned-address rules on every redirect.
Requests with file data have only a stall limit, as B2's. A read-back that checks a file's identity
sends no progress to the resident, so on a very slow link (about 270 KB/s or less for the 256 MiB
limit) the resident's 16-minute silence limit can stop it; during an upload that leaves the outcome
unknown with no receipt, as any lost reply does.

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
doesn't finish a cancel within 45, is killed and a mutation's outcome is unknown, with the path
of the worker's last checkpoint. Resident shutdown calls `SftpProvider::shutdown()` before it
waits for operations: running requests are cancelled and get at most 30 seconds from then, a
cancel already under way included.

**Client.** Every reply must answer a request still outstanding, exactly once; a reply put.gd
didn't ask for, or a second one, is a protocol error that ends the connection. One read pass
takes at most 1 MiB before it parses. Replies are bounded before use: a packet at most 1 MiB; a
version reply at most 64 extensions (names up to 256 bytes, data up to 4096); a status message
up to 4096 bytes and its language tag up to 256; a handle up to 256 bytes; a name list at most
1024 entries, each name up to 4096 bytes and its long name up to 8192. The folder setting
is at most 4096 bytes; a relative folder is joined to the login folder and the file name is appended,
so the path sent can be longer. At end of file the replies already read are still
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
isn't a symbolic link (a file not there yet is fine). put.gd never writes the user's. Ordinary
connections honour the user's `KnownHostsCommand`, but an upload needs a known_hosts line it can
record, so one whose server only that command vouches for fails before anything is sent. Keys never come from DNS or the server, localhost isn't exempt,
and there is no second check by IP address. A ProxyJump hop's own host key follows the user's
ssh settings, as in a terminal.

An unknown key fails the connection with `ssh_host_unknown_key`. Test then probes, after
askpass has stopped and with a fresh environment that has none: ssh with `BatchMode=yes`,
`StrictHostKeyChecking=accept-new`, `HashKnownHosts=no`, a new temporary `UserKnownHostsFile`,
`GlobalKnownHostsFile=/dev/null`, `PreferredAuthentications=none` and `IdentityAgent=none`
over the same route, so the key ssh accepted is written to that file and no credential is
offered to the destination (a jump host on the route still signs in with the user's own
settings). Its fingerprint (key type and unpadded base64 SHA-256 of the key blob, as
`ssh-keygen -l` prints it) goes into the error details. `trustHostKey` refuses when the user's
settings turn known_hosts off (`ssh_known_hosts_off`), when the host token isn't one literal
host (a `HostKeyAlias` pattern, list or negation: `ssh_host_alias_pattern`), and when the
user's files already know the host (`ssh_host_key_known`). Otherwise it probes again and keeps
the line only when it is recorded under that exact token with exactly the fingerprint given.
Writing holds an exclusive lock (`known_hosts.lock` in the same folder) across workers,
refuses a folder or file that is a symbolic link or not a regular one (checked with `lstat`),
reads the whole file and refuses one over 16 MiB rather than drop lines, replaces this host's earlier line only while
it still holds the fingerprint the destination recorded (else `ssh_host_key_mismatch`), writes
through a temporary file and checks the result is a regular 0600 file of this user. The
resident then records the fingerprint in the destination's `hostKeyFingerprint` itself,
conditional on the configuration revision the trust began with. The key is written first, so an
edit meanwhile leaves it trusted while the job says the fingerprint wasn't recorded; the record changes the
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
only while the configured folder joined to the login folder is still that path (symbolic links
aren't resolved again, so a parent link changed meanwhile can redirect it). The canonical (realpath) folder plus the
name is the path every name check, receipt, lock key and deletion uses. The path and the
verified server are checkpointed before the open. The file opens under its final name with
`WRITE|CREAT|EXCL` and no attributes, so the server's umask applies; once that request is sent
its reply is awaited for up to 20 seconds whatever a cancel says, so a handle that comes back is
never lost and the cleanup below can be attempted; a lost reply leaves the outcome unknown. Right after the open an `fstat`
records the new file's owner; it waits at most 5 seconds, and without an answer nothing can be
removed safely, so the outcome is unknown with the path. After a cancel or failure the CLOSE and REMOVE get 3 seconds on the
upload's own connection, sending included (a packet ssh can't take in time ends that
connection); on a slow link they wait behind the writes already in ssh's buffers
(#1888), so ssh is then ended and a new connection removes the file, only while it is still a
regular file of that owner and no larger than the upload. When the connection itself broke, put.gd goes
straight to that new connection, but only when the `fstat` after the open gave the file's complete
regular-file attributes (size, owner, times, type); otherwise nothing is removed and the outcome is
unknown with the path. A new `lstat` that finds no such file confirms the file is already gone. That connection is pinned as History's
checks are: only the known_hosts lines the attempt was verified by, and the same host, port and
user; otherwise nothing is removed. An upload keeps its password and
passphrase until it ends for that reconnect. The resident gives a cancelled request 45 seconds
before it stops the helper; Quit gives an active request at most 30, even with a cancel under way; cleanup that can't be confirmed is reported as unconfirmed, with the
path. When the `fstat` answer lacks the owner, type or times, CLOSE and REMOVE share one 8-second budget on the upload's own connection and no new connection is tried. A REMOVE answered "no such file" is reported as already gone, never as removed. A refused open followed by an `lstat` that finds the name is a collision, never
reconciled into ownership. Writes are pipelined, up to 64 in flight; progress counts only
acknowledged bytes, and no acknowledgement for two minutes ends the upload. A packet is never
cut short by a cancel. The bytes sent are hashed and must match the artifact's SHA-256. Then
`fsync@openssh.com` when offered, `fstat` of the handle (which must report size, owner, times
and permissions), a checkpoint of those plus the SHA-256, close, and an `lstat` that must show
the same regular file. Finalization starts once the server has acknowledged every byte; a cancel
from then on is ignored and the upload finishes. A cancel or failure before that is a cleanup
attempt as above, with the outcome removed, already gone or unconfirmed; its message says the
partial file was removed only when the server confirmed it, and otherwise names the path to
check. A write refused
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
SHA-256 checkpointed before the close, an `lstat` that matches them and a read-back with that SHA-256,
which is done only up to 256 MiB, so a larger file stays unknown. Without it the result is unknown with the path (`sftp_reconcile_unknown`); with
nothing journaled the attempt never opened a file, so the name is `missing` for this upload.

**Deletion and capabilities.** `stat`, the check before removal and `reconcile` connect with
`UserKnownHostsFile` set to a private temporary file holding only the recorded lines,
`GlobalKnownHostsFile=/dev/null`, `KnownHostsCommand=none` and `StrictHostKeyChecking=yes`, so
ssh itself proves the server satisfies the recorded entries: any recorded key, or a certificate
from a recorded authority (certificate authorities come only from known_hosts files, so the
recorded lines are all of them); any host-key failure there is `sftp_server_changed`, as is a
destination that now resolves to another host, port or user. A session connection verified by
exactly those lines (a test checking its own upload) is reused. A record whose server is known
only through a certificate authority has no display fingerprint (it is the first ordinary key
entry's, when there is one) and can still be deleted: the
recorded lines are what counts, in History and the worker alike. Then a
regular file with the recorded size, mtime and uid and, up to 32 MiB, the recorded SHA-256. The
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
