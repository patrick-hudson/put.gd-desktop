# Architecture

put.gd is a Linux desktop application with one resident operation engine and
on-demand graphical and network helpers. [Overview](overview.md) maps the
features, executables and documentation; this page describes their boundaries
and the behavioral contracts shared by GUI, shortcuts and agent clients.

## Process boundaries

`putgd` owns `SettingsService`, `ProviderRegistry`, the native `SourceAdapter`,
`OperationService`, `HistoryService`, `RecordingService`, `SetupService`,
`UpdateService`, the tray and the private `LocalServer` command endpoint.
The production registry contains S3, Backblaze B2 and Proton Drive. Shared core,
GUI, history and platform contracts are provider-neutral; AWS headers stay under
`src/storage/s3/`.

Cinnamon/X11, GNOME Wayland and KDE Wayland use `QCoreApplication` residents.
`src/platform/NativeSession.h` guards routing before application construction;
the adapters independently verify their native connections and capabilities.
Other or ambiguous routes retain a `QApplication` fallback, including GNOME/X11
and Plasma/X11. Its generic platform theme avoids eager desktop theme loading;
the temporary environment change is restored before helpers start.
QtGui remains linked for image and key-sequence support on Core routes.

| Desktop route | Resident ownership | Transient graphical work |
| --- | --- | --- |
| Cinnamon/X11 | Native XCB shortcuts, target/layout snapshots and `X11Clipboard`; session-bus tray | `putgd-capture` selects and captures against the frozen target |
| GNOME Wayland | `GnomeAdapter` authenticates the GNOME Shell bridge over the session bus; the extension owns Shell selection, capture, shortcuts and clipboard MIME transfer | Shell actors provide selection; `putgd-ui` provides application windows |
| KDE Wayland | Independent native Wayland output/data-control connections, KGlobalAccel and KWin ScreenShot2; session-bus tray | `putgd-kde-region` provides the LayerShellQt selector; KWin pixel capture stays in the resident, preserving executable authorization |

