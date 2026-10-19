# Screen recording

Status: **slices 1 to 7 of the [recording plan](plans/2026-10-17-2230-feat-screen-recording-plan.md)**,
with the open items listed in its slice status. Recording ships in the DEB,
RPM and AppImage; see "Packages and codecs".
The contract below is implemented and tested against a fake backend and the
real helper. On **X11 sessions** the `putgd-record` helper records monitors, the
desktop, regions and windows through `ximagesrc`, pauses and resumes on one
clip timeline, previews frames with masks, and exports MP4 (H.264, H.265), WebM
(VP9), GIF and animated WebP with trim and timed masks; the agent path (CLI
and MCP) is complete, and the drag selection, ready bounds, outside dimming
and floating control bar are in place, and every Stop opens the private review
window. The tray's **Capture Screen Recording** entry, the dashboard's **Record
screen** button and the `record` global shortcut start one (see "Tray and
shortcut" below). On Wayland the helper takes a PipeWire stream from the
compositor without a dialog where it can (KWin's screencast protocol on
Plasma, Mutter's screen cast service on GNOME) and otherwise through the
ScreenCast portal, whose picker is shown once per target kind thanks to
restore tokens; see "Wayland acquisition". A region without a rectangle is
chosen first with the desktop's own screenshot selector (KDE's layer-shell
selector, the GNOME Shell extension's), and the ready ring, the dimming and
the control bar stay outside the recorded area; see "Wayland selection, bounds
and bar". Desktop acceptance evidence is recorded
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
| `recording.prepare` | `target {kind: monitor\|desktop\|region\|window, identity?, rect?}`, `format mp4\|webm\|gif\|webp`, `codec?`, `fps? (1–60)`, `audio? {system?, microphone?}`, `maxEdge?` | durable intent, then ready bounds/controls and any OS consent; replies at once with `state: preparing`; poll `inspect` for `ready`. A region without `rect` opens the desktop's selector (Escape cancels; `inspect` shows `selecting: true`); the chosen bounds appear in `inspect` as `target.rect`, and `overlayExcluded` / `overlayPlacement` say whether the control bar stayed out of the recorded pixels |
| `recording.select` | `{id, rect: [x, y, w, h]}` | completes an open region selector with that rectangle as a drag would (X11 root pixels, logical desktop pixels on Wayland); `recording_state` when no selector is open |
| `recording.start` / `pause` / `resume` / `stop` | `{id}` | acknowledged once the backend reached the state; a duplicate of the in-flight command returns the current state, a different one is `recording_busy`; `stop` returns `durationUs` and enters `reviewing` |
| `recording.cancel` | `{id}` | discards the clip; the operation ends `cancelled` |
| `recording.inspect` | `{id}` | state, pending command, `selecting`, target, config, canvas, `durationUs`, `retainedUs`, edit, progress (`exportFraction` while exporting), `overlayExcluded`, `overlayPlacement`, `lastExport`, error; an ended session is `recording_unavailable` and its operation holds the outcome |
| `recording.raise` / `recording.overlay` | `{id}` / `{id, visible}` | overlay control; the recording indicator stays visible |
| `recording.edit` | `{id, revision, commands[1–100]}` | atomic `setTrim {start, end\|null}`, `resetTrim`, `addMask {mask}`, `updateMask {maskId, mask}`, `removeMask {maskId}` |
| `recording.preview` | `{id, time, maxEdge? 64–1600}` | one PNG frame (base64 `png`) at clip time with current trim/masks, plus `retained`, `outputTimeUs`, `revision`; returned only to the requesting client |
| `recording.saveCopy` | `{id, path, overwrite?}` | exports to an absolute path with the format's extension; no upload. Replies once the export starts with `state: exporting`; poll `inspect` until `reviewing`, then `lastExport.status` is `saved` or `failed` |
| `recording.upload` | `{id, revision}` | the only publishing action. Replies once the export starts with `state: exporting` and the linked `operationId`; see "Exports" below |

### Exports

An export can run for minutes: the helper allows 600 s, and a 60 s GIF or a
long H.265 clip on a busy machine takes well over the CLI's 15 s wait. So
`saveCopy` and `upload` check the request first (state, path and `overwrite`,
the revision, the animation limits) and refuse it at once, or reply as soon as
the export starts, like `prepare`. The reply is the session as `inspect` shows
it: `state: exporting`, `pending: saveCopy|upload`. Edits, previews and other
exports are refused with `recording_state` until the export ends; `cancel` still
works and discards it. While it runs, `progress.exportFraction` reports how far
it got, and `operation.inspect` reports phase `exporting` (copy) or `encoding`
(upload).

`lastExport` is the session's latest export: `{command, status, revision}`, plus
`path` for a copy. `status` is `exporting`, then `saved` (with `bytes` and
`durationUs`) or `failed` (with `error`, for example `recording_export_failed`,
`input_excessive` or `local_save_failed`). A failed export writes nothing and
returns the session to `reviewing`. For a copy, poll `recording.inspect` until
`state` is `reviewing` again. For an upload, `reviewing` with
`lastExport.status: failed` means nothing was uploaded. A successful upload export
hands the bytes to the pipeline and ends the session: `recording.inspect` then
answers `recording_unavailable`, and `operation.inspect` on the `operationId`
follows the upload to `completed` or `failed`.

Codec rules: MP4 takes `h264` (default) or `h265`; WebM takes `vp9`; GIF and
WebP take none. `audio.system` is the computer's playback and is refused for
GIF/WebP (`invalid_request`); `audio.microphone` is reserved and refused
(`recording_unsupported`). Errors: `recording_unavailable`,
`recording_unsupported`, `recording_active`, `recording_state`,
`recording_busy`, `recording_stale`, `recording_mask_unknown`,
`region_unavailable` (a Wayland desktop without a region selector),
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
  `preview` and `export`. Deadlines (`recording::helper*Ms` in
  `src/app/RecordingBackend.h`): 15 s per command, 30 s for `stop`, 600 s for
  `prepare` (a hands-on drag selection or OS consent), 600 s for `export`; a missed deadline is
  `recording_helper_timeout` and leaves the session where it was. The session
  methods that wait on a command reply only after the helper answers, so
  `putgd-cli` waits 15 s past the helper's deadline for them (45 s for `stop`,
  30 s for the others; `LocalServer::replyDeadlineMs`). `prepare` and the exports
  reply when they start.
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
honest `available`/`reason` pair. Registry presence is not proof (stock
Fedora's FFmpeg lists `h264_vaapi` while its free Intel driver has no H.264
encode profile, and its `openh264enc` loads a placeholder library), so the
probe tries the H.264 path for real; see "Packages and codecs". The live
pipeline still starts encoders in order and reports the one actually used.
CMake builds the helper only when `gstreamer-1.0 >= 1.24` and FFmpeg
libraries `>= 6.1` are found (`PUTGD_RECORDING`, default on, also for the
AppImage, which builds it against its own pinned FFmpeg); a resident without
the sibling binary reports `recording_helper_missing`.

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
afterwards. A refused export after Upload (a stale revision, or an animation
over its limits) reopens the window with the resident's reason, so the
session never sits silently in `reviewing`. `operation.cancel` on the
recording's operation (the Activity window's Cancel) ends the session the
same way `recording.cancel` does, discarding the working clip.

