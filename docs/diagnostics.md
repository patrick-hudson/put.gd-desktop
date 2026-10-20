# Logs and diagnostics

put.gd keeps a persistent log so a failed capture or upload can be worked out
afterwards (#74). This page covers where the log lives, what a record holds, what
is never written, how long files are kept, how unclean exits are reported, and the
diagnostics package a user sends to a maintainer. The `diagnostics` method
([cli.md](cli.md)) reports the log's current state.

## Location and permissions

The resident writes JSON Lines files to `$XDG_STATE_HOME/putgd/logs/`, which is
`~/.local/state/putgd/logs/` when `XDG_STATE_HOME` is unset or not absolute. Qt's
`QStandardPaths::StateLocation` needs Qt 6.7 and the minimum is 6.4.2, so put.gd
resolves the path itself. The `putgd` state folder and `logs/` are created owner-only
(0700) and must be directories owned by the user; each file is created 0600 with
`O_NOFOLLOW`. Windows builds are not supported yet; the planned location is
`%LOCALAPPDATA%\putgd\logs\`.

Files are named `putgd-<UTC start, yyyyMMddTHHmmsszzz>Z-<pid>-<sequence>.jsonl`, so
name order is time order. The resident starts a new file when it starts and when the
current file reaches the rotation size. Other files in the folder are never touched.

Only the resident opens log files. Helpers (`putgd-ui`, `putgd-capture`,
`putgd-kde-region`, `putgd-record`, `putgd-s3-worker`, and `putgd-ui --edit-image`)
keep their existing restrictions and write nothing to disk: the resident reads their
stderr and records it, bounded to 64 KiB per helper generation and 2 KiB per line,
then records how each one exited. The resident sets `QT_FORCE_STDERR_LOGGING=1` for
itself and its helpers, so their Qt warnings reach that pipe instead of the journal.

## Records

Each line is one JSON object:

| Field | Meaning |
| --- | --- |
| `v` | Record schema version, `1` |
| `time` | UTC timestamp with milliseconds |
| `level` | `debug`, `info`, `warning`, `error` or `fatal` |
| `category` | `putgd.app`, `putgd.operation`, `putgd.s3`, `putgd.proton`, `putgd.capture`, `putgd.ui`, `putgd.ipc`, `putgd.settings`, `putgd.history`, `putgd.recording`, or a Qt category such as `default` or `qt.qpa.xcb` |
| `process`, `pid` | The process the record describes: `putgd` for the resident, or the helper whose stderr it carries |
| `message` | Text, at most 4096 characters |
| `operationId`, `itemId` | Present when the record belongs to an operation or one of its items |
| `fields` | Structured values, such as an error's `code`, `message` and `details` |

Records at the default level:

- **Startup and shutdown:** `Resident started` with the build manifest (version and
  channel), OS, kernel, architecture, desktop, session type, Qt runtime and compile-time
  versions, platform plugin (`none` for a Core-only resident), resident kind, tray
  availability, storage locations and source capabilities; then `Desktop portal
  availability`; `Resident stopping` and `Resident stopped` with the exit code.
- **Operations:** `Operation accepted` (action, local or upload, provider, profile),
  `Retry accepted`, `Artifacts acquired` (item count, bytes, MIME types),
  `Upload names chosen` (each item's name and naming status), `Upload attempt` (attempt
  number, name, bytes, MIME type), `Upload finished`, `Upload name collision` or
  `Upload failed` (status, elapsed time, whether the outcome is uncertain and the
  error), `Uncertain upload reconciled`, `Name check failed`, `Sharing finished`, and
  `Operation finished` (state, duration since acceptance, bytes, per-status item counts
  and every item error).
- **Provider errors:** the error's `code`, `message` and `details`. S3 details carry
  `httpStatus`, `serviceCode`, `requestStage`, `sdkRetryable`, `curlCode`, `elapsedMs`,
  and the S3 request IDs `requestId` (`x-amz-request-id`) and `extendedRequestId`
  (`x-amz-id-2`). The same IDs are stored with the item's error in history.
- **Helpers:** `Helper started` (pid, generation), each stderr line, `Helper output
  truncated` when a generation passes its bound, and `Helper exited` (exit code),
  `Helper crashed` (the signal Qt reports) or `Helper stopped by put.gd`. The S3
  worker's normal start and exit are debug records, since each request has one.
- **Settings:** `Configuration rejected` with the stage (`load` or `save`), error
  code, JSON pointer and message. The rejected value is never written.
- **IPC:** `Session request rejected` with the method name and code for
  `invalid_request`, `invalid_json`, `request_too_large`, `unsupported_version`,
  `wrong_session` and `unknown_method`. Parameters are never written.
- **Unclean exits:** see below.

### AWS SDK records

The S3 worker installs an AWS SDK log system that writes each record to stderr as
`putgd-aws<TAB>E|W|I<TAB>tag<TAB>text`; the resident stores it with its level in
`putgd.s3`, with `fields.stream` `aws-sdk` and `fields.awsTag`. The level is Warn,
or Info while debug logging is on (`PUTGD_AWS_LOG_LEVEL` passed to the worker).
Debug and Trace log request headers, including authorization data, so no setting or
environment value selects them; anything other than `error` or `info` means Warn.

### Debug logging

`preferences.diagnostics.debugLogging` (see [configuration.md](configuration.md))
keeps debug records from put.gd's categories and `qDebug`. The resident records when
it was turned on in `$XDG_STATE_HOME/putgd/debug-logging.json` and turns it off
24 hours later, patching the setting back to `false`; a restart does not extend it.
`diagnostics.logging.debugLoggingExpiresAt` shows when it ends. `QT_LOGGING_RULES`
still overrides individual categories for developers.

## Redaction

Every record is redacted when it is written, whatever its source (structured records,
Qt messages, helper stderr, AWS SDK lines):

- `Authorization` and `Proxy-Authorization` values, to the end of the line.
- `X-Amz-Security-Token`, `X-Amz-Signature`, `X-Amz-Credential`, `Signature=` and
  `Credential=` values.
- Query strings and fragments of URLs (presigned URLs, and Proton share links, which
  carry the share password in the fragment). Scheme, host and path stay.
- Values after secret-like names: AWS secret and session-token names, `password`,
  `passphrase`, `token`, `secret`, `api_key`, `client_secret`, `cookie` and similar.
- `Bearer`/`Basic` credentials, PEM private keys, AWS access key IDs, 40-character
  mixed-case base64 values (the shape of an AWS secret key) and base64 runs of 120 or
  more characters.
- Structured fields named like secrets or content (`password`, `secretKey`, `token`,
  `credentials`, `clipboard`, `text`, `content`, `payload` and similar) lose their value.

No code path logs clipboard text, file or preview bytes, credentials or keychain data.
`tests/unit/test_logging.cpp` checks this with synthetic canaries the way
`test_cli.cpp` checks `synthetic-private` values: an upload of clipboard text that
fails like a denied S3 PUT, helper stderr carrying secrets, and each pattern above.
File names, local paths, buckets, object keys and public URLs are no more sensitive
than the history database, so local logs may contain them.

## A logging failure never blocks an operation

Callers only queue a record; one writer thread does all file IO. The queue holds at
most 4096 records. A full queue, an unusable folder or a failed write (a full disk,
for example) drops the record, counts it in `droppedRecords` and sets `degraded`, and a
write that fails part-way is truncated back so no half record is left. Operations,
helpers and session methods carry on. The next successful write clears `degraded`;
`droppedRecords` keeps counting.

## Retention

| Setting | Default | Limits |
| --- | --- | --- |
| `retention.logDays` | 14 | 1–365 days |
| `retention.logBytes` | 50 MiB (52428800) | 1 MiB–1 GiB |

The current file rotates at 5 MiB, or at a quarter of `logBytes` when that is
smaller (never under 64 KiB). Cleanup deletes files last written more than `logDays`
ago, then the oldest files until the folder fits `logBytes`; the file being written is
kept. It runs when the resident starts, after each rotation, when the settings change
and once a day.

**Clear logs** is the `logs.clear` session method (`putgd-cli logs.clear`, MCP
`clear_logs`). It deletes every log file, starts a new one with a `Logs cleared`
record and returns `removedFiles` and `removedBytes`. A Settings button is #122.

## Diagnostics fields

`diagnostics` (and MCP `inspect`) includes:

- `logging`: `available`, `directory`, `files`, `bytes`, `oldestRecord` (time of the
  oldest record kept), `degraded`, `degradedReason` (`unavailable`, `write_failed`,
  `disk_full` or `queue_full`), `droppedRecords`, `retention` (`days`, `bytes`),
  `rotationBytes`, `debugLogging` and `debugLoggingExpiresAt`.
- `loggingDegraded`: the same flag as `logging.degraded`.
- `uncleanExit`: see below.

The figures come from the writer's bookkeeping; `diagnostics` never reads the files.

## Unclean exits

At startup the resident writes `$XDG_STATE_HOME/putgd/running.json` (pid, start time,
version, boot ID) and removes it on a clean shutdown. SIGTERM, SIGHUP and SIGINT
write their number to `running.signal` beside it and then take their default action,
so behaviour on a logout is unchanged. If the next start finds the marker, and its
pid is not a running `putgd`, it records:

- `cause`: `signal` (with `signal`, logged at info, as a logout normally is) or
  `unknown` (a crash, SIGKILL or power loss, logged as a warning);
- `previousPid`, `previousStart`, `previousVersion`, `sameBoot` and `detectedAt`.

The last 10 records are kept in `unclean-exits.json`. `diagnostics.uncleanExit` holds
`detected` (this start found one), `last` and `recent` (newest first). Crash
backtraces are #74 §5, tracked in #124.

## Diagnostics package

One `.zip` a user can send to a maintainer, who can then work out what happened.
It is created through the session method `diagnostics.bundle` (CLI and MCP; the
Settings button and failure-notification triggers are #122). It is always saved
locally first; it can then be uploaded to a working destination (see "Uploading a
package" below).

```sh
putgd-cli diagnostics.bundle [--operation ID] [--since 7d] [--include-names] [--output PATH] [--upload [--profile ID] [--copy-link]]
```

The flags map to the JSON parameters `operation`, `since`, `includeNames`,
`output`, `upload`, `profileId` and `copyLink`; MCP `create_diagnostics_bundle`
takes `operation_id`, `since`, `include_names`, `output`, `upload`, `profile_id`
and `copy_link`.

- **`operation`:** package that one operation, with the log records from ten minutes
  before it was accepted to ten minutes after its last record. `since` is ignored.
- **`since`:** without `operation`, the failed, partial and interrupted operations
  created in this window (newest first, at most 50) and its log records. `1m` to
  `365d` with `m`, `h` or `d`; the default is `7d`.
- **`includeNames`:** keep file names, object keys, URLs, buckets, endpoints and
  labels. Without it they are hashed.
- **`output`:** an absolute `.zip` path, which must not exist yet (a package never
  replaces a file), or an existing folder. The CLI makes a relative path absolute.
  The default is `<archive root>/diagnostics/`, `~/Documents/put.gd/diagnostics/`
  unless the archive root was changed.

Packages are named `putgd-diagnostics-<UTC yyyyMMdd-HHmmss>Z.zip` and written 0600.
The reply gives `path`, `bytes`, `sha256`, `redaction` (`hashed` or `names`), `files`,
`operations`, `logRecords`, `droppedLogRecords`, `defaultFolder` and `pruned`. One
package is built at a time (`diagnostics_busy`); other errors are
`operation_not_found`, `diagnostics_output_exists`, `diagnostics_output_unavailable`
and `invalid_request`.

### Contents

| File | Contents |
| --- | --- |
| `manifest.json` | Package `schemaVersion` 1, creation time, build manifest, `redaction`, `scope` (operation or window), every other file's path, size and SHA-256, log record counts (`records`, `eligibleRecords`, `droppedRecords`) and the size limit |
| `environment.json` | `/etc/os-release` `ID`, `ID_LIKE`, `VERSION_ID`, `VERSION_CODENAME`, `VARIANT_ID`; kernel; architecture; desktop and session type; Qt compile-time and runtime versions; platform plugin and resident kind; tray and portal availability; locale; screen count with each screen's device pixel ratio and logical DPI (no pixels); total RAM; free bytes under the data folder |
| `diagnostics.json` | The current `diagnostics` output, including each helper's `lastExit`, `logging` and `uncleanExit` |
| `config.redacted.json` | The accepted settings. Credential references are reduced to their `mode`, a Proton share-password reference likewise. Buckets, endpoints, public URL bases, labels, Proton folder names and IDs are hashed; region, storage class, ACL mode, timeouts and other options stay. Archive folders keep their shape below `<home>` |
| `operations.json` | Each operation's ID, action, state, phase, creation time, cancellation, copy status, provider, profile and error, and per item its ID, MIME type, category, size, name, upload name, upload/share/remote status, phase, local-copy status, error (with HTTP status, provider code and S3 request IDs), object key, direct URL and the number of upload attempts in the log window |
| `logs/putgd.jsonl` | The log records in the window, oldest first, with the package redaction applied |

The package is at most 20 MiB. The other files go first; the log records fill the
rest, newest first, so when it would go over, the oldest records are dropped and
counted in `droppedRecords`.

**Never included:** credentials, secrets or tokens, keychain data, signed URLs,
artifact or preview bytes, screenshots, clipboard content and the raw
`history.sqlite`.

### Redaction in packages

On top of the log's own redaction, which applies again:

- Known folders become placeholders in both modes: `<config>`, `<data>`, `<state>`,
  `<runtime>`, `<archive>` and `<home>`.
- Without `includeNames`, each file name, upload name, object key, URL, bucket,
  endpoint, label and Proton ID becomes `<kind:hash>` (names, keys and paths keep
  their extension, as in `<name:3f2a9c1b7d0e.png>`), and so does the rest of any path
  below `<home>` or `<archive>`. The hash is an HMAC with a key made for that package
  and never stored, so one value has the same token throughout a package, which lets
  a maintainer follow a file across records, but tokens can't be matched across
  packages or guessed from likely names. put.gd's own GitHub release links stay.

`tests/unit/test_diagnostics_bundle.cpp` checks the contents and hashes, the size
limit, the default-folder rules and that synthetic secrets, clipboard text, credential
references and names stay out of a hashed package.

### Uploading a package

With `upload` (#123), the saved package is then sent to a destination so the
user can pass on a link instead of a file. The local copy is never affected: a
skipped or failed upload still answers successfully with `path`, plus
`uploadError` saying why.

- **Which destination:** `profileId`, or the default capture destination (the
  region capture's upload destination, else the first action that uploads).
- **"Working" check:** the destination's most recent finished upload must not have
  failed with a credential or configuration error (a `credential_*` or `config_*`
  code, an authentication or permission denial, a missing or invalid bucket, a
  region mismatch, Proton setup or account errors) with the destination's current
  settings and credential reference, unless a Test connection has passed since
  (changing the settings, or entering new credentials and pressing Test connection,
  clears it), and its **Test connection**
  must pass now with the credentials already loaded. An S3 Test connection writes
  and removes a small test object, as in Settings. A passing check is reused for 10
  minutes while the settings revision stays the same, so the dialog's check and the
  upload test the destination once.
- **Upload:** through the normal operation pipeline as one item of an internal
  `diagnostics` operation, named `putgd-diagnostics-<uuid>.zip` (a random UUID, not
  the operation's) whatever the naming templates say. A name clash renames, never
  overwrites. The package is not archived a second time, and its link is copied
  only with `copyLink`.
- **Sharing:** on S3 the link is the object's public URL: on a public-read bucket
  anyone with the link can download the package. On Proton Drive the share link
  expires after 7 days.
- **History:** the upload is an ordinary history entry (operation action
  `diagnostics`), deletable from the destination like any upload. Later packages
  leave a successfully uploaded package out, its operation and its log records;
  one whose upload failed stays in, like any failure.

The reply adds `upload` (`profileId`, `label`, `provider`, `publicLink`,
`warning`, `shareExpirationDays` for Proton, `operationId`, `historyId`, `name`,
`state`), and `link` (with `linkCopied` when asked), or `uploadError`:

| Code | Meaning |
| --- | --- |
| `diagnostics_no_destination` | No action uploads anywhere; the package is local only |
| `diagnostics_destination_failing` | The last upload to this destination failed with a credential or configuration error (`details.lastError`, `details.operationId`); fix it in Settings first |
| `diagnostics_destination_unavailable` | Test connection failed (`details.error`) |
| `diagnostics_link_unavailable` | Uploaded, but the destination gives it no link (an S3 profile without a public URL) |
| a provider code | The upload itself failed, such as `s3_permission_denied`; `details.operationId` names the operation |

`diagnostics.uploadTarget` (`putgd-cli diagnostics.uploadTarget '{"profileId":"..."}'`,
MCP `get_diagnostics_upload_target`) runs the same check without a package and
answers `{available: true, profileId, label, provider, publicLink, warning,
shareExpirationDays?}` or `{available: false, reason, message}`. Settings' dialog
(#122) uses it to show "Also upload and copy a link", checked when a working
destination exists and hidden otherwise, with the warning beside it.

### The default folder

`<archive root>/diagnostics/` belongs to put.gd. After saving a package there, put.gd
deletes the packages beyond the newest 10 and any older than 30 days, matching only
its own `putgd-diagnostics-*.zip` names. A package saved anywhere else is never
deleted, and other files in the folder are left alone.

## Not yet available

The Settings controls (debug logging, Clear logs, Create diagnostics package) and the
failure-notification triggers with **Show in folder**, and the dialog's "Also upload
and copy a link" checkbox, are #122; crash backtraces and kept debug symbols are #124;
Windows is #125.
