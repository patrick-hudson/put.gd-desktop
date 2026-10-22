# UI process protocol

The resident supervises a separate `putgd-ui` executable for Settings, History,
Activity and About. The tray, capture/clipboard adapters and operation services
stay in `putgd`. Native acceptance and memory comparison remain pending; passing
transport/widget tests alone does not establish either.

## Command transport

`SessionClient` uses one asynchronous local socket per request with the existing
schema-1 CLI envelope. It allows 64 concurrent requests, a 1 MiB encoded request,
a 16 MiB encoded response and a 15-second deadline. Callbacks arrive at most once
on the receiver's thread. Destroying the receiver or abandoning a request suppresses
delivery; it does not cancel accepted work. There are no automatic retries.
A lost, malformed or mismatched reply after transmission reports
`details.outcomeUnknown=true`. Keep accepted operation/setup IDs and inspect them;
never infer that a failed transport prevented the effect.

The Linux resident retains its native Unix socket implementation and does not link
Qt Network for this client. Qt Network belongs to the on-demand client/helper.
The request buffer in LocalServer is cleared after a complete frame is parsed.
Transient Qt strings/JSON are released normally; this is not a cryptographic wipe.

## Resident endpoints

| Method | Parameters | Successful result |
| --- | --- | --- |
| `ui.show` | `view`: settings/updates/history/activity/about/editFile (`updates` is Settings scrolled to Updates, as the rail's Update available link opens it) | `shown` only after helper acknowledgement |
| `ui.quit` | `{}` | `quitting:true`, then explicit resident shutdown |
| `ui.snapshot` | `{}` | build manifest, resident epoch, readiness, settings document/metadata and diagnostics |
| `ui.moveDestination` | `direction` up/down, optional `profileId` | the open Settings window's new `order`, `selected` ID and `saving`, after the helper's `commandResult` |
| `ui.viewState` | `{}` | helper `state`; with a Settings checkpoint, `settings.section` and the derived `settings.captureRouting` only, and the derived `appearance` (see [state](ui-process-state.md)) |
| `settings.save` | `document`, `expectedRevision` | authoritative document and metadata |
| `setup.storeCredentials` | `profileId`, `expectedRevision`, `credentials` | `setupId` |
| `setup.unlockCredentials` | `profileId`, `expectedRevision` | `setupId` |
| `setup.testDestination` | `profileId`, `expectedRevision` | `setupId` |
| `setup.credentialHint` | `profileId`, `expectedRevision` | cached availability/mode, at most four access-key ID characters plus ellipsis, secret/token presence; no keychain IO |
| `setup.revealCredentialSecret` | `profileId`, `expectedRevision` | unlocked cached S3 `secretKey` only, on explicit GUI Show; no job record or keychain IO; rejected by CLI |
| `setup.inspect` | `id` | setup ID, kind, profile ID, state and error |

Credential input contains accessKey/secretKey and an optional sessionToken. It is
transient private socket data, never configuration, history, state checkpoints,
arguments or diagnostic output. The resident resolves the saved credential reference
and checks both accepted and observed configuration revisions. Tests use synthetic
values in an isolated temporary configuration.

Setup jobs outlive an individual connection or UI process. The resident owns secret
storage, session credentials and the existing destination-test journal callback.
There is at most one active job per profile/kind and 32 active jobs in total;
64 completed jobs remain inspectable. Records contain no credential inputs.

`setup.inspect` returns a provider result when the setup action supplies one. Proton tests
return `result.testScope: "account-folder"`: the saved account and selected folder
are accessible, with no upload or sharing test. S3 success requires its synthetic
upload, unauthenticated public GET and confirmed cleanup.

Jobs are process-lifetime status, not another persistent operation journal.
A restart cannot silently replay a credential write or destination test.
Existing settings.patch/reload and CLI/MCP response contracts remain unchanged.

## Private helper control

The supervised child's stdin/stdout carries newline JSON frames with `version:1`,
a decimal-string sequence, a finite message type and object payload. This is a
separate control channel for bootstrap, view activation, invalidation and safe
state checkpoints; upload/settings/credential commands stay on the session socket.
Frames are limited to 10 MiB, ordered without gaps, read incrementally and rate
bounded. Application output plus transport pending bytes is bounded. No frame
contents enter error messages. The Unix helper pipe adapter uses nonblocking file
descriptors and Qt socket notifiers; it has no polling thread. Other platforms
compile the adapter but currently report it unavailable, pending native pipe work.

The lifecycle/state rules are in [resident-ui-process.md](resident-ui-process.md)
and U11 of the requirements plan. Ordinary last-window close must retain safe
unsaved state before exit; minimized windows and native dialogs remain alive.
The helper uses desktop/application ID `putgd-ui`. The resident registers its PID
with the source adapter before sending bootstrap; windows appear only after a
matching resident snapshot. X11 excludes that PID. GNOME authenticates the PID
registration through its existing bridge and blocks window capture when an older
extension cannot acknowledge it. KDE queries the original captured UUID's native
application identity and excludes `putgd-ui`; it does not recapture whichever
window happens to have focus after the first reply. Missing identity while the
helper is live fails window capture explicitly. See the isolated ownership tests;
native compositor evidence is a separate gate.

## View commands

`ui.moveDestination` reaches the helper as a `command` frame with `version: 1`,
`command` and `params`. The resident sends it only to a ready helper, and bootstrap
already rejects a helper whose build differs from the resident's. A helper refuses
any other command `version` with `ui_command_unsupported`, and an older helper
without command support drops the frame, so the command fails with
`ui_command_timeout`; neither changes settings. It never starts one, and
without a helper it answers `ui_view_closed`. At most eight commands are outstanding;
each answer is a `commandResult` with `replyTo`, an error object and a `result`
object. The helper refuses unknown commands (`ui_command_unknown`) and answers
`ui_view_closed` when no Settings window is open. A helper exit, failure or shutdown
fails outstanding commands, and an unanswered command fails after ten seconds with
`details.outcomeUnknown=true`.

## Shortcut recording

Shortcut recording uses only the supervised helper's private pipe. The helper
sends `shortcutRecording` with one boolean `active` field. The resident replies
with `shortcutRecordingChanged`, the request's `replyTo`, the acknowledged
`active` value, and an error object. An absent private controller fails recording
explicitly. The UI must wait for the successful pause acknowledgement before
accepting a chord; capture state and provisional keys never enter checkpoints.

The resident suppresses action dispatch before asking the native adapter to
release global shortcuts. X11 removes passive and active keyboard grabs, waits
for a server roundtrip, and discards queued shortcut presses; XInput2 raw key
presses are ignored until the pause ends. GNOME awaits the
extension's `Bind` reply with empty accelerators. KDE awaits `setInactive` for
each registered action. Desired configuration remains separate from effective
registrations; settings changes and native service reconnection retain the pause.
Completion, cancellation, helper failure, pipe failure and helper exit restore
the latest saved bindings. A rejected partial pause restores those bindings
before returning its error. Native restore failures remain explicit errors.
The UI bounds its acknowledgement wait at twelve seconds and sends an ordered
restore request on timeout. Late replies cannot begin a timed-out capture.

Controller/widget and synthetic D-Bus tests verify these state transitions;
X11 fixture tests verify grab release/restoration. These checks do not replace
visible Cinnamon, GNOME or KDE acceptance and memory evidence.

## Process and close lifecycle

Only the resident launches the helper. Bootstrap carries the socket path, resident
epoch, compiled build manifest and safe in-memory checkpoint. The helper verifies
both its compiled manifest and the resident's live snapshot before acknowledging
readiness. Installed packages reject mixed versions of all three executables.
A missing/crashed/incompatible helper fails its pending view requests; another
explicit activation can start one replacement. There is no automatic restart loop.
Startup has a 10-second deadline. The oldest pending view request has a 12-second
end-to-end acknowledgement deadline, including startup or a helper closing and
relaunching, so the resident replies before the existing 15-second clients expire.
When a deadline or a protocol failure makes the resident kill the helper, it logs
why first (`Killing helper`, with the deadline, phase and the views still waiting),
and the exit record says put.gd killed it; see diagnostics.md.
A build mismatch explicitly asks the user to restart after pending work finishes;
repeatedly reopening a newly installed helper cannot upgrade the live resident.
The bounded `lastExit` diagnostic records only the last helper generation, exit
code and normal/crash status. A confirmed close with a nonzero exit still reports
a helper failure; tests must check the actual helper exit, not only the parent.

An ordinary final close uses a checkpoint handshake:

1. The helper validates the complete safe state locally and disables editing.
   If a checkpoint is outstanding, it waits for that acknowledgement before
   sending `closeRequested`, keeping two large snapshots out of the bounded
   output queue. A five-second checkpoint wait restores the open draft.
   Invalid or oversized state also keeps the draft open.
2. The resident validates and stores one checkpoint, enters Closing and replies
   `closeAccepted`. It leaves the existing operation engine running.
3. The helper becomes irreversibly exiting, sends `closeConfirmed`, clears secret
   fields/releases previews, and quits. The resident may kill a confirmed helper
   after two seconds if teardown fails to finish.
4. If the helper waits five seconds, it sends `closeCancel` and keeps editing
   disabled. A late `closeAccepted` cannot authorize exit during cancellation.
   Only `closeCancelled` reenables the draft. The resident resumes queued view
   requests and invalidations in order. A resident waiting ten seconds for the
   helper to confirm/cancel fails that immutable helper explicitly.

The close request sequence identifies the attempt. Acknowledgements for an older
attempt cannot close a newer draft. View activations that cross final exit are
queued for the next helper; no settings/upload/delete mutation is replayed.
Minimizing or switching desktops does not close a view. Explicitly closed minimized
views no longer count as open. About's OK/Escape path is included even though Qt
hides that dialog without a normal Close event. Native modal/transient dialogs
must finish before their owning view can close.

Settings/History/Activity state is RAM-only for this resident lifetime; credentials,
rows, decoded images and previews never enter it. The 8 MiB state ceiling fits the
existing 1 MiB configuration contract plus raw draft controls. Unknown provider
fields are rejected before export until their nonsecret state has an explicit
classification. See [state schema](ui-process-state.md) for extension rules.

Explicit Quit shuts down the helper and then the resident's existing operation
shutdown path. On desktops without a tray, Settings exposes Quit put.gd. Closing
Settings alone retains the resident. Parent pipe EOF ends the helper, which never
attempts to create another resident. stderr is drained without retaining private
helper output. The current Unix pipe implementation targets Linux; additional
platforms need their native implementation before claiming support.

## Reproduce the foundation checks

Provision dependencies as documented in [build.md](build.md), then run:

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev -R '(session_client|ui_control|ui_endpoints|cli|keychain)_integration|local_ipc_contract'
cmake --preset asan
cmake --build --preset asan --target test_session_client test_ui_control test_ui_endpoints
ctest --preset asan -R '^(session_client|ui_control|ui_endpoints)_integration$'
```

The [structured receipt](evidence/2026-10-08-ui-foundations.json) retains source
hashes, original failing tests, fixed ownership failures, JUnit and commands.
No fixture is evidence of native desktop support. Final U11 acceptance still
requires the same source on Cinnamon, GNOME and KDE, VNC witnesses, ongoing work
across UI exit, and total resident-plus-helper memory measurements.

Before Qt loads a platform/theme plugin, the helper duplicates its inherited pipe ends into private close-on-exec descriptors and redirects normal stdin to `/dev/null` and stdout to stderr. GTK/GLib diagnostics cannot enter the versioned protocol. Direct terminal launches are rejected before changing terminal flags. The `--version` path stays available without a resident or display.