Choosing GIF or WebP is a format choice, not a pixel count: when the output
would exceed `limits.recordingAnimationEdge`, the resident sets `maxEdge` to
the limit at prepare and whenever the format changes, and pushes the result
to the review. The duration limit still needs a trim; the review's status
line says so ("GIF is limited to 60 s: trim the clip") as soon as the
retained interval exceeds it, using the limits the resident sends with every
`setEdit`.

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
| `progress {durationUs, bytes, paused, encoder, audioEncoder}` / `{exportFraction}` | retained time and file size while recording (once a second); export progress (at most ten a second, so a fast GPU export stays inside the control channel's frame rate) |
| `selecting {selector: x11\|kde-layer-shell}` | the helper's region selector is open; `recording.select` can complete it |
| `started`, `paused`, `resumed` | the bar's Record / Pause / Resume |
| `stopped {durationUs, canvas, reason: user\|budget}` | the clip is finalized and under review |
| `cancelled` | Cancel on the bar, Discard in the review, the window closed |
| `configChanged {config}` | format/codec/audio changed on the bar (ready) or the format changed in review |
| `edit {revision, commands}` | a review-window proposal at the revision it displayed |
| `uploadRequested {revision}`, `saveCopyRequested {path, revision}` | the review's Upload / Save copy |
| `failed {error}` | the pipeline or helper failed; the operation ends `failed` without an upload |

## Wayland acquisition

On a Wayland session the compositor provides the pixels as a PipeWire video
stream and `pipewiresrc` feeds them through `videorate` at the configured
rate into the same encoder, pause and budget logic as X11. The probe picks
one of three ways to obtain that stream and reports it as `acquisition`:

- `kde-screencast`: KWin's own `zkde_screencast_unstable_v1` protocol
  (version 3 or later) on a Plasma session. `kdeScreencast {available,
  version, outputs:[{name, logical}]}` carries the facts. A monitor or
  `desktop` target streams the named output (the first output when the
  identity is empty) and a `region` with a rectangle streams exactly that
  logical rectangle (`stream_region`), so no crop is needed. KWin only
  advertises the protocol to executables whose application desktop entry
  lists it in `X-KDE-Wayland-Interfaces`; packaging installs
  `putgd-record.desktop` with `Exec=<bindir>/putgd-record` for that, the
  same mechanism `docs/desktops/kde.md` describes for ScreenShot2. Without
  the entry the facts say `kde_screencast_unavailable` and the portal is used.
- `mutter-screencast`: the `org.gnome.Mutter.ScreenCast` service on a GNOME
  session (`mutterScreenCast {available, service}`). A monitor or `desktop`
  target calls `RecordMonitor` with the Qt screen's connector name (the
  primary screen when the identity is empty), a `region` with a rectangle
  calls `RecordArea`; `PipeWireStreamAdded` delivers the node. The service
  answers session clients on the GNOME desktops tested; where it refuses,
  the error is reported and the portal remains the way to share.
