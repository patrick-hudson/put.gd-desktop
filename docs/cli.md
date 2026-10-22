# Resident JSON CLI

Start `putgd` once in your graphical user session. `putgd-cli METHOD [JSON_PARAMS]`
connects to `$XDG_RUNTIME_DIR/putgd/session.sock`. `--socket PATH` explicitly selects a
session endpoint; `--request-id ID` sets correlation. The runtime directory is owner-only
and the socket uses Qt's user-access restriction. A second resident never removes a
live socket. No TCP listener exists.

Launching `putgd`, `putgd --history` or `putgd --activity` again asks the resident
to open that window. `--activity` is the same as `--history`: both open History. The launcher exits 0 only after the matching window acknowledgement;
a refused, malformed, incomplete or oversized reply exits 1. The exchange has one
15-second deadline and a 16 KiB reply limit. A connected resident remains the session
owner even if opening its UI fails: the launcher does not start a second resident or
replay the request. Diagnostics contain only known error codes and fixed guidance,
never resident-provided error messages or details. `ui_incompatible` means the installed
UI differs from the running resident; let pending uploads finish, then restart put.gd.
A later launch retries normally. Duplicate `putgd --background` launches only probe
the resident and return immediately without opening a window.

Requests are one newline-terminated JSON object, at most 1 MiB, with exactly
`schemaVersion: 1`, `requestId`, `session` (the socket's exact path), `method` and
`params`. Responses contain `schemaVersion`, the same `requestId`, `ok`, `result` and
`error` (`code`, `message`, `details`). Normal command stdout is one JSON response;
logs belong to stderr. Exit 0 means accepted/successful transport result, not necessarily
completed upload. Inspect the returned operation ID for eventual outcomes.
Active `operation.inspect` and `operation.recent` results also expose an operation
`phase` and per-item `phase`/optional `uploadProgress` with `bytesTransferred` and
optional `bytesTotal`. A missing total means percentage is unknown. These are
transient observations, not confirmed publication or final result evidence.

The CLI waits 3 s to connect, 3 s to send and then a bounded time for the whole
reply, at most 16 MiB: 15 s for most methods. Screen-recording commands that the
resident answers only after the recording helper wait 15 s past the helper's own
deadline, so its `recording_helper_timeout` still arrives: 45 s for
`recording.stop` and 30 s for `recording.capabilities`, `prepare`, `select`,
`start`, `pause`, `resume`, `raise`, `overlay` and `preview`. Work that can take longer
replies when it starts: `recording.prepare` with `state: preparing`, and
`recording.saveCopy` and `recording.upload` with `state: exporting`; poll
`recording.inspect` (see [recording.md](recording.md#exports)). A missed wait is
`transport_failed`; the resident may still finish the request, so inspect before
retrying.

Examples:

```sh
putgd-cli capabilities
putgd-cli action.invoke '{"action":"clipboard"}'
putgd-cli operation.inspect '{"id":"RETURNED-ID"}'
putgd-cli history.list '{"limit":50,"offset":0,"category":"images"}'
putgd-cli settings.get
```

Methods and parameters:

| Method | Parameters |
|---|---|
| `action.invoke` | `action`: region, window, desktop, clipboard, edit; `edit` also accepts `source` and `path` as `editor.open` |
| `editor.open` | optional `source` region (default)/window/desktop/clipboard/file, `path` (absolute, file only) |
| `editor.inspect`, `.submit`, `.cancel`, `.raise` | operation `id` of the open editor |
| `editor.apply` | `id`, `commands` array (1–100); atomic |
| `editor.preview` | `id`, optional `maxEdge` 64–1600; base64 sanitized PNG |
| `editor.saveCopy` | `id`, absolute `.png` `path`, optional `overwrite` |
| `editor.fileDialog` | `id`, `action` (`open`, `inspect`, `cancel`); the editor's **Save a copy…** chooser and its state ([file-dialogs.md](file-dialogs.md)) |
| `operation.recent` | empty object; configured recent-activity bound |
| `operation.inspect`, `.cancel`, `.retry`, `.copy`, `.retryLocalSave` | `id`; `.cancel` also takes an optional `itemId` (an item's `artifact.id`) to cancel one file of an active batch while the others continue. That item ends `cancelled` and the next Retry uploads it |
| `history.list` | optional limit, offset, search, category, mime, profileId, uploadStatus, localStatus, remoteStatus, from, to |
| `history.detail`, `history.copyLink`, `history.copyImage` | history `id` |
| `history.openLink` | history `id`; opens the stored link in the default browser as the link-ready notification does, and returns `route` (`portal` or `launcher`), `opened` and `fallbackFrom`. `link_unavailable` for a removed or expired link, `link_not_opened` when nothing could open it ([diagnostics.md](diagnostics.md#opening-links-and-folders-on-the-host)) |
| `history.preview` | history `id`; makes a missing image preview or video poster from the retained artifact or an unchanged local copy and returns `state` (`ready` or `made`), `previewPath` and `source`, or why there is none; waits up to 45 s ([recording.md](recording.md#previews-made-on-selection)) |
| `history.clear`, `cache.clear` | empty object; `cache.clear` returns `removedOperations`, `abortedUploads` and `incompleteUploads` (at most 100 `{operationId, itemId, error, reported}` entries whose multipart abort S3 refused; `reported` is true once the user was told about that upload), and `incompleteUploadNotice` when the pass kept uploads nobody was told about yet (`count`, `bytes`, `buckets`, `labels`, `providers`, `error`, `hint`, `message`); it also returns `trigger: manual`, `elapsedMs` and `finished`. The same cleanup runs on its own hourly (#202): `diagnostics.result.artifactCleanup` reports `running`, `automatic`, `intervalMs`, the `last` outcome (`trigger: automatic`, plus `more` when a pass stopped at its batch of 50 and `stopped` when a Retry or shutdown cut it short) and `incompleteUploadNotice`, the latest notice since the resident started (#226) |
| `logs.clear` | empty object; deletes every persistent log file and starts a new one ([diagnostics.md](diagnostics.md#retention)) |
| `update.status` | empty object; the daily update check: `build` (version, kind, channel, packageTarget, supported, reason), `check`, `channelSetting`, `channel`, `enabled`, `notifies`, `checking`, `attemptedAt`, `checkedAt`, `nextCheckAt`, `result` and the last `error` ([updates.md](updates.md)) |
| `update.check` | empty object; checks now, even with daily checks off, and returns `update.status`'s object, or `update_feed_unavailable`, `update_feed_invalid`, `update_check_failed` or `update_check_unavailable`. Waits up to 55 s ([updates.md](updates.md)) |
| `diagnostics.bundle` | optional `operation`, `since` (`1m`–`365d`, default `7d`), `includeNames`, absolute `output`, `preview`, `notify`, `upload` with optional `profileId` and `copyLink`; or the CLI flags `--operation`, `--since`, `--include-names`, `--output`, `--upload`, `--profile ID`, `--copy-link`. Saves a redacted diagnostics package locally; `preview` only reports what it would hold (it takes no upload options); with `upload` it then sends the package to a working destination and returns `link`, or `uploadError` saying why it was skipped or failed ([diagnostics.md](diagnostics.md#uploading-a-package)). Waits up to 10 minutes |
| `diagnostics.uploadTarget` | optional `profileId`; where a package would upload and whether that destination works now: `available`, `profileId`, `label`, `provider`, `publicLink`, `warning`, `shareExpirationDays`, or `reason` and `message`. Runs Test connection ([diagnostics.md](diagnostics.md#uploading-a-package)) |
| `diagnostics.reveal` | absolute `path` of a saved `putgd-diagnostics-*.zip`, or of any package `diagnostics.bundle` saved in this resident under another name (#223); Show in folder ([diagnostics.md](diagnostics.md#settings-and-notifications)) |
| `delete.prepare` | `ids` array of confirmed history IDs |
| `delete.confirm` | identical ordered `ids`, exact returned `acknowledgement` |
| `delete.inspect`, `.retry`, `.cancel` | deletion-operation `id` |
| `settings.developmentTemplate` | empty object; inspection only |
| `settings.importDevelopment` | `expectedRevision`; explicit validated setup |
| `settings.get`, `.reload` | empty object |
| `settings.validate` | complete `document` |
| `settings.patch` | recursive `patch`, `expectedRevision`; null is a value |
| `settings.reorderProfiles` | `order` (every profile ID exactly once), `expectedRevision`; display order only |
| `capabilities`, `diagnostics` | empty object |
| `ui.quit` | empty object; explicitly exit the resident application |
| `ui.moveDestination` | `direction` up/down, optional `profileId`; drives the open Settings window |
| `ui.diagnostics` | `action` (`inspect`, `setDebugLogging`, `clearLogs`, `openPackage`, `setPackage`, `savePackage`, `showInFolder`, `closePackage`), with `enabled`, `includeNames`, absolute `folder`, `upload` or `operation` where they apply; drives Settings → Advanced → Diagnostics |
| `ui.historySaveDiagnostics` | operation or history `id`; clicks **Save diagnostics for this upload** on that failed History row |
| `ui.fileDialog` | `dialog` (`editImage`, `historySaveAs`, `outputFolder`, `diagnosticsFolder`), `action` (`open`, `inspect`, `cancel`), optional `field` (`root`, `images`, `videos`, `text`, `other`; `outputFolder` only) or history/operation `id` (`historySaveAs` only); opens a chooser in the open window as its button does and answers with its state ([file-dialogs.md](file-dialogs.md)) |

Editor methods, command shapes and error codes are specified in
[image-editor.md](image-editor.md). Nothing is uploaded until `editor.submit` or the
window's Upload; `editor.cancel` and `operation.cancel` store nothing.

`ui.quit` acknowledges with `quitting: true` before scheduling shutdown. It exits
the resident, closes its UI helper, stops shortcuts and cancels pending work through
the normal shutdown path. Closing a Settings, History or Activity window does not
quit the resident. On desktops without a tray, Settings exposes a **Quit put.gd** button.

History date boundaries use stored ISO UTC timestamps. Search is a literal substring of
the display filename; SQL wildcard characters are escaped. Pages are 1–200 entries.
Delete preparation performs no destructive request. Review its original destinations
and effects before confirming. Wrong acknowledgement, unresolved publication, local-only
or skipped entries never issue a deletion. Clearing history/cache never removes cloud
objects. Copy retry never reuploads. No source or credential value is accepted in the
CLI's JSON configuration. Native sources and S3 become available only when their real
adapters are implemented and registered; current unsupported capabilities are explicit.

For a configuration walkthrough, inspect `capabilities`, `settings.get`, and
`diagnostics` before invoking an action. `settings.get` returns the accepted JSON at
`result.document` and file state at `result.metadata`: `path`, `acceptedRevision`,
`observedRevision`, `validation`, and watcher status. `result.effectiveLimits` holds
the resolved `maxFileBytes`, `maxBatchBytes` and `pendingArtifactBytes` (`null` for no
limit) and, under `disk`, the upload store's `freeBytes`, `totalBytes`, system
`reserveBytes`, `reservedBytes` held by imports in flight, and `availableBytes`. Use
`observedRevision` as the patch's `expectedRevision` and reread after a successful
patch or conflict:

```sh
putgd-cli settings.patch '{"patch":{"retention":{"recentActivityLimit":77}},"expectedRevision":"RETURNED-OBSERVED-REVISION"}'
putgd-cli settings.get
```

The three artifact byte limits take a byte count, `"auto"` or `null` (No limit); free
disk space and the destination's maximum always apply. See [configuration](configuration.md):

```sh
putgd-cli settings.patch '{"patch":{"limits":{"maxFileBytes":null,"maxBatchBytes":null,"pendingArtifactBytes":"auto"}},"expectedRevision":"RETURNED-OBSERVED-REVISION"}'
```

`settings.reorderProfiles` changes only the order of the `profiles` array, which is
the order of the Destinations list and every Destination dropdown. Routing,
per-action exceptions, the Default badge and history refer to profiles by ID and
do not change. The `order` must name every current profile ID exactly once:
a missing, extra or duplicate ID returns `profile_order_invalid` with
`details.missing`, `details.extra` and `details.duplicate`, and a stale
`expectedRevision` returns `config_conflict`. Neither writes anything. An order that
matches the current one returns `changed: false` without rewriting the file.

```sh
putgd-cli settings.reorderProfiles '{"order":["proton","s3-1"],"expectedRevision":"RETURNED-OBSERVED-REVISION"}'
```

`ui.moveDestination` is the UI path for agents and tests: it acts on an open Settings
window like **Move up** / **Move down** in the destination's ⋯ menu. With `profileId`
it first selects that destination. It moves the selection one place, keeps it
selected and saves through the window's revision-checked autosave. The reply gives
`order`, `selected` and `saving`; confirm the save with `settings.get`. Without an open
Settings window it returns `ui_view_closed`; at the top or bottom of the list it
returns `destination_move_invalid`.

`ui.diagnostics` drives Settings → Advanced → Diagnostics in the open window
([diagnostics.md](diagnostics.md#settings-and-notifications)). Every reply is the
group's state: `debugLogging` (`enabled`, `expiresAt`), `logs` (`files`, `bytes`,
`oldestRecord`), `loggingDegraded`, `uncleanExit`, the displayed texts, and `package`
(the dialog: `open`, `includeNames`, `folder`, `preview`, `uploadTarget`, `upload`,
`saving`, `saved`, `error`), plus
`result` for `clearLogs` and `showInFolder`. `setDebugLogging` takes `enabled` and
saves like the checkbox. `clearLogs` deletes the logs without the button's
confirmation. `openPackage` opens **Create diagnostics package…** (optional
`operation`, `includeNames`) and answers with its preview; `setPackage` changes
`includeNames`, the `folder` (an existing folder, which Settings remembers) or
`upload`. `package.uploadTarget` is the dialog's `diagnostics.uploadTarget` answer
with `checking` (true while the check runs); `upload: true` works only once it is
`available` (otherwise `diagnostics_upload_checking` or
`diagnostics_upload_unavailable` with its `reason`), and `package.upload` says
whether **Also upload and copy a link** is ticked (it starts ticked when available).
`savePackage` saves and answers with `package.saved` (`path`, `bytes`, `sha256` …,
plus `link`, `linkCopied` or `uploadError` after an upload). With the upload ticked
it answers as soon as saving starts; `inspect` shows `package.saving` until
`package.saved` arrives;
`showInFolder` reveals it; `closePackage` closes the dialog. Without an open
dialog the package actions return `diagnostics_dialog_closed`.

`ui.historySaveDiagnostics` selects the History row of that operation (or of a file's
history ID) on the page shown and clicks **Save diagnostics for this upload**,
answering with the saved package. It returns `history_row_not_found` when the row
isn't on that page and `diagnostics_row_not_failed` for a row that didn't fail. Both
methods need the window open (`ui_view_closed`) and finish within the window's
10-second command deadline; a slower save answers `ui_command_timeout`, after which
`ui.diagnostics` `inspect` shows the outcome.

A successful `settings.get` envelope can still report
`result.metadata.validation.ok: false` for invalid on-disk bytes; `result.document`
remains the last accepted document. `settings.reload` returns `config_invalid` with
the failing JSON path in `error.details.path`. Correct that field in the complete
file using the atomic-save/locking convention in [configuration.md](configuration.md),
reload, then verify validation and both revisions. `diagnostics` reports backend,
Qt version, journal readiness, active operation count, capabilities, and the
configuration and data folders in use with any earlier layout left behind
(`locations`, see [configuration.md](configuration.md#locations)); inspect
configuration separately to diagnose a settings error.

`diagnostics.result.build` is the compiled build identity, and
`diagnostics.result.release` is the GitHub release the main window's rail links its
version to: `version` (the full `build.version`), `kind` and `url`. Stable, candidate
and nightly versions (`stable`, `candidate`, `nightly`) give
`https://github.com/patrick-hudson/put.gd-desktop/releases/tag/vVERSION`, the tag
`tools/releases` cuts; the build system gives those versions only to clean builds of
that exact tag. Every other build, including local and CI development builds, gives
`none` and an empty `url`, and the rail shows its version as plain text. The mapping
reads only the compiled identity and never the network, so a candidate's link opens
only after its draft release is published.

`diagnostics.result.logging` reports the persistent log without reading it: its
`directory`, `files`, `bytes`, `oldestRecord`, `degraded` and `degradedReason`,
`droppedRecords`, `retention`, `rotationBytes`, `debugLogging` and
`debugLoggingExpiresAt`; `loggingDegraded` repeats the flag.
`diagnostics.result.uncleanExit` reports whether this start found the previous
resident's running marker, with the last and recent records. See
[diagnostics.md](diagnostics.md#diagnostics-fields).

`diagnostics.result.memory` contains `allocatorReclamationSupported`,
`allocatorReclamationPending`, `allocatorReclamationCount` and
`lastAllocatorReclamationUsec`. On glibc Linux the count records one-shot attempts
to return free heap pages after successful startup or payload work and background workers drain, followed
by two seconds of quiet. An attempt need not reclaim any pages. These fields carry
no payloads or addresses; measure RSS separately using [testing-memory.md](testing-memory.md).
Other allocators report unsupported and keep the existing cleanup behavior.

`setup.storeCredentials` is rejected by the CLI before connecting to the resident (`credentials_require_settings`). Enter access keys in Settings; use credential references in agent configuration. The authenticated local Settings transport retains its credential-entry endpoint.

Proton link management uses `history.share`, `history.revokeLink` and
`history.inspectLink` with an exact history `id`. They return `shareOperationId`;
read `share.inspect` with that ID until its durable outcome is known. A confirmed
link expiry appears as `expiresAt` there and as `shareExpiresAt` in
`history.detail`, also after it has passed. Revoke
removes URL access only. See [Proton Drive](proton-drive.md). Account setup and
password entry belong in Settings; the CLI rejects `setup.storeSharePassword`
so passwords are never passed as process arguments.

For Proton, explicit `operation.retry` can adopt a longer current request
deadline when the account and folder still match. Inspection exposes that
separate attempt budget as `requestTimeoutMs`; the original destination
snapshot remains unchanged. Retry retains the original publication ID and
reconciles before another upload.

`history.copyImage` copies the selected retained original image through the resident clipboard adapter, so it remains available after closing History. It accepts only a history ID; input is limited to 32 MiB and 16 million pixels, with a bounded full-size PNG conversion. Changed or unavailable originals are rejected.
