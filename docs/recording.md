# Screen recording

Status: **slices 1 and 2 of the [recording plan](plans/2026-10-17-2230-feat-screen-recording-plan.md)**.
The contract below is implemented and tested against a fake backend and
against the real helper handshake. The transient `putgd-record` helper exists
with its identity handshake, command/event framing and a truthful media probe,
but no capture, review or export pipeline yet, so every installation still
reports `recording.capabilities` with `available: false` (reason
`capture_not_implemented`, or a more specific gap) and `recording.prepare`
returns `recording_unavailable`. Nothing here is desktop acceptance evidence.

## Workflow

prepare → ready → start → recording ⇄ paused → stop → **private review**
(trim, timed masks, preview, local copy) → explicit **Upload** → the ordinary
operation pipeline (immutable artifact, archive, destination, history,
clipboard). Cancel is available in every non-terminal state and uploads nothing.

- Stop **always** opens the private review, including an unedited clip and a
  stop while paused. No recording bytes reach the artifact store, archive or
  a destination before the separate `upload` command; preview, edits, local
  copies, closing the review, cancel and restart never authorize it.
- One recording session exists at a time, and it excludes the still-image
  editor (`recording_active` / `editor_active`). Its ID equals the operation
  ID that the journal holds in state `recording`. A restart turns that
  operation into `interrupted` (`recording_interrupted`), discards the
  private staging and never resumes or uploads it.
- Upload exports the current edit revision, validates the file (nonempty,
  within `limits.maxFileBytes`), then admits exactly those bytes as a
  `recording` payload: streamed from private staging, declared MIME, named
  by `naming.recordingTemplate`, category `videos` for MP4/WebM and `images`
  for GIF/WebP (animated bytes are stored as-is, never normalized). Retry
  reuses the stored artifact; a failed export keeps the review open and the
  unedited clip private.

## Time, geometry and the edit model

- Time is an integer count of **microseconds on the recorded-clip timeline**
  with pauses removed; intervals are half-open `[start, end)`. Export rounds
  to the nearest frame boundary at the recording's `fps`
  (`recording::alignToFrame`), identically for preview and export.
- The output canvas is fixed when the recording starts (`canvas: [w, h]`);
  masks use canvas pixels. A topology change during recording ends it with a
  helper failure rather than moving the canvas.
- `edit` holds one contiguous trim (`trimStart`, `trimEnd` or null for the clip
  end) and up to 64 masks `{id, kind: blur|solid, rect: [x,y,w,h], start, end,
  strength}`. Masks are stored in source time. At export each mask is
  intersected with the retained interval and rebased by the trim start:
  retaining 10–30 s makes a source mask at 12–15 s active at output 2–5 s, and
  a mask wholly outside the trim is dropped. Changing the trim never moves a
  mask on the source timeline.
- Every accepted `edit` advances `revision`. `upload` and `edit` take the
  revision they were reviewed at and fail with `recording_stale` otherwise.
- Animated exports (GIF, WebP) are silent and must fit
  `limits.recordingAnimationSeconds` (retained duration) and
  `limits.recordingAnimationEdge` (output long edge); set `maxEdge` on the
  recording to downscale explicitly. Nothing is scaled or shortened silently.

## Session methods

All methods go through `putgd-cli` unchanged. `id` is the recording ID.

| Method | Params | Result / effect |
| --- | --- | --- |
| `recording.capabilities` | | availability and reason, target kinds, formats, codecs per format, audio sources, `active` recording ID, limits, `timeUnit`; no capture, no prompt |
| `recording.prepare` | `target {kind: monitor\|desktop\|region\|window, identity?, rect?}`, `format mp4\|webm\|gif\|webp`, `codec?`, `fps? (1–60)`, `audio? {system?, microphone?}`, `maxEdge?` | durable intent, then ready bounds/controls and any OS consent; replies at once with `state: preparing`; poll `inspect` for `ready` |
| `recording.start` / `pause` / `resume` / `stop` | `{id}` | acknowledged once the backend reached the state; a duplicate of the in-flight command returns the current state, a different one is `recording_busy`; `stop` returns `durationUs` and enters `reviewing` |
| `recording.cancel` | `{id}` | discards the clip; the operation ends `cancelled` |
| `recording.inspect` | `{id}` | state, pending command, target, config, canvas, `durationUs`, `retainedUs`, edit, progress, error |
| `recording.raise` / `recording.overlay` | `{id}` / `{id, visible}` | overlay control; the recording indicator stays visible |
| `recording.edit` | `{id, revision, commands[1–100]}` | atomic `setTrim {start, end\|null}`, `resetTrim`, `addMask {mask}`, `updateMask {maskId, mask}`, `removeMask {maskId}` |
| `recording.preview` | `{id, time, maxEdge? 64–1600}` | one PNG frame (base64 `png`) at clip time with current trim/masks, plus `retained`, `outputTimeUs`, `revision`; returned only to the requesting client |
| `recording.saveCopy` | `{id, path, overwrite?}` | exports to an absolute path with the format's extension; no upload |
| `recording.upload` | `{id, revision}` | the only publishing action; returns `state: submitted` and the linked `operationId` |