- `portal`: `org.freedesktop.portal.ScreenCast` (version 2 or later) on the
  session bus, used for `window` targets everywhere and for everything when
  neither native path is available. `portal {portalVersion, sourceTypes,
  cursorModes}` carries the portal's facts and `targets` lists `window` only
  when the portal offers it. `prepare` opens a portal session
  (`CreateSession`, `SelectSources`, `Start`) and the compositor shows its
  picker, which is why the `prepare` deadline is long. With portal version 4
  or later the share is requested with `persist_mode 2` and the returned
  restore token is kept in `<XDG_DATA_HOME>/putgd/putgd-record/recording-portal-tokens.json`
  keyed by desktop and target kind, so the next recording of that kind
  restores the share without a picker until the compositor invalidates the
  token (a refused token simply shows the picker again and stores the new
  one). A `region` records the shared monitor cropped to its rectangle
  (`videocrop`, stream-relative), which is chosen before the portal session
  opens (see below).

Both native paths name a node on the session's PipeWire daemon, so the probe
also requires its socket (`pipewire {socket}`; `PIPEWIRE_REMOTE`,
`PIPEWIRE_RUNTIME_DIR` and `XDG_RUNTIME_DIR` are honoured) and leaves the
portal, which passes its own connection as a descriptor, in charge when it
is missing. Monitor and window identities are optional everywhere (on X11 an
empty monitor identity means the first connected monitor). Reasons for an
unavailable Wayland session are `live_source_missing` (no `pipewiresrc`),
`portal_unavailable`, `portal_too_old` or `session_bus_unavailable`.
Cancelling a picker ends the operation as `cancelled`; Cancel, Stop and
discard close the stream (`Session.Close`, `Session.Stop` or the protocol's
`close`), which ends the share. `tests/unit` covers the portal stream
demarshalling; the three paths are exercised on the Fedora GNOME and Debian
KDE desktops.

