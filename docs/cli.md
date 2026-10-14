# Resident JSON CLI

Start `putgd` once in your graphical user session. `putgd-cli METHOD [JSON_PARAMS]`
connects to `$XDG_RUNTIME_DIR/putgd/session.sock`. `--socket PATH` explicitly selects a
session endpoint; `--request-id ID` sets correlation. The runtime directory is owner-only
and the socket uses Qt's user-access restriction. A second resident never removes a
live socket. No TCP listener exists.

Launching `putgd`, `putgd --history` or `putgd --activity` again asks the resident
to open that window. The launcher exits 0 only after the matching window acknowledgement;
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
| `action.invoke` | `action`: region, window, desktop, clipboard |
| `operation.recent` | empty object; configured recent-activity bound |
| `operation.inspect`, `.cancel`, `.retry`, `.copy`, `.retryLocalSave` | `id` |
| `history.list` | optional limit, offset, search, category, mime, profileId, uploadStatus, localStatus, remoteStatus, from, to |
| `history.detail`, `history.copyLink` | history `id` |
| `history.clear`, `cache.clear` | empty object |
| `delete.prepare` | `ids` array of confirmed history IDs |
| `delete.confirm` | identical ordered `ids`, exact returned `acknowledgement` |
| `delete.inspect`, `.retry`, `.cancel` | deletion-operation `id` |
| `settings.developmentTemplate` | empty object; inspection only |
| `settings.importDevelopment` | `expectedRevision`; explicit validated setup |
| `settings.get`, `.reload` | empty object |
| `settings.validate` | complete `document` |
| `settings.patch` | recursive `patch`, `expectedRevision`; null is a value |
| `capabilities`, `diagnostics` | empty object |
| `ui.quit` | empty object; explicitly exit the resident application |

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
`observedRevision`, `validation`, and watcher status. Use `observedRevision` as the
patch's `expectedRevision` and reread after a successful patch or conflict:

```sh
putgd-cli settings.patch '{"patch":{"retention":{"recentActivityLimit":77}},"expectedRevision":"RETURNED-OBSERVED-REVISION"}'
putgd-cli settings.get
```

A successful `settings.get` envelope can still report
`result.metadata.validation.ok: false` for invalid on-disk bytes; `result.document`
remains the last accepted document. `settings.reload` returns `config_invalid` with
the failing JSON path in `error.details.path`. Correct that field in the complete
file using the atomic-save/locking convention in [configuration.md](configuration.md),
reload, then verify validation and both revisions. `diagnostics` reports backend,
Qt version, journal readiness, active operation count, and capabilities; inspect
configuration separately to diagnose a settings error.

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
read `share.inspect` with that ID until its durable outcome is known. Revoke
removes URL access only. See [Proton Drive](proton-drive.md). Account setup and
password entry belong in Settings; the CLI rejects `setup.storeSharePassword`
so passwords are never passed as process arguments.
