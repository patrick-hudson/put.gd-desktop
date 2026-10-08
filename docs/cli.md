# Resident JSON CLI

Start `putgd` once in your graphical user session. `putgd-cli METHOD [JSON_PARAMS]`
connects to `$XDG_RUNTIME_DIR/putgd/session.sock`. `--socket PATH` explicitly selects a
session endpoint; `--request-id ID` sets correlation. The runtime directory is owner-only
and the socket uses Qt's user-access restriction. A second resident never removes a
live socket. No TCP listener exists.

Requests are one newline-terminated JSON object, at most 1 MiB, with exactly
`schemaVersion: 1`, `requestId`, `session` (the socket's exact path), `method` and
`params`. Responses contain `schemaVersion`, the same `requestId`, `ok`, `result` and
`error` (`code`, `message`, `details`). Normal command stdout is one JSON response;
logs belong to stderr. Exit 0 means accepted/successful transport result, not necessarily
completed upload. Inspect the returned operation ID for eventual outcomes.

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
