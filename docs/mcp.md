# Optional local MCP helper

The Python 3.12+ helper uses the official MCP SDK over stdio. Each tool validates its
arguments and calls the production `putgd-cli` as an argument vector. It owns no
capture/storage pipeline, credentials, model, TCP listener or shell tool. The resident
app must already run in the same user session. Start it following [build.md](build.md).
Capabilities state actual adapter availability; the helper cannot enable a missing backend.

From the repository root, install the locked helper with ordinary uv:

```sh
uv sync --project tools/mcp --locked
uv run --project tools/mcp --locked putgd-mcp --cli "$PWD/build/dev/putgd-cli"
```

Configure an MCP client to launch the helper with stdio, for example:

```json
{
  "mcpServers": {
    "putgd": {
      "command": "/absolute/repository/tools/mcp/.venv/bin/putgd-mcp",
      "args": ["--cli", "/absolute/repository/build/dev/putgd-cli"]
    }
  }
}
```

`--socket /absolute/path/session.sock` selects an explicit same-user session endpoint;
otherwise the CLI uses `$XDG_RUNTIME_DIR/putgd/session.sock`. Paths containing spaces
are single argument-vector elements. `--timeout` sets the CLI process deadline in
seconds (default 60). The CLI bounds its own wait for each method (up to 51 s for
`recording.stop` or `action.invoke`; see [cli.md](cli.md)), so keep `--timeout` above that or slow
recording commands report `cli_timeout`. A deadline terminates the client process, not a resident operation:
a sent action may already have been accepted. Inspect activity through the CLI before
invoking another action; do not infer rollback from a transport error. The helper never
starts the resident or retries a mutation automatically.

Some stdio clients launch servers with a minimal environment that omits
`XDG_RUNTIME_DIR`. In that case, pass the actual resident socket explicitly with
`--socket`; do not guess another user's endpoint or start a second resident.
The fresh Cinnamon walkthrough used this explicit endpoint to connect the official
SDK client to the already-running graphical resident. The omission was a client
launch-environment issue, not a failed native acquisition.

## Tools and effects

All tools return version-1 CLI envelopes with `ok`, `result`, `error` and a request ID.
Successful transport acceptance need not mean a completed operation. Preserve returned
operation IDs and inspect eventual item outcomes. Adapter errors are structured as
`invalid_request`, `cli_unavailable`, `cli_timeout`, or `invalid_response`; resident errors
such as `session_unavailable` and `config_conflict` retain their original envelope.