### Wayland selection, bounds and bar

A `region` without a rectangle (the tray, the dashboard's Record screen and
the shortcut all start one) is chosen **before** any compositor stream or
portal session exists, with the selector screenshots already use, so Escape
or a right click leaves nothing behind: the operation ends `cancelled` and no
share was ever opened.

- **KDE Plasma**: the helper itself shows the layer-shell screenshot selector
  (`KdeRegionOverlay`, the same class `putgd-kde-region` runs), which reports
  a logical rectangle only after KWin has rendered its surfaces transparent
  and unmapped them; then `stream_region` (or the portal plus `videocrop`)
  starts for exactly that rectangle. The helper sends `selecting` while it is
  open.
- **GNOME**: clients cannot place surfaces, so the resident asks the put.gd
  Shell extension (recording protocol 1, `SelectArea`) for the rectangle with
  its St selector, then launches the helper with it; `RecordArea` streams
  that area. Without the extension (or with an older one) a rectless region
  is refused with `region_unavailable` instead of recording the whole monitor.
- Elsewhere on Wayland a region needs `target.rect`.

`recording.select {id, rect}` (MCP `select_recording_region`) completes an
open selector of either kind as a drag would; `recording.cancel` closes it.

The **ready ring** (3 px white, dark hairline, just outside the rectangle),
the **dimming** of everything else while recording or paused and the
**control bar** follow the X11 rules where the compositor allows them:

- KDE: the ring and the dim strips are input-transparent overlay layer
  surfaces (split per output), and the bar is an overlay layer surface placed
  below the rectangle, else above it, else at the top of another output, else
  inside its bottom-right corner. A rectangle's stream contains none of them
  unless the bar had to sit inside. KWin 6.6 and later also leave every
  window of the client that requested a `zkde_screencast` stream out of that
  stream, so on those versions the bar is excluded even from monitor
  recordings (`kdeScreencast.hidesRequester`, from the KWin version on
  D-Bus); KWin 6.3 does not, and the portal path never does.
- GNOME: once the helper reports `overlayPlacement: compositor`, the resident
  calls the extension's `ShowRecordingFrame {rect, kind, state, pid}`: the
  Shell draws the ring and, while recording or paused, dim strips with a hole
  for the bar as non-reactive actors outside the area, and moves the helper's
  bar window (title `put.gd recording controls`, accepted only when its
  process is a direct child of the authenticated resident) below or above
  the area, onto another monitor, or, failing all, inside the corner.
  `HideRecordingFrame` removes everything when the clip is under review or
  the session ends; disabling the extension, locking or the resident leaving
  the bus does too. Mutter has no per-window screencast exclusion, so a whole
  single-monitor recording on GNOME contains the bar.
- A `window` stream (portal) carries only that window: `overlayExcluded` is
  true and no ring is drawn.

`overlayPlacement` reports where the bar went: `outside`, `inside` (it is in
the clip), `hidden` (KWin 6.6 leaves it out of its own stream) or
`compositor` (nobody could place it; it may be in the clip); `overlayExcluded`
is true only for `outside` and `hidden`.

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
at half the CPU of `x264enc veryfast`. Pause drops raw frames and samples on the
encoders' sink pads (never encoded buffers, which would leave P-frames with
missing references that strict decoders such as OpenH264 refuse) and folds
the gap into a timestamp offset, so the working clip's timeline excludes
paused time for video and audio alike; the retained duration is measured from
the first retained frame because some encoders carry a constant timestamp
offset. The helper stops on its own and reports `stopped` with reason
`budget` when the working clip nears `limits.maxFileBytes` minus a reserve.
Progress events carry `durationUs`, `bytes`, `paused` and the encoder names.