Codec rules: MP4 takes `h264` (default) or `h265`; WebM takes `vp9`; GIF and
WebP take none. `audio.system` is the computer's playback and is refused for
GIF/WebP (`invalid_request`); `audio.microphone` is reserved and refused
(`recording_unsupported`). Errors: `recording_unavailable`,
`recording_unsupported`, `recording_active`, `recording_state`,
`recording_busy`, `recording_stale`, `recording_mask_unknown`,
`recording_animation_too_long`, `recording_animation_too_large`,
`recording_export_failed`, `file_exists`, plus the shared `invalid_request`,
`queue_full`, `busy_selecting`, `editor_active` and `output_required`.

## Settings

| Field | Meaning |
| --- | --- |
| `actions.record` | Optional route like the other actions; absent documents inherit the region route with no shortcut |
| `naming.recordingTemplate` | Optional, default `Screen-Recording-{yyyy}-{MM}-{dd}-{HH}-{mm}-{ss}.{ext}` |
| `limits.recordingAnimationSeconds` | Optional, default 60 |
| `limits.recordingAnimationEdge` | Optional, default 1280 |

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

- `bootstrap {role: record, nonce, generation, build}` → `ready` echoing the
  envelope and the helper's build manifest; a mismatch is `recording_incompatible`,
  silence past the startup deadline (10 s) is `recording_helper_start_timeout`.
- `command {requestId, command, params}` → `commandResult {requestId, ok, result}`
  or `{..., ok: false, code, message, details}`; commands are `capabilities`,
  `prepare`, `start`, `pause`, `resume`, `stop`, `cancel`, `raise`, `overlay`,
  `preview` and `export`. Deadlines: 15 s per command, 30 s for `stop`, 120 s for
  `prepare` (OS consent), 600 s for `export`; a missed deadline is
  `recording_helper_timeout` and leaves the session where it was.
- `event {event: progress|stopped|failed, ...}` from the helper is forwarded to
  the service with the session ID; an unexpected exit fails the session with
  `recording_helper_crashed` (or `recording_helper_exited`) and never uploads.
- `shutdown` after upload, cancel or failure; the helper must exit normally
  within 2 s or it is killed. Only control metadata crosses the pipes: frames,
  audio and files stay in the helper and its private staging.

`putgd-record --capabilities` is a separate transient run whose JSON the backend
caches for the resident's lifetime (a failed probe is retried after a minute).
It never opens a capture source, audio device or portal session. The result
carries `session` (x11/wayland/none), the GStreamer version and element
inventory (`ximagesrc`, `pipewiresrc`, `pulsesrc`, the H.264 encoders in
preference order, muxers, `webpenc` animation support), the FFmpeg library
versions with encoders, muxers and filters, the derived `formats`/`codecs`
the export side can produce, `liveCapture {source, encoder, audio}` and the
honest `available`/`reason` pair. Encoder presence is registry/library
presence, not hardware proof: stock Fedora's FFmpeg lists `h264_vaapi` and
`hevc_vaapi` while its free Intel driver has no H.264/HEVC encode profiles, so
the capture slice must select encoders by a real negotiation attempt and
report the one actually used. CMake builds the helper only when
`gstreamer-1.0 >= 1.24` and FFmpeg libraries `>= 6.1` are found
(`PUTGD_RECORDING`, default on; off for the AppImage); a resident without the
sibling binary reports `recording_helper_missing`.

## Agent and MCP control

| Tool | Session method |
| --- | --- |
| `get_recording_capabilities` | `recording.capabilities` |
| `prepare_recording` | `recording.prepare` |
| `control_recording` (start/pause/resume/stop/cancel/raise) | `recording.<command>` |
| `get_recording` | `recording.inspect` |
| `set_recording_overlay` | `recording.overlay` |
| `edit_recording` | `recording.edit` |
| `preview_recording` (MCP image) | `recording.preview` |
| `save_recording_copy` | `recording.saveCopy` |
| `upload_recording` | `recording.upload` |

## Verification

- `recording_model_contract` (`tests/unit/test_recording_model.cpp`):
  transitions, target/config parsing, trim/mask rebasing, atomic invalid
  edits, frame alignment, animation budgets, settings fields, artifact
  admission of a `recording` payload with the naming template.
- `recording_service_integration` (`tests/integration/test_recording_service.cpp`):
  the full lifecycle through `LocalServer` with a fake backend, including
  stop while paused, stale revisions, local copies, export failure, cancel,
  helper events, animation limits, malformed requests, editor exclusion and
  restart interruption.
- `recording_helper_integration` (`tests/integration/test_recording_helper.cpp`):
  the helper supervisor against a fixture helper (probe caching, lifecycle,
  progress and helper-initiated stop events, crash, silence, wrong build,
  export timeout, broken probes, missing helper) and, when built, the product
  `putgd-record`: its probe JSON, the identity handshake, the explicit
  `capture_not_implemented` refusal and its normal exit.
- `tests/integration/test_mcp.py`: tool → method forwarding parity and the
  preview image route against the synthetic peer.
- `tests/integration/test_config_schema.py`: optional `record` action,
  template and limits.
