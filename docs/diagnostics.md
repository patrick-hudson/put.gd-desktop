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
  error), `Multipart upload completed` (part count, bytes, attempt, elapsed time and
  the completion's `requestId`/`extendedRequestId`), `Uncertain upload reconciled`,
  `Name check failed`, `Incomplete upload aborted` or `Incomplete upload kept` (an
  artifact cleanup's abort of a failed item's multipart upload, with the error when S3
  refused it), `Sharing finished`, and
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

With debug logging on, an S3 multipart upload adds one `Multipart part uploaded`
record per successful part request (`putgd.s3`): `partNumber`, `bytes`, `attempt`,
`elapsedMs`, the `lane` that sent it and how many `lanes` the upload had then, and
S3's `requestId` and `extendedRequestId`, so a slow or retried upload can be traced
part by part with AWS support. These records carry numbers and request IDs only:
never the file name, object key, bucket, endpoint, upload ID or a signed URL, so they
need no hashing in a diagnostics package. The worker sends them in batches of at most
64; past 256 queued records it counts them in an `Upload diagnostics dropped` record
rather than slowing the upload.

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

- `cause`: `crash` (with `signal` and the handler's `crash` record, logged as an
  error, "The previous resident crashed"), `signal` (with `signal`, logged at info,
  as a logout normally is) or `unknown` (SIGKILL, power loss or a crash the handler
  could not record, logged as a warning);
- `previousPid`, `previousStart`, `previousVersion`, `sameBoot` and `detectedAt`.

The last 10 records are kept in `unclean-exits.json`. `diagnostics.uncleanExit` holds
`detected` (this start found one), `last` and `recent` (newest first).

## Crash records

Every put.gd executable (`putgd`, `putgd-ui`, `putgd-capture`, `putgd-kde-region`,
`putgd-record` and `putgd-s3-worker`) links a small crash handler (#124,
`src/core/CrashHandler.cpp`) that installs itself before `main`, from the
executable's `.preinit_array`. ld.so runs that before any shared library's
constructor, so the handler is in place before any other code in the process
runs (#211). For SIGSEGV,
SIGBUS, SIGILL, SIGFPE and SIGABRT it runs on an alternate signal stack, walks the
crashing thread's stack with `_Unwind_Backtrace` (called once at startup, so
nothing is loaded or allocated while crashing) and writes, with `write(2)` only:

- the process, pid, thread, build version, signal, `si_code`, fault address and PC;
- each frame's raw address, the module it falls in and its offset in that module;
- for each module named by a frame: its load address, GNU build ID and file.

The module map comes from `dl_iterate_phdr`, collected at startup (and by the
resident again 15 seconds in, after the S3 module and Qt plugins have loaded). A
frame in a library loaded later is still resolved, from `/proc/self/maps` and the
ELF headers mapped in memory. Nothing else is recorded: no registers beyond the PC,
no memory and no variables, so a record holds no user data.

Afterwards the handler restores the signal's previous disposition and lets it
happen, so the process still dies by that signal: the exit status, a core dump,
systemd-coredump and any handler installed earlier behave as before. The handler
records only the first crashing thread; a crash on another thread meanwhile waits
up to two seconds for it.

`sigaltstack` is per thread, and a new thread starts without one. The crash library
therefore defines `pthread_create`, and each executable exports it
(`--export-dynamic-symbol`), so Qt, libstdc++, GStreamer, the AWS SDK and put.gd's
own code all start their threads through it (#156). It wraps the start routine: the
new thread maps a 64 KiB alternate stack with a guard page below it
(`MAP_NORESERVE`, untouched unless the thread crashes) before running the routine,
and unmaps it when the thread ends. A stack overflow on any thread is therefore
recorded, from that thread, including a thread a library starts from its own
constructor before `main` (#211). At that point libc may not have set up `getenv` or
`program_invocation_short_name` yet, so the handler reads `PUTGD_CRASH_HANDLER`,
`PUTGD_TEST_CRASH` and its process name from the arguments and environment that
ld.so passes to `.preinit_array` functions. With `PUTGD_CRASH_HANDLER=0`, or in
sanitizer builds, `pthread_create` passes straight through. The native package
audit rejects a crash-handled program that doesn't export it.

- **The resident** writes its record to `$XDG_STATE_HOME/putgd/running.crash`,
  opened (0600, `O_NOFOLLOW`) when it starts. The next start reads it beside the
  `running.json` marker, so the unclean-exit record gets `cause` `crash` and the
  parsed record, which reaches the log, `diagnostics.uncleanExit` and the next
  package. A clean shutdown removes the file.
- **Helpers** write to stderr, which the resident already collects. Their
  `putgd-crash` lines become one `Helper crash record` (level `error`, field
  `crash`), outside the 64 KiB per-generation output bound, before `Helper crashed`.

The `crash` object has `recordVersion` (1), `process`, `pid`, `thread`, `version`,
`signal`, `signalName`, `code`, `faultAddress`, `pc`, `time`, `frames`
(`index`, `address`, `module`, `offset`), `modules` (`index`, `loadBias`,
`buildId`, `file`) and `complete` (false when the record was cut short). Offsets
are addresses in the module's own address space, which is what `eu-addr2line`
takes. The raw line format, which `tools/diagnostics/symbolize_crash.py` also
reads, is `putgd-crash<TAB>kind<TAB>...`:

```text
putgd-crash	begin	1	putgd-ui	4242	4242
putgd-crash	version	0.1.1-nightly.312
putgd-crash	signal	11	SIGSEGV	1	0x0
putgd-crash	time	1760918400
putgd-crash	pc	0x55d0c8e41a2b
putgd-crash	frame	0	0x55d0c8e41a2b	0	0x3fa2b
putgd-crash	frame	1	0x7f3a1c2d4e10	1	0x2d4e10
putgd-crash	module	0	0x55d0c8e02000	3c4f...e1	/usr/bin/putgd-ui
putgd-crash	module	1	0x7f3a1c000000	9b2a...07	/usr/lib/x86_64-linux-gnu/libQt6Core.so.6.8.2
putgd-crash	end	2
```

The handler is compiled out of ASan/TSan builds (`PUTGD_SANITIZER`), where the
sanitizers own these signals, and `PUTGD_CRASH_HANDLER=0` turns it off at run
time. It does not replace a handler something else installed first: that one runs
afterwards. Qt installs none outside its test library.

### Development crash trigger

Development builds (`PUTGD_CRASH_TRIGGER`, on unless the build type is a release
type or `PUTGD_RELEASE_TAG` is set) read
`PUTGD_TEST_CRASH=<process>:<segv|abort|ill|fpe|bus|overflow>[:<delay ms>[:worker]]`:
the executable of that name crashes after the delay, on its main thread or, with
`worker`, on a thread of its own. `overflow` recurses until the stack runs out.
Helpers inherit the resident's environment, so one variable crashes the resident
or a chosen helper:

```sh
PUTGD_TEST_CRASH=putgd:segv:5000 ./build/dev/putgd               # the resident, 5 s in
PUTGD_TEST_CRASH=putgd-ui:abort:2000 ./build/dev/putgd           # the UI helper, 2 s after it starts
PUTGD_TEST_CRASH=putgd-record:overflow:1000:worker ./build/dev/putgd   # a stack overflow on a recorder thread
```

Configuring a release build with the trigger fails, and the native package audit
(`tools/releases/audit_native.py`) rejects any packaged binary that contains
`PUTGD_TEST_CRASH`. `tests/unit/test_crash_handler.cpp` crashes a child process
with each path (file, stderr, a library loaded later, the trigger) and symbolizes
its frames. It also overflows the main thread, a `std::thread`, a `QThread` and a
thread that a dlopen'd library starts, and checks that every thread gets its
64 KiB stack and gives it back. A linked fixture library
(`tests/fixtures/crash_early_library.cpp`) starts a thread from its constructor,
before the executable's own constructors; the test checks that the thread has its
stack and that overflowing it gives a complete record (#211):

![Before: with the handler installed from a constructor, the library constructor's thread has no alternate stack and its overflow leaves an empty record. After: installed from .preinit_array, the test passes and the record is complete, from that thread, symbolized to overflowInEarlyThread](evidence/crash-early-threads-211/crash-early-thread-before-after.png)

### systemd-coredump (option B)

Where `coredumpctl` exists, a package also holds `coredumps.json`: for the newest
five crashes of put.gd executables in the package's window, the time, pid, signal,
executable, storage state and the text stack trace systemd-coredump already made
with elfutils, keeping only its `Module ... build-id` and frame lines. The command
line, environment, host name and unit are left out, and the core file itself is
never read or included, since it holds process memory. Fedora installs
systemd-coredump by default; Debian, Ubuntu and Mint usually do not, and then
`coredumps.json` says `available: false`.

## Kept debug symbols

Release builds compile with `-g` and, when each product binary links, move its
debug information to `<binary>.debug` (`objcopy --only-keep-debug`, then
`--strip-debug --add-gnu-debuglink`), before the install step hashes the S3 module
or the AppImage inventories its files (`PUTGD_SPLIT_DEBUG`: `AUTO` splits Release
builds, `ON` any build type, `OFF` none; see [build.md](build.md)). Packages carry
only the stripped binaries, with their GNU build IDs.

Each package job then runs `tools/releases/debug_symbols.py stage`, which checks
every product binary's `.debug` file (same build ID, a debug link, debug sections
only in the `.debug` file) against the binary the package installs, and stages the
files by build ID. The release collects all five builds (four distro packages and
the AppImage) into one asset, `putgd-<version>-debug-x86_64.tar.zst`, laid out like
a debuginfod cache (`.build-id/<2 hex>/<rest>.debug`) with a `manifest.json` that
names each build's programs and build IDs. Pull requests build the same asset as
the `debug-symbols-dry-run` artifact. See [release.md](release.md).

The builds split with zlib-compressed debug sections, which keeps the staged files
and the jobs' artifacts small, but leaves a compressor nothing to find. The asset
is about half that size, around 213 MB instead of 433 MB for five builds (#162).
`debug_symbols.py` decompresses the sections and runs `dwz` on each build's files.
The DWARF its programs share moves into `.dwz/<label>.debug`, which each file's
`.gnu_debugaltlink` names relative to itself (`../../.dwz/<label>.debug`). Then
`zstd -19 --long=27` compresses the whole tar. That window still unpacks with
plain `tar --zstd -xf`. Keep the extracted tree together: a `.debug` file read
without its `.dwz` file is missing the shared types and some names.

## Symbolizing a crash record (maintainers)

1. Get the records: the package itself, a log file, `running.crash`, or helper
   stderr all work.
2. Get the debug files for the build in the record (`version`, and each module's
   build ID): download and extract that release's debug asset.

   ```sh
   gh release download v0.1.1-nightly.312 -p 'putgd-*-debug-x86_64.tar.zst'
   mkdir debug && tar --zstd -xf putgd-0.1.1-nightly.312-debug-x86_64.tar.zst -C debug
   ```

   For a local build, the build tree itself holds `<binary>.debug` files
   (configure with `-DPUTGD_SPLIT_DEBUG=ON`, or use a Debug build's binaries).
3. Symbolize. The script finds each module's debug file by build ID in the
   extracted asset, then asks debuginfod for distro libraries such as Qt and glibc:
   `debuginfod-find` when it is installed, otherwise the servers in
   `DEBUGINFOD_URLS` (or `/etc/debuginfod/*.urls`) directly over HTTPS, cached in
   `~/.cache/putgd-debuginfod`. Fedora, Debian and Ubuntu run servers; use the one
   for the distro the record came from, such as `https://debuginfod.debian.net`,
   `https://debuginfod.ubuntu.com` or `https://debuginfod.fedoraproject.org`, or
   `https://debuginfod.elfutils.org`, which federates several:

   ```sh
   python3 tools/diagnostics/symbolize_crash.py putgd-diagnostics-20261020-101500Z.zip --debug-dir debug
   ```

   By hand, each frame is one lookup in the module's debug file, which finds the
   build's shared `.dwz` file by its relative link. Frame 0 is the faulting
   instruction; the others are return addresses, so look up `offset - 1`:

   ```sh
   eu-addr2line -f -C -i -e debug/.build-id/3c/4f...e1.debug 0x3fa2b
   llvm-symbolizer --obj=debug/.build-id/3c/4f...e1.debug 0x3fa2b
   debuginfod-find debuginfo 9b2a...07    # a distro library's debug file
   ```

   `eu-unstrip -n --core` is not needed: the record already names each module's
   build ID and offset.

## Diagnostics package

One `.zip` a user can send to a maintainer, who can then work out what happened.
It is created through the session method `diagnostics.bundle` (CLI and MCP), the
**Create diagnostics package…** dialog in Settings, or **Save diagnostics for this
upload** on a failure notification or a failed History row (see
[Settings and notifications](#settings-and-notifications)). It is always saved
locally first; it can then be uploaded to a working destination (see "Uploading a
package" below).

```sh
putgd-cli diagnostics.bundle [--operation ID] [--since 7d] [--include-names] [--output PATH] [--upload [--profile ID] [--copy-link]]
```

The flags map to the JSON parameters `operation`, `since`, `includeNames`,
`output`, `upload`, `profileId` and `copyLink`; MCP `create_diagnostics_bundle`
takes `operation_id`, `since`, `include_names`, `output`, `preview`, `notify`,
`upload`, `profile_id` and `copy_link`. Two more JSON parameters:

- **`preview`:** `true` returns what the package would hold and writes nothing:
  `files`, `redaction`, `scope` (`from`, `to`, and `operationId` or `since`),
  `operations` (a count) and `operationList` (each one's `id`, `action`, `state`,
  `created` and item count), `logRecords`, `droppedLogRecords`, the `folder` it would
  be saved in and `defaultFolder`. The Settings dialog shows this before saving.
- **`notify`:** `true` makes the resident post a **diagnostics saved** notification
  with **Show in folder** after saving. Not together with `preview`.

A preview writes nothing, so it takes none of `upload`, `profileId` and `copyLink`.

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
| `coredumps.json` | systemd-coredump's text stack traces for put.gd executables in the window, or `available: false` (see "systemd-coredump" above); never a core file |
| `operations.json` | Each operation's ID, action, state, phase, creation time, cancellation, copy status, provider, profile and error, and per item its ID, MIME type, category, size, name, upload name, upload/share/remote status, phase, local-copy status, error (with HTTP status, provider code and S3 request IDs), object key, direct URL and the number of upload attempts in the log window |
| `logs/putgd.jsonl` | The log records in the window, oldest first, with the package redaction applied |

The package is at most 20 MiB. The other files go first; the log records fill the
rest, newest first, so when it would go over, the oldest records are dropped and
counted in `droppedRecords`.

**Never included:** credentials, secrets or tokens, keychain data, signed URLs,
artifact or preview bytes, screenshots, clipboard content and the raw
`history.sqlite` and core dumps.

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
  expires after 7 days: Proton reported a link created at 17:48:43Z expiring
  7 days later, and the link opened without a Proton session (checked live
  on 2026-10-20, [evidence](evidence/2026-10-20-round10-proton/README.md)).
- **History:** the upload is an ordinary history entry (operation action
  `diagnostics`). On S3 it can be deleted from the destination like any upload.
  Proton refuses remote deletion (`unsupported_capability`), as for every
  Proton upload; use **Revoke link** to end URL access, and delete the file in
  Proton Drive if needed. Later packages
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
uses it to offer the upload (see below).

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
  file and **Show in folder**.
- **Also upload to <destination> and copy a link** (#158). Opening the dialog calls
  `diagnostics.uploadTarget` for the default destination in the background (it can
  take minutes: a Test connection); the preview and **Save package** don't wait for
  it. While it runs, a line under the options says it is checking. When the
  destination works, the checkbox appears, ticked, with the sharing `warning` under
  it, and the button reads **Save and upload**: the save adds `upload`, `copyLink`
  and that `profileId`. Afterwards the dialog shows the link and whether it was
  copied, or why it wasn't uploaded, and the saved file stays either way. When the
  destination doesn't work, the checkbox stays hidden and the line says why: no
  upload destination, the last upload failed with a credential or configuration
  error, or Test connection failed (with the destination's own message).

A failed, partial or interrupted operation's notification has a **Save diagnostics
for this upload** button, and so do failed History rows (in the inspector and the row
menu). Each builds a package for that one operation, in the remembered folder or the
default one. The resident then posts **put.gd — diagnostics saved** with the file name,
size and folder, and a **Show in folder** button; after an upload from the dialog it
also says where the package went, or why it wasn't uploaded. Notification buttons
need the desktop's notification server to support actions (GNOME, Plasma and
Cinnamon do). Plain notifications ask for 10 seconds. On GNOME Shell and Plasma a
notification with buttons does too: both keep it, buttons and all, in their
notification lists after the popup. Elsewhere, including Cinnamon, it is sent with
the `resident` hint and `expire_timeout` 0 (#151): Cinnamon otherwise removes an
app's notifications as soon as one of its windows takes focus, and with them the
buttons. put.gd closes a resident notification itself once a button or its body is
clicked. The server is identified by `GetServerInformation`. The History row offers
the same action.

**Show in folder** is the session method `diagnostics.reveal` with an absolute `path`
(MCP `show_diagnostics_package`). It asks the file manager to select the package
through `org.freedesktop.FileManager1.ShowItems`; when nothing answers, it opens the
folder with `gio open`. It accepts only put.gd's own `putgd-diagnostics-*.zip` names
(`invalid_request`) of files that still exist (`diagnostics_package_missing`).

Agents drive the same controls in an open window: `ui.diagnostics` (MCP
`settings_diagnostics`) and `ui.historySaveDiagnostics` (MCP
`history_save_diagnostics`); see [cli.md](cli.md) and [mcp.md](mcp.md).

## Not yet available

Wiring the upload into the **Create diagnostics package…** dialog ("Also upload and
copy a link") is #158; Windows is #125.
