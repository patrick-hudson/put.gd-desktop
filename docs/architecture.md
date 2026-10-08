# Shared operation services

`OperationService` owns acceptance, acquisition, immutable artifacts, naming, archive,
publication, explicit retries and result copying. GUI/hotkeys and JSON session clients
use this service. `HistoryService` owns offline browsing, explicit link copying and
separately identified remote deletion. Neither service includes AWS headers.

The resident process owns one `SettingsService`, provider registry, native source
adapter, operation service, history service and owner-only `QLocalServer`. U2's resident
source adapter reports every native action/result/shortcut unavailable. The registry is
empty until real providers are registered. Unit fixtures register an opaque non-S3
provider only in tests; these tests do not prove compositor or cloud support.

Acceptance stores the action, configuration revision, complete non-secret settings,
original destination and credential reference before source acquisition starts. The
client receives its operation ID before interactive selection. Acquired file batches
commit all private spools before the first publication. Aggregate acquisition is serialized
against the spool budget while publication may run concurrently. A bounded private
manifest recovers committed artifacts whose item checkpoint was interrupted. Resident
journal ownership precedes recovery; abandoned staging files are removed before any
new acquisition can start. PNG encoding, file copying,
SQL, history queries, runtime configuration I/O and provider work run on workers. Acquisition never fetches URLs:
clipboard file URLs take precedence over image MIME, which takes precedence over text.
Finalized image/video/text/other files all use the same pipeline.

The per-operation journal and per-item history commit in one SQLite transaction.
Unknown publication has no invented URL or delete target. Restart changes interrupted
in-flight PUTs to unknown and never automatically publishes. Explicit retry retains
artifacts, candidates, conditions and publication markers, reconciles unknown results
first and skips completed uploads. Archive failure is independent of publication.
Local-only actions never call a provider. Archives are unique atomic user-owned files;
retry reads private hash-verified spools, never a mutable archive.

Automatic copy requires the latest invocation ticket and unchanged clipboard revision.
Explicit operation/history copying invalidates previous automatic eligibility. Upload
receipts survive copy suppression/failure. Interactive selection is guarded by its
operation ID; cancelling another upload cannot cancel the active selector.

History uses indexed SQLite filters and bounded LIMIT/OFFSET pages (1–200 records),
without loading remote content. Image previews are private, bounded 256-pixel cache
files generated from acquired bytes, independently evictable and never remotely fetched. SQL connections are private to their worker thread.
Schema upgrades run transactionally; an invalid legacy record preserves the old
schema and records. Clearing history never contacts a provider or removes files, retains
pending deletion identities, and uses tombstones so later copy-status updates cannot
resurrect deliberately cleared entries. Remote-management fields remain independent of
upload/local-save/copy updates. Default diagnostics exclude history, names, URLs,
settings/credential references, spools and payloads.

Deletion preparation binds exact selected IDs, immutable receipts and provider-declared
effects into an acknowledgement. Confirmation revalidates that set, journals it before
network work and serializes mutations using provider-owned destination/object identity.
Identity checks never adopt a replacement. Drivers must await successful durable
checkpoints before each destructive step. Failed/unknown/partial deletion retains its
original receipt and versioned opaque progress for explicit retry. Cancellation cannot
undo an already sent mutation. Shutdown cancels queued deletion targets and drains
the sent request before destroying history services, retaining its truthful result.
Browser/CDN cache invalidation is outside this pipeline.
