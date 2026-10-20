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
`clear_logs`) and the **Clear logs…** button in Settings → General → Diagnostics,
which asks first. It deletes every log file, starts a new one with a `Logs cleared`
record and returns `removedFiles` and `removedBytes`.

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
It is created through the session method `diagnostics.bundle` (CLI and MCP), the
**Create diagnostics package…** dialog in Settings, or **Save diagnostics for this
upload** on a failure notification or a failed History row (see
[Settings and notifications](#settings-and-notifications)). It is saved locally only;
nothing is uploaded (#123 tracks the optional upload).

```sh
putgd-cli diagnostics.bundle [--operation ID] [--since 7d] [--include-names] [--output PATH]
```

The flags map to the JSON parameters `operation`, `since`, `includeNames` and
`output`; MCP `create_diagnostics_bundle` takes `operation_id`, `since`,
`include_names`, `output`, `preview` and `notify`. Two more JSON parameters:

- **`preview`:** `true` returns what the package would hold and writes nothing:
  `files`, `redaction`, `scope` (`from`, `to`, and `operationId` or `since`),
  `operations` (a count) and `operationList` (each one's `id`, `action`, `state`,
  `created` and item count), `logRecords`, `droppedLogRecords`, the `folder` it would
  be saved in and `defaultFolder`. The Settings dialog shows this before saving.
- **`notify`:** `true` makes the resident post a **diagnostics saved** notification
  with **Show in folder** after saving. Not together with `preview`.

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

### The default folder

`<archive root>/diagnostics/` belongs to put.gd. After saving a package there, put.gd
deletes the packages beyond the newest 10 and any older than 30 days, matching only
its own `putgd-diagnostics-*.zip` names. A package saved anywhere else is never
deleted, and other files in the folder are left alone.

## Settings and notifications

**Settings → General → Diagnostics** (#122) shows:

- **Debug logging**, the `preferences.diagnostics.debugLogging` checkbox. Under it,
  when it turns itself off (`diagnostics.logging.debugLoggingExpiresAt`).
- **Logs**: the file count, folder size and oldest record from
  `diagnostics.logging`, with the retention limits, and **Clear logs…**.
- A warning while `loggingDegraded` is true, with the reason and the dropped record
  count. Uploads and captures carry on regardless.
- **Last unclean exit**: `diagnostics.uncleanExit.last`, the version, when it started,
  and whether a signal (such as a logout) or an unknown cause (a crash, a forced kill
  or a power loss) ended it.
- **Create diagnostics package…**, a dialog that shows a `preview` first: what the
  package covers, its files with the upload and log record counts, the failed uploads
  included, the redaction and the folder. **Include file names** sets `includeNames`
  (off each time the dialog opens). **Choose folder…** saves the folder in
  `preferences.diagnostics.packageFolder`, so later packages go there; **Use default**
  clears it. **Save package** calls `diagnostics.bundle` with `notify`, then shows the
  file and **Show in folder**. A space under the options is kept for #123's optional
  upload.

A failed, partial or interrupted operation's notification has a **Save diagnostics
for this upload** button, and so do failed History rows (in the inspector and the row
menu). Each builds a package for that one operation, in the remembered folder or the
default one. The resident then posts **put.gd — diagnostics saved** with the file name,
size and folder, and a **Show in folder** button. Notification buttons need the
desktop's notification server to support actions (GNOME, Plasma and Cinnamon do). put.gd
asks for a 10-second notification; once Cinnamon has expired it, use the History row
instead.

**Show in folder** is the session method `diagnostics.reveal` with an absolute `path`
(MCP `show_diagnostics_package`). It asks the file manager to select the package
through `org.freedesktop.FileManager1.ShowItems`; when nothing answers, it opens the
folder with `gio open`. It accepts only put.gd's own `putgd-diagnostics-*.zip` names
(`invalid_request`) of files that still exist (`diagnostics_package_missing`).

Agents drive the same controls in an open window: `ui.diagnostics` (MCP
`settings_diagnostics`) and `ui.historySaveDiagnostics` (MCP
`history_save_diagnostics`); see [cli.md](cli.md) and [mcp.md](mcp.md).

## Not yet available

Uploading a package and copying its link is #123; crash backtraces and kept debug
symbols are #124; Windows is #125.
