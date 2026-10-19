# Logs and diagnostics

put.gd keeps a persistent log so a failed capture or upload can be worked out
afterwards (#74). This page covers where the log lives, what a record holds, what
is never written, how long files are kept and how unclean exits are reported.
The `diagnostics` method ([cli.md](cli.md)) reports the log's current state.

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

## Not yet available

The diagnostics package (`diagnostics.bundle`, #74 §3) follows in a separate pull
request. The Settings controls (debug logging, Clear logs, Create diagnostics package)
and failure-notification triggers are #122; upload-and-copy-link for packages is #123;
crash backtraces and kept debug symbols are #124; Windows is #125.