Export runs in the helper with the FFmpeg libraries on the working clip:
`trim` in stream ticks, `setpts` rebase, RGB conversion, per-mask
`crop→avgblur→avgblur→overlay` (two box passes, as the preview draws them;
`boxblur` is GPL-only in FFmpeg) or `drawbox` enabled on `gte(t,a)*lt(t,b)` in output
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
order for such timing runs; `putgd-record --motion CLIP [SAMPLES]` prints the
mean frame difference between frames sampled across a clip (about 0.01 for a
still picture), so a recording test can require that it captured movement.
For system audio, `putgd-record --av-marker OUT.mkv SECONDS` writes a
synthetic marker clip (MJPEG and PCM in Matroska, built-in FFmpeg encoders
only): black 1280×720 frames with a white flash and a 1 kHz tone burst, both
100 ms, at the start of every second. Play it in the recorded area with
system audio on, then `putgd-record --av-sync CLIP` decodes the recording in
one demux pass, finds the flash onsets (mean luma rising edges) and burst
onsets (2 ms RMS envelope), pairs each flash with the nearest burst within
half a period and prints JSON with every offset (`offsetsMs`, audio minus
video, positive when the sound is late), `medianMs` and `spreadMs`; the
marker itself measures 0 ms. None is read by the resident and none carries
frames.

The AppImage's own helper runs all of these on its working clips and on saved
MP4 (H.264), WebM and GIF copies: its FFmpeg adds the `mov` and `gif`
demuxers, the Opus, GIF, MJPEG and PCM decoders and libvpx's VP9 decoder
(libvpx is already bundled for VP9 export), and the MJPEG and PCM encoders for
`--av-marker`. Extract the image and run it headless with `APPDIR` set, for
example `./putgd-*.AppImage --appimage-extract`, then
`APPDIR=$PWD/squashfs-root squashfs-root/usr/bin/putgd-record --motion COPY.mp4 8`.
It can't read H.265 MP4 copies (FFmpeg's HEVC decoder would add 0.34 MiB to
the image) or WebP ones (FFmpeg 7.1 has no animated WebP decoder): score those
with a distro build of `putgd-record`, and WebP motion with Pillow. Each CI
AppImage build runs `tools/releases/appimage_diagnostics.py` on the extracted
image: the helper writes a marker clip, exports it to MP4, WebM and GIF, and
`--motion`, `--preview` and, for the clips with sound, `--av-sync` must read
every copy.

## Packages and codecs

Recording keeps an H.264 working clip, so capture needs a working H.264
encoder and every preview and export needs FFmpeg's H.264 decoder.
`recording.capabilities` tests both instead of trusting the registry: each
registered live encoder encodes two synthetic 320×240 frames through
`h264parse`, each export encoder opens on a 320×240 frame (VA-API ones on a VA
device and frame pool, as the export does; NVENC only by its driver libraries
being loadable, because opening it initialises CUDA and wakes a sleeping GPU),
and the H.264 decoder opens. The results are `gstreamer.usableH264Encoders`
with `gstreamer.encoderFailures`, `exportEncoders` (openable ones only, which
also decide `formats` and `codecs`) with `ffmpeg.encoderFailures`, and
`ffmpeg.h264Decoder {name, usable, failure}`. Without a usable encoder or
decoder the reason is `live_encoder_missing` (none registered),
`h264_encoder_unusable` (installed, none works) or `h264_decoder_unusable`,
and the probe adds `message` and a distribution-specific `hint`.
`recording.prepare` then refuses with `recording_unavailable`, whose message
is the two together and whose details carry `reason` and `hint`; the dashboard
shows that message in a warning dialog and the tray as its failure notification. The checks add
a few tens of milliseconds to a probe; a fresh GStreamer registry costs more
than all of them.

