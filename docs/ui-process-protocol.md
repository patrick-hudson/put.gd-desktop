# UI process protocol

The U11 foundation is implemented; the separately launched windows and supervisor
are still being integrated. The production tray remains in the resident. These
contracts do not yet establish a memory saving or a completed process split.

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
| `ui.snapshot` | `{}` | build manifest, resident epoch, readiness, settings document/metadata and diagnostics |
| `settings.save` | `document`, `expectedRevision` | authoritative document and metadata |
| `setup.storeCredentials` | `profileId`, `expectedRevision`, `credentials` | `setupId` |
| `setup.unlockCredentials` | `profileId`, `expectedRevision` | `setupId` |
| `setup.testDestination` | `profileId`, `expectedRevision` | `setupId` |
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
The future helper must use desktop/application ID `putgd-ui` so native capture
can exclude the application's own windows across the process boundary.

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
