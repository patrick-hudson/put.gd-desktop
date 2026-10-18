# Screen recording

Status: **slices 1 to 3.3 of the [recording plan](plans/2026-10-17-2230-feat-screen-recording-plan.md)**.
The contract below is implemented and tested against a fake backend and the
real helper. On **X11 sessions** the `putgd-record` helper records monitors, the
desktop, regions and windows through `ximagesrc`, pauses and resumes on one
clip timeline, previews frames with masks, and exports MP4 (H.264, H.265), WebM
(VP9), GIF and animated WebP with trim and timed masks; the agent path (CLI
and MCP) is complete, and the drag selection, ready bounds, outside dimming
and floating control bar are in place, and every Stop opens the private review
window. The tray's **Capture Screen Recording** entry, the dashboard's **Record
screen** button and the `record` global shortcut start one (see "Tray and
shortcut" below). Wayland acquisition (portal + `pipewiresrc`) is not
implemented, so Wayland installations report `available: false` with reason
`wayland_not_implemented`. Desktop acceptance evidence is recorded
separately in `docs/verification.md` when it exists; nothing here claims it.

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
| `recording.prepare` | `target {kind: monitor\|desktop\|region\|window, identity?, rect?}`, `format mp4\|webm\|gif\|webp`, `codec?`, `fps? (1–60)`, `audio? {system?, microphone?}`, `maxEdge?` | durable intent, then ready bounds/controls and any OS consent; replies at once with `state: preparing`; poll `inspect` for `ready`. A region without `rect` asks the user to drag it (Escape cancels); the chosen bounds appear in `inspect` as `target.rect`, and `overlayExcluded` says whether the control bar stayed outside them |
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
  `preview` and `export`. Deadlines: 15 s per command, 30 s for `stop`, 600 s for
  `prepare` (a hands-on drag selection or OS consent), 600 s for `export`; a missed deadline is
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

## Overlay (X11)

`prepare` for a region without a rect shows a translucent drag surface on every
screen ("Drag to choose the area to record. Esc cancels."); release keeps the
bounds and nothing records yet. The ready state draws a 3 px white ring with a
dark hairline just outside the recorded rectangle (never over its pixels) and
places the control bar below it, above it, or, when neither fits, inside its
bottom-right corner (`overlayExcluded: false`, also for monitor and desktop
targets). While recording, four translucent strips dim everything outside the
rectangle; ring and strips are transparent for input, bypass the window
manager and stay on top, so the desktop inside and outside stays usable and
the capture never sees them.

The control bar shows a state dot (green ready, red recording, amber paused),
a green Record button before capture and a red Stop button during it,
the retained time, the target, a system-audio checkbox and a format/codec
choice (editable only before Record; GIF/WebP disable audio), and Record,
Pause/Resume, Stop and Cancel. Its actions run in the helper and are reported
to the resident as events (`started`, `paused`, `resumed`, `stopped` with
reason `user`, `cancelled`, `configChanged`), which the service applies to the
same state machine the CLI drives; `recording.overlay` hides or shows the bar
and `recording.raise` brings it forward. Stop and Cancel from the bar are
mouse actions; the recording global shortcut is the keyboard stop route (see
"Tray and shortcut").

### Tray and shortcut

`action.invoke {action: "record", mode?}` is the entry point behind the tray,
the dashboard rail and the global shortcut (`actions.record.shortcut`, unbound
by default; recorded in Settings → Shortcuts like the others). With no active
recording it runs `recording.prepare` for a dragged region with the default
configuration (MP4, no audio; the bar changes both before Record). With one
active, mode `open` (the tray and the rail) raises the bar or review window,
and mode `toggle` (the shortcut) starts a `ready` session, stops a `recording`
or `paused` one, and otherwise raises. The reply carries the step taken
(`prepare`, `start`, `stop`, `raise`) beside the command's own result. The MCP
`perform_action` tool accepts `record` and the CLI passes `mode` through. An
open image edit refuses the action with `editor_active`, as a recording refuses
edits.

## Review window

Every Stop (from the bar, the CLI, or the byte budget) opens the helper's
private review window: the frame at the scrubber position with the masks
active at that time drawn over it, "Trim start here" / "Trim end here" /
"Reset trim" and the retained interval, a mask list with "Add blur" / "Add
solid cover" (then drag on the frame), "Remove", "Mask starts here" / "Mask
ends here" and the blur strength, an output format choice (GIF/WebP drop
audio; the capture rate cannot change), and Save copy… / Discard / Upload.
Previews are rendered in the helper by the same `previewFrame` the CLI and
MCP preview use, so timing and placement agree with the export. While a mask
is being dragged the pending area is drawn as a dashed rubber band. Play /
Pause (Space) plays the retained interval silently from the scrubber at the
chosen speed (0.5×, 1×, 2×, 4×): frames are decoded in order on a worker by
`FrameSequence`, masks applied as the export renders them, and shown at their
clip time; frames that fall behind the clock are decoded but not drawn, so a
large clip keeps pace with dropped frames. Scrubbing or any edit control
pauses playback; the audio track is not previewed (the export keeps it).