| Tool | Inputs / forwarded service | Effect |
| --- | --- | --- |
| `inspect` | capabilities, diagnostics, settings.get | Local metadata only; accepted non-secret config/path/revisions/status and redacted backend diagnostics. On X11, `capabilities.result.source.shortcuts.keyPaths` names the listening key paths and the last one that fired (`passive`, or `raw` when another client held the keyboard; [#341](https://github.com/patrick-hudson/put.gd-desktop/issues/341)) |
| `get_view_state` | ui.viewState | Settings → Capture is available as last checkpointed when Settings has been constructed: default output and each action's destination, sharing, resolved mode, inheritance and enabled controls. Appearance (theme setting, desktop preference and source, scheme, token values) is returned independently, including before Settings is constructed. |
| `perform_action` | action region/window/desktop/clipboard/edit/record → action.invoke | Acquires source; configured local save or public upload; may write clipboard result. `record` prepares a dragged-region recording (or raises the active one); drive it with the recording tools |
| `get_operation` | id; kind operation/delete/share | operation.inspect, delete.inspect or share.inspect (a `shareOperationId`: sharing status and confirmed link `expiresAt`); local state |
| `retry_operation` | id; stage upload/local_save/delete | operation.retry, operation.retryLocalSave, delete.retry; explicit stage effects |
| `copy_result` | id → operation.copy | Copies retained result/receipt; no upload |
| `cancel_operation` | id; kind operation/delete; optional item_id | Requests cancellation; sent remote mutations cannot be rolled back. With item_id (an operation item's `artifact.id`), only that file stops and the batch continues |
| `list_history` | optional pagination/filters → history.list | Offline local metadata. `category: "text+other"` selects History's **Other files** aggregate (stored `text` and `other`); every other category keeps exact matching ([CLI semantics](cli.md)) |
| `get_history_entry` | id → history.detail | Original receipt/local-copy/deletion state |
| `make_history_preview` | id → history.preview | Makes a missing image preview or video poster (never made, failed or cleaned up) from the retained file or an unchanged local copy; returns `previewPath` and `state` `ready`/`made`, or why there is none ([recording.md](recording.md#previews-made-on-selection)) |
| `copy_history_link` | id → history.copyLink | Writes stored direct URL to clipboard |
| `open_history_link` | id → history.openLink | Opens the stored direct URL in the default browser exactly as clicking the link-ready notification does: the XDG portal's OpenURI, else `gio open` with the host's environment ([diagnostics.md](diagnostics.md#opening-links-and-folders-on-the-host)). Returns `route` (`portal`/`launcher`), `opened` and `fallbackFrom`; `link_not_opened` when nothing opened it |
| `delete_history_uploads` | nonempty ids; optional acknowledgement | Omitted acknowledgement prepares effects; exact returned acknowledgement confirms remote deletion |
| `reload_configuration` | settings.reload | Validates/reloads existing file; no source/provider activity |
| `patch_configuration` | patch, expected_revision → settings.patch | Validates full candidate and atomically saves via shared SettingsService. `limits.maxFileBytes`, `maxBatchBytes` and `pendingArtifactBytes` take a byte count, `"auto"` or `null` (No limit); `inspect` returns the resolved values and the disk figures under `configuration.result.effectiveLimits` |
| `reorder_destinations` | order, expected_revision → settings.reorderProfiles | Sets the destination display order; every profile ID exactly once. A missing/extra/duplicate ID or stale revision changes nothing; routing and history keep their IDs |
| `move_destination` | direction up/down, optional profile_id → ui.moveDestination | Moves the selected (or given) destination one place in the open Settings window and saves it, as **Move up** / **Move down** do |
| `create_diagnostics_bundle` | optional operation_id, since, include_names, output, preview, notify, upload, profile_id, copy_link → diagnostics.bundle | Saves a redacted diagnostics package (.zip, at most 20 MiB) locally and returns its path. `preview` reports the contents without writing; `notify` shows the saved notification with Show in folder; with upload, also sends it to a working destination and returns its link or `uploadError` ([diagnostics.md](diagnostics.md#uploading-a-package)); waits up to 630 s then |
| `get_diagnostics_upload_target` | optional profile_id → diagnostics.uploadTarget | Whether a package can upload now, where, and the sharing warning; runs Test connection ([diagnostics.md](diagnostics.md#uploading-a-package)) |
| `show_diagnostics_package` | absolute path → diagnostics.reveal | Opens the file manager with a saved package selected; without one, the portal's OpenDirectory or `gio open` on its folder; any path `create_diagnostics_bundle` saved in this resident works, whatever its name ([diagnostics.md](diagnostics.md#settings-and-notifications)) |
| `settings_diagnostics` | action, optional enabled, include_names, folder, operation_id, upload → ui.diagnostics | Drives Settings → Advanced → Diagnostics in the open window: inspect, setDebugLogging, clearLogs, openPackage, setPackage (upload ticks "Also upload and copy a link" once `package.uploadTarget` is available), savePackage, showInFolder, closePackage ([cli.md](cli.md)) |
| `get_update_status` | → update.status | The daily update check: this build's channel and package, the setting, the channel followed (`channelSource`: a DEB/RPM follows its repository, with the commands that move it in `action.otherChannel`), the last result, the next check and the last error ([updates.md](updates.md)) |
| `check_for_updates` | → update.check | Checks now, as **Check now** does; nothing is downloaded or installed ([updates.md](updates.md)) |
| `install_update` | → update.install | AppImage only, with `confirm: true`: **Download and install** (zsync through the host's appimageupdatetool, else a verified full download), checked against the release's signed `SHA256SUMS`, put in place of the running image; progress and result in `get_update_status`'s `install` ([updates.md](updates.md#download-and-install-from-putgd)). While the other channel is watched (`install.kind: switch`) it is **Switch to CHANNEL VERSION**: `channel` must name `install.switch.to`, and `allow_older: true` is needed when `install.switch.older` ([switching](updates.md#switching-channels)) |
| `restart_after_update` | → update.restart | With `confirm: true`, once an installed update waits: quits put.gd and starts the new version |
| `open_software_updates` | → update.openSoftwareUpdates | Opens Update Manager, Discover or GNOME Software for a DEB/RPM install that updates from put.gd's repository; put.gd itself installs nothing ([package-repositories.md](package-repositories.md)) |
| `history_save_diagnostics` | id → ui.historySaveDiagnostics | Clicks **Save diagnostics for this upload** on that failed row of the open History window and returns the package |
| `drive_file_dialog` | dialog editImage/historySaveAs/outputFolder/diagnosticsFolder, action open/inspect/cancel, optional field, id → ui.fileDialog | Opens a file chooser in the open window as its button does (the desktop's own through the XDG portal, or the themed fallback), reads its state or cancels it; the state says which backend showed it ([file-dialogs.md](file-dialogs.md)) |
| `clear_logs` | logs.clear | Deletes every persistent log file and starts a new one; uploads, history, settings and saved diagnostics packages stay. `inspect` reports the log under `diagnostics.logging` ([diagnostics.md](diagnostics.md)) |
| `open_editor` | optional source region/window/desktop/clipboard/file, path → editor.open | Opens the visible editor (Capture region & edit); uploads nothing yet. History's **Edit again** is `source: file` with the item's `local.path` from `history.list` or `history.detail` |
| `get_editor` | id → editor.inspect | Image size, crop, shapes, undo/redo, phase |
| `edit_image` | id, commands → editor.apply | Atomic redact/blur/arrow/rectangle/highlight/text/crop/undo/redo, shown live |
| `preview_editor` | id, optional max_edge → editor.preview | Returns the sanitized result as an MCP image plus metadata |
| `save_editor_copy` | id, path, optional overwrite → editor.saveCopy | Writes the sanitized PNG to a file; no upload |
| `editor_file_dialog` | id, action open/inspect/cancel → editor.fileDialog | The editor's **Save a copy…** chooser as the button opens it, its state, or a cancel; a file picked there gets the sanitized PNG |
| `submit_editor` | id → editor.submit | Sends only the sanitized result through the configured output |
| `cancel_editor` / `raise_editor` | id → editor.cancel / editor.raise | Discard without upload / bring the window forward |
| `get_recording_capabilities` | recording.capabilities | Recording support, formats/codecs, audio, limits, active ID; no capture or prompt |
| `prepare_recording` | target, format, codec?, fps?, system_audio, max_edge? → recording.prepare | Durable intent, ready bounds/overlay and any OS consent; captures nothing yet. `disk_space_insufficient` when the disk can't hold a recording that lasts (#213) |
| `control_recording` | id, command start/pause/resume/stop/cancel/raise → recording.<command> | Acknowledged state changes; stop always opens private review, cancel uploads nothing |
| `get_recording` / `set_recording_overlay` | id → recording.inspect / id, visible → recording.overlay | State, `selecting`, edit revision, progress, `lastExport`, `stopReason` and `stopDetails` (including the triggering disk-space sample retained after a budget stop, not ongoing free-space telemetry), bar placement (`overlayPlacement`) / overlay visibility; a hidden bar stays hidden through state changes until `recording.raise`, which keeps it out of a full-screen clip |
| `select_recording_region` | id, rect → recording.select | Completes an open region selector (tray, shortcut, dashboard) with a rectangle, as a drag would |
| `edit_recording` | id, revision, commands → recording.edit | Atomic trim and timed blur/solid masks in microseconds and canvas pixels |
| `preview_recording` | id, time_us, max_edge → recording.preview | One frame with trim/masks applied as an MCP image plus metadata |
| `drive_recording_review` | id, action, time_us?, speed?, kind?, rect?, mask_id?, strength?, format?, codec? → recording.review | Works the open review window's controls as a user does (seek, play/pause, speed, trim in/out at the playhead, add/select/retime/remove masks, blur strength, output format, Save copy… with `saveCopyDialog` / `closeFileDialog`) and inspects it (playhead, selection, contextual mask controls, the video's share of the window); edits go through the resident like the window's own |
| `save_recording_copy` | id, path, overwrite → recording.saveCopy | Exports the reviewed result to a file; no upload. Returns `exporting` when the export starts; poll `get_recording` until `reviewing` and read `lastExport` |
| `upload_recording` | id, revision → recording.upload | The only publishing action; sends the validated export through the configured output. Returns `exporting` and the `operationId` when the export starts; a failed export returns the session to `reviewing` with `lastExport.error`, otherwise `get_operation` follows the upload |

Upload retry delegates to the shared operation service: completed uploads are not repeated,
unknown publication reconciles its original identity, and terminal uploads can instead copy
their result. `local_save` retries archives independently; `delete` uses a separately persisted
deletion-operation ID. Get/cancel default to `kind="operation"`; retry defaults to
`stage="upload"`. No retry reconstructs a target from a URL or current destination.

History filters are `limit` (1–200), `offset` (nonnegative), `search`, `category`, `mime`,
`profile_id`, `upload_status`, `local_status`, `remote_status`, `from_date`, `to_date`.
Configuration patches include the optional `captureDefaults` and inherited action routes;
the resident validates their complete result.
The snake-case names map to the CLI's camel-case fields; date bounds are stored ISO UTC
strings. Search matches a literal display-name substring. Explicit history requests may
return names/URLs; default diagnostics do not. Browsing fetches no remote bytes.

For deletion, call `delete_history_uploads({"ids":["synthetic-entry-id"]})` to prepare.
Review original destinations and provider effects; with authorization for those exact
objects/effects, call again with identical ordered IDs and the returned acknowledgement.
An empty selection or acknowledgement is rejected before invocation. A mismatch is
rejected by the resident before mutation. Never supply arbitrary URLs, object keys,
prefixes or bucket-wide targets. Inspect per-item removed/missing/changed/failed/unknown/
retained-version outcomes using `kind="delete"`. Local files/history survive deletion;
origin deletion cannot invalidate browser/CDN caches. See [S3 behavior](s3.md).

For configuration, inspect before patching, then use the reported file-content revision
`metadata.observedRevision` as `expected_revision` (see [configuration.md](configuration.md) for metadata).
The `inspect` tool nests the three CLI envelopes under `result.capabilities`,
`result.diagnostics`, and `result.configuration`. Read the accepted document from
`result.configuration.result.document` and the revision from
`result.configuration.result.metadata.observedRevision`; check each nested `ok`
and the configuration metadata's `validation.ok`. The outer successful inspection
envelope alone does not establish that the on-disk configuration validates.
A synthetic change is `{"patch":{"preferences":{"notifications":false}},
"expected_revision":"RETURNED-FILE-REVISION"}`. A stale revision returns `config_conflict`:
reread and rebase; never silently overwrite. Object patches recurse; arrays/scalars replace,
and null is an explicit value. The helper only accepts documented non-secret sections;
credentials are references, never key values. The complete schema/development example and
pure provider validators remain authoritative. Invalid external edits preserve accepted
settings. Atomic-save/locking conventions, dirty-GUI conflicts and operation snapshots
are documented in configuration.md. Reload and patch do not acquire clipboard content,
prompt for credentials or issue provider requests.

## Headless contract checks and remaining acceptance

After the app build and locked helper setup, run:

```sh
PUTGD_CLI="$PWD/build/dev/putgd-cli" uv run --project tools/mcp --locked \
  python -m unittest discover -s tests/integration -p test_mcp.py -v
```

This separate suite uses an official SDK stdio client, the production CLI, synthetic
Unix-session responses and temporary malformed/timeout peers. It checks discovery,
request IDs, methods/parameters, structured errors, explicit deletion acknowledgement,
spaces in paths, metadata-only inspection, expected-revision conflict forwarding, and
a slow `recording.stop` (answered after 16 s) outlasting the CLI's ordinary wait.
It does not prove real compositor, GUI, provider network effects, public links or native
clipboard behavior. The C++ service suites separately verify resident business behavior.

The native CLI, graphical lab runner and development-package commands are implemented.
Use [vm-lab.md](vm-lab.md) for disposable overlays and native scenarios,
[release.md](release.md) for existing CMake/CPack commands and pending installed
lifecycle checks, and [agents.md](agents.md) for the source and verification map.
An earlier [fresh-agent Cinnamon walkthrough](evidence/2026-10-08-fresh-cinnamon.json)
exercised build/package/run/collect instructions, actual MCP/CLI local-operation
parity and configuration-error recovery. Its local-only scope and source-identity
limitations remain recorded; it did not exercise cloud publication or deletion.

Full U9 still requires complete production-resident tool/effect coverage and a
current-source reproducible walkthrough, including remote receipt/history/deletion
behavior. The source55 Cinnamon live S3 check used the production CLI and GUI,
so it does not establish MCP cloud parity. Current native and memory checkpoints
also leave the remaining U6 display/failure cases, full compositor release gates
and U10 release lifecycle open. [verification.md](verification.md) records the
source-specific checks and outstanding acceptance.