Targets and display layouts are frozen at invocation. A later focus change never
retargets a window capture. Layout changes invalidate acquisition; app-owned
windows are excluded. Region selection is live by default. With
`preferences.regionSelectionMode=screenshot_first`, pixels are acquired after
durable authorization and before selection, and the result is cut from those
frames. On KDE the selector can receive a sealed background memfd; it still
returns geometry rather than capturing pixels itself. Helper protocols validate
build identity, nonce, generation, bounds, message order and normal exit.
Missing required selector-cleanup proof fails closed. See the
[desktop contracts](overview.md#desktop-adapters) for compositor-specific details.

`UiProcessController` starts one matching `putgd-ui` window helper on demand.
`UiApplicationController` owns the main History/Destinations/Settings window and
About. `UiSession` caches JSON and bounded thumbnails; it owns no SQLite
connection, provider or credential store. Commands use the same resident endpoint
as the CLI. Private pipes carry bootstrap, activation, invalidation and validated
RAM-only checkpoints. The last ordinary window close completes the checkpoint
handshake and exits the helper; accepted work and clipboard ownership continue.
Explicit Quit shuts down the helper and resident. Minimize keeps the helper alive.
See [protocol](ui-process-protocol.md) and [checkpoint state](ui-process-state.md).

Separate `putgd-ui --edit-image` invocations own image editing. The original stays
private until explicit submission; only the sanitized PNG enters the shared
pipeline. `putgd-ui --image-job` performs generic codec discovery, decoding and
preview work offscreen without providers, credentials or application windows.
The parent validates private file identity, limits, hashes and normal exit.
Known PNG readers verify the signature and disable automatic format discovery.
See [image editor](image-editor.md) and [image worker](image-worker.md).

`putgd-record` owns GStreamer acquisition, the working clip, overlays, review,
previews and FFmpeg export. `RecordingService` owns the authoritative session,
durable intent and edit revision. Stop opens private review; preview, editing,
Save a copy, cancellation and restart never authorize upload. Only explicit
Upload admits a validated export to `OperationService`. The editor and recording
session exclude one another. See [recording](recording.md).

A resident belongs to its login session. `SessionWatch` observes logind; loss of
the resident's X server also triggers shutdown. A launcher hands off to a live
resident and can retire an exact identified resident from an ended graphical
session. Failure to open a window never authorizes a second resident.

## Acquisition, artifacts and publication

`OperationService` owns acceptance, acquisition, naming, immutable artifacts,
local archive copies, publication, retries and result copying. `HistoryService`
owns offline browsing, explicit copies, sharing and separately identified remote
deletion. Hotkeys, windows and JSON clients use these services.

Acceptance snapshots the action, configuration revision, complete non-secret
settings, original destination and credential reference. The operation ID is
returned after durable acceptance and identifies work independently of the client connection.
Native target preparation can precede journal I/O, and a selector can collect
geometry early, but pixel acquisition waits for both durable checkpoints.
Interactive ownership is tied to that operation ID; cancelling an unrelated
upload cannot cancel the current selector.

Acquired file batches commit every private spool before the first publication.
Aggregate acquisition is serialized against the spool budget while publication
can run concurrently under shared upload slots. A bounded private manifest
recovers committed artifacts if the item checkpoint is interrupted. Journal
ownership precedes recovery; incomplete staging is discarded before new work.
File imports stream in bounded chunks and uploads stream from retained artifacts.
PNG encoding, file copying, SQL, configuration I/O and provider work run on workers.

Clipboard acquisition never fetches URLs: local file URLs take precedence over
image MIME, then text. A plain-text URL is text. Finalized image, recording,
text and arbitrary-file payloads share the artifact/archive/upload pipeline.
File-manager selections enter through `uploadFiles`; folder handling is described
in [file-manager upload](file-manager-upload.md).

The operation journal and per-item history commit in one SQLite transaction.
An unknown publication has no invented URL or deletion target. Recovery marks
interrupted uploads unknown and never automatically publishes. Explicit retry
retains artifacts, candidate names, conditions and publication markers, reconciles
unknown results first and skips completed uploads. Archive failure is independent
of publication. Local-only actions never call a provider. Archives are unique,
atomic user-owned files; retries read private hash-verified spools rather than
mutable archive copies.

## Storage and secrets

`StorageProvider` declares capabilities, name checks, upload, reconciliation,
stat, removal and optional sharing. Receipts preserve the original destination,
credential reference, opaque object identity, publication marker and direct URL
separately. Core and history never reconstruct an object from its URL or route
an old receipt through an edited profile. Validation performs no network or
credential-unlock work. See [provider contract](storage-providers.md).

S3 is lazy: on Linux, each driver call starts `putgd-s3-worker`; the resident
does not load AWS libraries. The worker loads the private module from the exact
build/install path, checks its ABI/build/transport identity, and owns SDK
initialization, clients, requests and shutdown on its SDK thread. Module code
stays pinned for that process lifetime; normal worker exit releases its mappings.
Dispatch lanes and admission are bounded. SDK automatic retries are disabled;
the adapter reconciles its own bounded attempts. A crashed, timed-out or abnormal
worker never authorizes blind replay of a possible mutation. See [S3](s3.md).

`putgd-b2-worker` performs B2 HTTP in a fresh process per request. Proton uses the
privately packaged `putgd-proton-runtime`, with account operations serialized by
the provider. Their distinct collision, version and sharing semantics remain
behind the common receipt contract. See [B2](b2.md) and [Proton](proton-drive.md).

`SetupService` owns explicit credential storage/unlock and destination/account
setup. Configuration and journals contain references, never secret inputs.
S3/B2 use `CredentialStore`; Proton account and share-password storage uses its
helper's secret-service namespace. Startup can read saved credentials; uploads
do not initiate unlock. `UpdateService` delegates checks and AppImage installation
to short-lived `putgd-cli` children, keeping TLS out of the idle resident.

## Clipboard result policy

Upload copying follows issue #32. `preferences.uploadClipboardBehavior` defaults
to `each_link`, including legacy configurations. A confirmed link becomes eligible
after its durable item checkpoint. Batch links copy in source order: each ready
link waits for earlier files to settle. A separate sharing step, as on Proton,
must settle before that file advances the ordering cursor (#478).

`list` groups overlapping operations with the same saved mode, explicit-copy
epoch and expected clipboard revision. It copies available links in invocation
and item order after every member settles. Durable receipts are refreshed before
copying so known removed or revoked links are excluded.

All clipboard commits are serialized. Native adapters acknowledge the exact
revision of a successful application write; only pending copies that expected
the overwritten revision advance to it. Later user edits remain distinct even
before a queued callback runs. New uploads do not invalidate earlier upload-link
copies. Explicit operation/history copies and local-only actions advance the
upload-copy epoch. Clipboard imports reserve their source until acquisition ends;
confirmed unsent native suppression during a new import is paused and retried.

Copy outcomes wait for their individual commits or list group. Partial copying
uses `copyStatus=partial`; failed or suppressed copying never causes reupload.
Publication receipts stay durable independently. Local-only MIME copying retains
its latest-invocation/revision guard. Legitimate native clipboard ownership can
retain encoded image bytes and bounded incremental transfers until release.

## History and destructive actions

History uses indexed SQLite filters and bounded LIMIT/OFFSET pages of 1–200
records. It does not fetch remote content. Private image/video previews have a
256-pixel edge and an independently evictable cache; missing previews can be
regenerated from verified retained bytes. SQL connections belong to their worker
threads. Schema upgrades are transactional; invalid legacy data preserves the
previous schema and records.

Clearing history neither contacts providers nor removes files. It preserves
pending remote-management identities and uses tombstones so later status writes
cannot resurrect cleared rows. Remote-management fields remain independent of
upload, local-save and copy-status updates. Default diagnostics packages exclude
payload bytes and the raw history database, redact secrets/references and hash
names and URLs unless explicitly included; see [diagnostics](diagnostics.md).

Deletion preparation binds exact selected IDs, immutable receipts and
provider-declared effects into an acknowledgement. Confirmation revalidates that
set and journals it before network work. Mutations serialize by provider-owned
destination/object identity, without adopting replacements. Drivers await durable
checkpoint acknowledgement before each destructive step. Failed, unknown or
partial deletion retains its original receipt and versioned opaque progress for
explicit retry. Cancellation cannot undo a sent mutation. Shutdown cancels queued
targets and drains sent work before history services are destroyed. Browser/CDN
cache invalidation is outside this pipeline.

## Linux command transport and idle memory

Linux uses native Unix-domain sockets with Qt Core socket notifications, rather
than an eager Qt Network dependency. The runtime directory is owner-only and the
socket is 0600. Live or indeterminate endpoints are preserved; stale cleanup
requires an owned socket with an explicitly refused/missing endpoint. Complete
socket paths fit within 107 encoded bytes, excluding NUL. Lab launches preflight
resolved UTF-8 paths with `tools/diagnostics/check_socket_paths.py`.

Requests have a 1 MiB limit, ten-second framing deadline and 64-client admission
limit. Nonblocking writes drain replies to slow readers. Bounded input draining
continues after dispatch to detect disconnects and release admission slots;
lifetime-guarded late replies are harmless. Schema-1 commands serve `putgd-cli`,
`UiSession` and the optional MCP helper. See [CLI](cli.md) and [MCP](mcp.md).

On glibc Linux, startup recovery, accepted acquisition and explicit local
image/text copying mark allocator reclamation pending. One two-second quiet
timer waits for selectors, operations, copy queues, workers, payload leases and
recording to drain, then calls `malloc_trim(0)` once. New background work stops
the timer. Live clipboard data and widgets are preserved. Diagnostics expose the
pending flag, count and duration; shutdown stops the timer. The fixed allocator
policy and whole-family measurement budget are in [memory strategy](memory-strategy.md).