| Package | Live H.264, in preference order | Export H.264 | Setup |
| --- | --- | --- | --- |
| DEB (Ubuntu 24.04 / Mint 22.3, Ubuntu 26.04 / Kubuntu, Debian 13) | `vah264lpenc`/`vah264enc` (plugins-bad, or plugins-extra on 26.04), `vaapih264enc` (Recommends `gstreamer1.0-vaapi`), `x264enc` (Recommends `gstreamer1.0-plugins-ugly`), `openh264enc` (plugins-bad, a Depends) | `h264_nvenc`, `h264_vaapi`, `libx264`, `libopenh264` | none: plugins-bad always brings `openh264enc` and `voaacenc` |
| RPM (Fedora 44) | `vah264*enc` (stock Intel and AMD drivers have no H.264 encode; RPM Fusion's do), `openh264enc` (Recommends `gstreamer1-plugin-openh264`) | `h264_nvenc`, `h264_vaapi`, `libopenh264` | Fedora Workstation ships `libopenh264` as the `noopenh264` placeholder until Cisco's `openh264` replaces it: `sudo dnf install openh264 gstreamer1-plugin-openh264`. The RPM recommends both, so a default `dnf install` brings them when the `fedora-cisco-openh264` repository is on (Fedora's default); if it was turned off, run `sudo dnf config-manager setopt fedora-cisco-openh264.enabled=1` first |
| AppImage | `vah264lpenc`/`vah264enc`, `vaapih264enc` through the host's VA-API driver, then the bundled `x264enc` | `h264_nvenc`, `h264_vaapi` through the host's driver, then the bundled `libx264`; H.265 `hevc_nvenc`, `hevc_vaapi`, then the bundled `libx265` | none: the bundled x264/x265 cover hosts whose driver cannot encode |

On Fedora with the placeholder the probe reports `h264_encoder_unusable`
(`openh264enc`: "Could not initialize supporting library"), FFmpeg's
`libopenh264` decoder and encoder fail to open, and the hint names the
command above. Debian 13 has its own release DEB
(`putgd-VERSION-debian13-x86_64.deb`), built against Debian's FFmpeg 7.1: the
Ubuntu DEBs link FFmpeg 6.1 (24.04) or 8.0 (26.04), so neither installs there
now that the helper is packaged.

### AppImage runtime

The AppImage builds `putgd-record` against a pinned upstream FFmpeg
(`packaging/appimage/tools.lock.json`) configured with `--enable-gpl` and
exactly the demuxer, decoders, encoders, muxers and filters the export uses
(`packaging/appimage/media.json`): the native H.264 and AAC decoders,
`h264_nvenc`, `h264_vaapi`, `libx264`, `hevc_nvenc`, `hevc_vaapi`, `libx265`,
`libvpx-vp9`, `gif`, `libwebp_anim`, `aac` and `libopus`. The encoder order
puts the machine first: the host's GPU encoders (NVENC, VA-API) are tried by
actually opening or starting them, and the bundled x264 (live and MP4) and
x265 (H.265 export) are used only when none of them works; the encoder used
is reported as for the other packages. Live H.265 does not exist, so only
FFmpeg's `libx265` is bundled, not GStreamer's `x265enc`. With x264, x265 and a
GPL FFmpeg inside, the image is a GPL-2.0-or-later combined work:
`usr/share/doc/putgd/APPIMAGE-GPL-NOTICE.txt` says so and names the bundled
versions, the licence texts are under `usr/share/doc/putgd/bundled-licenses`,
and every release publishes the corresponding source as
`putgd-VERSION-appimage-media-source.tar.gz` (see [releases](release.md)).
GStreamer comes from the Ubuntu 24.04 builder: the core libraries, the plugins
the live pipeline uses (`coreelements`, `videotestsrc` for the probe,
`videoconvertscale`, `videorate`, `videocrop`, `audioconvert`, `audioresample`,
`ximagesrc`, `pipewire`, `pulseaudio`, `matroska`, `audioparsers`,
`videoparsersbad`, `va`, `vaapi`, `x264`, `voaacenc`) and `gst-plugin-scanner`
under `usr/libexec/gstreamer-1.0/`. VA-API (`libva*`), PipeWire's client
library, the NVIDIA libraries and any OpenH264 stay the host's: libva must
match the host's VA drivers and libpipewire its daemon and modules. On a host
without libva, AppRun adds the bundled copy in `usr/lib/putgd-host-fallback/`
so the helper still starts. (A host OpenH264 cannot be used: the image's
GStreamer and FFmpeg were built against Ubuntu 24.04's `libopenh264.so.7`,
while Fedora 44 ships `.so.8`.)

When `putgd-record` runs from the mounted image (`APPDIR` contains the
executable), it confines its own GStreamer before initialising it
(`src/record/BundledRuntime.cpp`): `GST_PLUGIN_SYSTEM_PATH` is the image's
plugin directory, `GST_PLUGIN_PATH` is emptied, `GST_PLUGIN_SCANNER` is the
image's scanner and `GST_REGISTRY` is
`$XDG_CACHE_HOME/putgd/gstreamer-1.0-appimage-registry.bin`, rebuilt once per
mount. Only the helper sets these, so host programs started by put.gd (the
browser behind a notification) keep the host's GStreamer. On Plasma, AppRun
writes a second temporary private entry next to the ScreenShot2 one,
`putgd-record-appimage-XXXXXXXX.desktop` with `Exec` naming the mounted
`putgd-record` and `X-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1`,
so KWin offers its screencast protocol to the helper; both entries are removed
on exit. The assembly audit runs the relocated helper's probe and fails unless
every required element loads from the image's plugin directory only, the
H.264 decoder opens, all four formats export, the bundled `x264enc`,
`libx264` and `libx265` work (the builder has no GPU, so they are what must)
and no host-matched library was bundled; the receipt records the result under
`postDeploymentAudit.recording` and the bundled GPL packages under
`recordingRuntime.gplComponents`.

## Agent and MCP control

| Tool | Session method |
| --- | --- |
| `get_recording_capabilities` | `recording.capabilities` |
| `prepare_recording` | `recording.prepare` |
| `control_recording` (start/pause/resume/stop/cancel/raise) | `recording.<command>` |
| `get_recording` | `recording.inspect` |
| `select_recording_region` | `recording.select` |
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
  restart interruption, a fake desktop shell selecting a region and framing
  the bar (`overlayPlacement`), cancelling and driving a shell selection
  without ever starting the helper, and the helper's `selecting` event with
  `recording.select`. Its slow-export case holds the fake export open while the
  production `putgd-cli` runs `saveCopy` and `upload`: both answer `exporting`
  at once, `inspect` shows progress, the lock on edits and `lastExport`, and a
  cancel during the export admits nothing. It also checks that the CLI's wait
  for every method that waits on the helper outlasts that helper's deadline.
- `recording_helper_integration` (`tests/integration/test_recording_helper.cpp`):
  the helper supervisor against a fixture helper (probe caching, lifecycle,
  progress and helper-initiated stop events, crash, silence, wrong build,
  export timeout, broken probes, missing helper) and, when built, the product
  `putgd-record`: its probe JSON, the identity handshake, the explicit
  `capture_not_implemented` refusal and its normal exit.
- `tests/integration/test_mcp.py`: tool → method forwarding parity, the
  preview image route against the synthetic peer, a `recording.stop` that the
  peer answers after 16 s still reaching the caller through the production CLI,
  and the adapter's default deadline staying above the CLI's longest wait.
- `gnome_ui_ownership_contract` (`tests/desktop/test_gnome_ownership.mjs`,
  GNOME hosts only): `SelectArea` answering and `Release` cancelling, bar
  placement order, dim strips cut around the area and the bar, and refusing
  a bar whose process is not the resident's child.
- `tests/integration/test_config_schema.py`: optional `record` action,
  template and limits.