The window never owns the edit: each change is proposed to the resident as an
`edit {revision, commands}` event, the resident applies it to the authoritative
model (or refuses a stale or invalid proposal) and pushes the result back with
the `setEdit` command, which is also sent after every CLI/MCP edit. "Save
copy…" and "Upload" send `saveCopyRequested {path, revision}` and
`uploadRequested {revision}`; the resident runs exactly the `saveCopy` and
`upload` paths, so a stale revision or an animation over budget is refused
and the window shows the resident's current edit again. Discard, closing the
window and Escape during selection end the operation as `cancelled`. Upload
closes the window at once and the export and upload continue in the
background; the operation reports like any other upload (tray, history,
`operation.inspect`), and the resident shuts the helper down when the export
has been handed over. Save copy keeps the window open with the controls
disabled and the status showing progress, so the user can still upload
afterwards.

### Tray feedback

The tray follows the operation's observations. Through capture, pause,
review and Save copy the icon keeps its idle colour and only the tooltip
changes ("Ready to record", "Recording", "Recording paused", "Reviewing the
recording", "Saving a copy of the recording"). Upload is the hand-over: the
helper's export then reports the `encoding` phase and the icon turns amber
(`#f3c66f`, solid) for "Encoding the recording" and stays amber while the
clip is stored and the local copy saved. Once storage traffic starts the
ordinary upload feedback takes over: dark green filling with the transferred
fraction, bright green with "Upload complete", then idle. Failures flash red as
for any operation. `tests/gui/test_tray.cpp` walks this chain with the fake
backend.

### Helper events

| Event | Meaning |
| --- | --- |
| `progress {durationUs, bytes, paused, encoder, audioEncoder}` / `{exportFraction}` | retained time and file size while recording; export progress |
| `started`, `paused`, `resumed` | the bar's Record / Pause / Resume |
| `stopped {durationUs, canvas, reason: user\|budget}` | the clip is finalized and under review |
| `cancelled` | Cancel on the bar, Discard in the review, the window closed |
| `configChanged {config}` | format/codec/audio changed on the bar (ready) or the format changed in review |
| `edit {revision, commands}` | a review-window proposal at the revision it displayed |
| `uploadRequested {revision}`, `saveCopyRequested {path, revision}` | the review's Upload / Save copy |
| `failed {error}` | the pipeline or helper failed; the operation ends `failed` without an upload |

## Capture and export engine (X11)

`putgd-record` resolves the target against the current RandR layout
(`X11Targets`): monitors by output name, the desktop as the root window, a
region as root coordinates inside the root, a window by decimal or `0x` XID
(captured through `ximagesrc xid=`, so it keeps its identity when moved).
Dimensions shrink to even values rather than growing into unselected pixels.

The live pipeline is `ximagesrc → videoconvert → H.264 encoder → h264parse →
matroskamux` with `pulsesrc device=@DEFAULT_MONITOR@ → AAC` when system audio
is requested. Encoders are tried in order `vah264lpenc, vah264enc,
vaapih264enc, x264enc, openh264enc` (AAC: `avenc_aac, fdkaacenc, voaacenc`)
by actually starting the pipeline, GPU first, and the chosen names are
reported in `recording.inspect` progress. The VA encoders take NV12, the CPU
encoders I420. Bitrate for the bitrate-driven encoders is about 0.1 bit per
pixel per frame, bounded to 1.5–20 Mbit/s; the legacy `vaapih264enc` runs
at constant QP 24 because Intel's iHD driver refuses CBR and VBR for it
(that refusal, surfacing as "Internal data stream error", was why it used
to fail on Mint 22.3 and Ubuntu 26.04); on the workstation it captures 720p
at half the CPU of `x264enc veryfast`. Pause drops buffers at the encoder queues and folds
the gap into a timestamp offset, so the working clip's timeline excludes
paused time for video and audio alike; the retained duration is measured from
the first retained frame because some encoders carry a constant timestamp
offset. The helper stops on its own and reports `stopped` with reason
`budget` when the working clip nears `limits.maxFileBytes` minus a reserve.
Progress events carry `durationUs`, `bytes`, `paused` and the encoder names.

Export runs in the helper with the FFmpeg libraries on the working clip:
`trim` in stream ticks, `setpts` rebase, RGB conversion, per-mask
`crop→boxblur→overlay` or `drawbox` enabled on `gte(t,a)*lt(t,b)` in output
time, optional `scale` to `maxEdge`, `fps` (at most 15 for animations), then
the first encoder that opens from a preference list: MP4/H.264 tries
`h264_nvenc` (NVIDIA GPU, system-memory frames), then `h264_vaapi` (Intel
and AMD GPUs through libva), then `libx264` (`veryfast`, CRF 23) and
`libopenh264`; H.265 tries `hevc_nvenc`, `hevc_vaapi`, then `libx265`
(`faster`, CRF 26); WebM uses `libvpx-vp9` (`realtime` deadline, `cpu-used`
5, row threads); GIF is a two-pass `palettegen`/`paletteuse`; WebP is
`libwebp_anim`. NVENC is opened with `p4`/`hq`, VBR and a constant quality
target (26 for H.264, 28 for HEVC). VA-API opens a device on the default
render node, then `/dev/dri/renderD128`… in turn (the display GPU may be an
NVIDIA card without a VA driver), allocates an NV12 surface pool, uploads
each filtered frame with `av_hwframe_transfer_data` and encodes with constant
QP (24 for H.264, 26 for HEVC), the one rate control every VA driver offers;
AMD's `radeonsi` and Intel's `iHD` share this path, with no B-frames. An
encoder that does not open (no driver, no entry point, as HEVC on a UHD 630)
is skipped for the next candidate, so other machines keep the CPU path. HEVC
in MP4 is tagged `hvc1` for Apple players. The working clip is decoded with frame and
slice threads on every core. Audio is trimmed with `atrim` in seconds and
encoded as AAC (MP4) or Opus (WebM). MP4 output is written with `faststart`.

On the workstation (i9-9880H, UHD 630, Quadro T2000, 60 s 2238×1318 clip
with audio, `putgd-record --export`): H.264 and H.265 on NVENC export at
about 3.9× real time (15.6 s), H.264 on the Intel iGPU through VA-API at
2.6× (23.5 s), `libx264` at 2.1×, VP9 at 1.7×, a 10 s GIF at 0.9×; `libx265`
stays below real time (0.68× at `faster`), which is why the GPU paths come
first. AMD hardware was not available; its VA-API export is the same libva
code path exercised on Intel here and on the Skylake builders. H.265 links do not play in Firefox or Chrome on Linux; both format
menus say so in the item tooltip and the review status, and the default
stays H.264. Each decode pass
opens its own demuxer and verifies that a backward seek did not overshoot the
trim start (Matroska clips whose first cluster has no cue point otherwise
start at the next keyframe). Preview decodes the frame at or after the
requested time and applies the masks active at that source time with a box
blur or a solid fill, so preview and export agree on timing and placement.

Lab diagnostics: `PUTGD_RECORD_DEBUG_LOG=<file>` appends pipeline and command
traces, `PUTGD_RECORD_KEEP_WORKING=<dir>` keeps a copy of each working clip,
`putgd-record --export CLIP OUT.ext [--codec h264|h265|vp9] [--max-edge N]
[--trim startUs endUs] [--no-audio]` exports a kept clip the way the review
does and prints the encoder, frame count, bytes and wall time, and
`PUTGD_RECORD_EXPORT_ENCODERS=<comma list>` replaces the encoder preference
order for such timing runs. None is read by the resident and none carries
frames.

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
