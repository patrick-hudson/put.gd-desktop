# Shared operation services

The current process and memory design is summarized in [resident memory strategy](memory-strategy.md), with exact source63 measurements and remaining release gates.

`OperationService` owns acceptance, acquisition, immutable artifacts, naming, archive,
publication, explicit retries and result copying. GUI/hotkeys and JSON session clients
use this service. `HistoryService` owns offline browsing, explicit link copying and
separately identified remote deletion. Neither service includes AWS headers.

The resident process owns one `SettingsService`, provider registry, native source
adapter, operation service, history service and owner-only local command server. It registers
the S3 provider and selects the X11, GNOME Wayland or KDE Wayland adapter from the
actual Qt/session backend. Unsupported sessions use an unavailable adapter. Diagnostics
report actual capture, shortcut and clipboard capabilities separately. Unit fixtures
register an opaque non-S3 provider only in tests; these tests do not prove compositor
or cloud support.

`putgd-ui` owns the optional Settings, History, Activity and About windows. The
resident owns its tray/menu and uses `UiProcessController` to start one matching
helper, acknowledge visible views and retain a bounded safe draft checkpoint.
`UiSession` forwards all commands to the same resident endpoint as the CLI; it
owns no SQLite connection, provider or credential store. Last-window close exits
the helper after a validated checkpoint, while resident actions and uploads remain
active. Explicit Quit uses the existing resident shutdown path. Protocol/state
limits and extension rules are in [UI process protocol](ui-process-protocol.md).

The S3 provider facade has no eager AWS dependency. Its first network request loads
a private module from the exact build/install path and validates its application,
source, toolchain, Qt and transport identity before creating a driver. A mismatch
requires reinstalling a matching package and restarting the resident. The module
stays pinned. A lazy permanent worker owns SDK initialization, client/request
lifetimes and SDK shutdown on the same thread, with bounded admission and client
caching. This is internal packaging; the provider registry remains static and
provider-neutral. Local-only startup does not map AWS or its curl dependency.

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

On glibc Linux, successful startup recovery, accepted capture/clipboard actions
(including cancellation) and explicit local image/text copying mark an
idle allocator cleanup as pending. A single two-second quiet timer waits until
selectors, active/queued operations, tracked background threads and image leases
have drained. New background work stops the timer. It calls the public
`malloc_trim(0)` once, returning free allocator pages without discarding live
widgets, native fonts/dialogs or clipboard MIME. Other allocators do not run
this pass. Redacted diagnostics expose pending/count/duration for reproduction;
idle cleanup has no repeating timer and stops during shutdown.

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

## Linux local command transport

The Linux resident uses Unix-domain sockets with Qt Core
socket notifications. The versioned JSON protocol, CLI and MCP interfaces stay
the same. The CLI and the lazy S3 module may use Qt Network; the Linux resident
has no eager Qt Network link requirement. Other platforms retain Qt local sockets.

The runtime directory is owner-only; the socket is mode 0600. A live or
indeterminate endpoint is preserved. Only an explicitly refused/missing owned
socket is eligible for stale cleanup. Requests retain their 1 MiB limit,
10-second framing deadline and 64-client limit. Replies drain through a
nonblocking write notification, including when the client reads slowly.
After a request is dispatched, bounded input draining continues to observe peer
disconnection while its reply is pending. Disconnected clients release their
admission slot; guarded late replies are harmless. The reviewed transport is
integrated in commit `1fdfff0`.
