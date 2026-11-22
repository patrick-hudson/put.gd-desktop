# Storage provider contract

Implement `StorageProvider` and register it in `ProviderRegistry`. Settings validation
must be pure and must not read source content, unlock secrets or contact the network.
No profile is accepted without its registered validator. The production registry has
no test provider. S3 implementation belongs exclusively under `src/storage/s3/`,
Backblaze B2's under `src/storage/b2/` ([b2.md](../user/b2.md)), Cloudflare R2's under
`src/storage/s3/` as a provider over the S3 one (`R2Provider`, [r2.md](../user/r2.md)) and the custom HTTP
uploader's under `src/storage/http/` ([custom-uploader.md](../user/custom-uploader.md)), and Nextcloud's and
plain WebDAV's under `src/storage/dav/` ([nextcloud.md](../user/nextcloud.md)).

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
limit) the resident's 16-minute silence limit can stop it, and the identity stays unconfirmed.

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
