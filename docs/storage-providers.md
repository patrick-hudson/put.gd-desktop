# Storage provider contract

Implement `StorageProvider` and register it in `ProviderRegistry`. Settings validation
must be pure and must not read source content, unlock secrets or contact the network.
No profile is accepted without its registered validator. The production registry has
no test provider. S3 implementation belongs exclusively under `src/storage/s3/`, and
Backblaze B2's under `src/storage/b2/` ([b2.md](b2.md)).

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
required safeguards fail explicitly. `checkName` returns absent, present or inconclusive,
an opaque target reference and an opaque precondition. Denied/time-limited checks are
never treated as available names. Conditional PUT is the final race protection.

A version-1 receipt preserves provider/profile/revision, original non-secret destination,
credential-store/session reference, versioned opaque object reference, concurrency token,
publication marker and direct URL as separate fields. Core/history/CLI never parse the
URL, assume bucket/key/ETag fields, or reroute through current profiles. Upload results
are uploaded, collision, failed or unknown. Reconciliation is own, foreign, missing or
unknown and must inspect the saved publication identity before any retry/rename.

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
