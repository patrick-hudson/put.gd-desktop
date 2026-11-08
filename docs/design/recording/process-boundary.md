# Screen recording: process boundary

Part of [Screen recording](../recording.md).

## Process boundary

`RecordingService` (`src/app/RecordingService.*`) owns the session, journal
intent, state machine, edit model and export staging under
`<data>/recording/<id>/`; `RecordingBackend` (`src/app/RecordingBackend.h`)
is the only media seam. The resident never links GStreamer or FFmpeg and never
decodes frames or audio.

`RecordingHelperBackend` (`src/app/RecordingHelperBackend.*`) is the production
backend. It launches one `putgd-record` process per recording session from the
resident's own directory and speaks the private UiControl framing over the
child's pipes, mirroring the capture and editor helpers:

- `bootstrap {role: record, nonce, generation, build, appearance {scheme}}` → `ready` echoing the
  envelope and the helper's build manifest; a mismatch is `recording_incompatible`,
  silence past the startup deadline (10 s) is `recording_helper_start_timeout`.
- `command {requestId, command, params}` → `commandResult {requestId, ok, result}`
  or `{..., ok: false, code, message, details}`; commands are `capabilities`,
  `prepare`, `start`, `pause`, `resume`, `stop`, `cancel`, `raise`, `overlay`,
  `preview`, `export` and `review` (the review window's controls, #283). Deadlines (`recording::helper*Ms` in
  `src/app/RecordingBackend.h`): 15 s per command, 30 s for `stop`, 600 s for
  `prepare` (a hands-on drag selection or OS consent), 600 s for `export`; a missed deadline is
  `recording_helper_timeout` and leaves the session where it was. The session
  methods that wait on a command reply only after the helper answers, so
  `putgd-cli` waits 15 s past the helper's deadline for them (45 s for `stop`
  and `action.invoke`, which can toggle Stop, 30 s for the other recording
  commands; `LocalServer::replyDeadlineMs`). `prepare` and the exports
  reply when they start.
- `event {event: progress|stopped|failed, ...}` from the helper is forwarded to
  the service with the session ID; an unexpected exit fails the session with
  `recording_helper_crashed` (or `recording_helper_exited`) and never uploads.
- `appearance {scheme: dark|light}` from the resident whenever the resolved
  interface scheme changes while the helper runs; the bar, review window and
  selector restyle at once (#198, [appearance](../appearance.md#recording-overlays)).
  Any other value ends the helper as a protocol error.
- `shutdown` after upload, cancel or failure; the helper must exit normally
  within 2 s or it is killed. Discard settles outstanding requests before
  shutdown, so a late `commandResult` from the authenticated, ready helper
  during teardown is ignored rather than treated as an unknown request.
  Envelope checks still apply; unknown request IDs during an active session
  remain protocol errors. Cancel on the bar, `recording.cancel` and review
  Discard use this graceful teardown path. Only control metadata crosses the
  pipes: frames, audio and files stay in the helper and its private staging.

`putgd-record --capabilities` is a separate transient run whose JSON the backend
caches for the resident's lifetime (a failed probe is retried after a minute).
It never opens a capture source, audio device or portal session. The result
carries `session` (x11/wayland/none), the GStreamer version and element
inventory (`ximagesrc`, `pipewiresrc`, `pulsesrc`, the H.264 encoders in
preference order, muxers, `webpenc` animation support), the FFmpeg library
versions with encoders, muxers and filters, the derived `formats`/`codecs`
the export side can produce, `liveCapture {source, encoder, audio}` and the
honest `available`/`reason` pair. Registry presence is not proof (stock
Fedora's FFmpeg lists `h264_vaapi` while its free Intel driver has no H.264
encode profile, and its `openh264enc` loads a placeholder library), so the
probe tries the H.264 path for real; see "Packages and codecs". The live
pipeline still starts encoders in order and reports the one actually used.
CMake builds the helper only when `gstreamer-1.0 >= 1.24` and FFmpeg
libraries `>= 6.1` are found (`PUTGD_RECORDING`, default on, also for the
AppImage, which builds it against its own pinned FFmpeg); a resident without
the sibling binary reports `recording_helper_missing`.

### Live free-space stop (#254)

The transient capture helper samples the working clip's filesystem every 250 ms,
including while paused, and refreshes the sample before a byte-limit stop. It
stops through EOS when free space above the existing system reserve falls to
`2 * max(clipBytes, 64 MiB) + 32 MiB`. The two copies estimate the eventual export
and upload artifact; the working clip is already deducted from free space and
is not counted again. The extra 32 MiB leaves headroom for queued encoded media
and Matroska finalization. This is a conservative estimate, not a filesystem
reservation or a guarantee of an export's size. The system reserve remains
5% of volume capacity, bounded to 1–4 GiB.

The minimum prepare admission is three 64 MiB budgets above that reserve. At
start the live threshold is 160 MiB, below the admitted 192 MiB, so a recording
on a minimally admitted volume can begin. Space recovered before the threshold
is crossed does not latch a stop. When disk and byte pressure coincide, the
fresh disk observation takes precedence.

A successfully drained and decodable clip enters private review with
`stopReason: budget` and `stopDetails.reason: disk_space`. Alongside the existing
budget and duration fields, `stopDetails` retains the triggering `freeBytes`,
`reserveBytes`, `availableBytes`, `requiredBytes`, `finalizationReserveBytes`,
and `clipBytes`. Those figures do not change if another process releases space
while the helper is finalizing. The review banner and tray explain the stop;
CLI `recording.inspect` and MCP `get_recording` expose the same details.

Sampling cannot prevent an arbitrary writer from consuming all remaining space
between checks or during finalization. An explicit GStreamer no-space error
reports `recording_disk_full`, `reason: disk_space`, and `finalized: false`,
with the original error under `error.details.cause` (including its existing bounded
debug details). Other write, encoder, drain or preview errors retain their actual
error code and message: a prior low-space stop is not proof of their cause.
When they follow a disk stop, `error.details.stopDetails` retains the triggering
sample and `error.details.finalized: false` states that no reviewable clip was retained. The operation
fails and private staging is discarded under the existing lifecycle. Nothing
is uploaded. Free space may also need to be restored before export; a failed
export keeps an existing review open. Preview, save copy and review never
authorize Upload.

This contract depends on companion [code PR #572](https://github.com/patrick-hudson/put.gd-desktop/pull/572)
at `4ee657db2d7ee00b2e784a6d7b3f0e4e722f7dd6`, which merges before these docs.
The [pipeline mapping](https://github.com/patrick-hudson/put.gd-desktop/blob/4ee657db2d7ee00b2e784a6d7b3f0e4e722f7dd6/src/record/RecordingPipeline.cpp#L361)
sets `error.details.cause`; the [session failure handler](https://github.com/patrick-hudson/put.gd-desktop/blob/4ee657db2d7ee00b2e784a6d7b3f0e4e722f7dd6/src/record/RecordSession.cpp#L82)
adds `stopDetails` and `finalized` inside `error.details`. The resident
[forwards those details unchanged](https://github.com/patrick-hudson/put.gd-desktop/blob/4ee657db2d7ee00b2e784a6d7b3f0e4e722f7dd6/src/app/RecordingService.cpp#L274)
and [stores the same Error](https://github.com/patrick-hudson/put.gd-desktop/blob/4ee657db2d7ee00b2e784a6d7b3f0e4e722f7dd6/src/app/RecordingService.cpp#L207) on the failed operation.
