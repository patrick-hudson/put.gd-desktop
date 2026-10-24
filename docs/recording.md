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
| `recording.prepare` | `target {kind: monitor\|desktop\|region\|window, identity?, rect?}`, `format mp4\|webm\|gif\|webp`, `codec?`, `fps? (1–60)`, `audio? {system?, microphone?}`, `maxEdge?` | durable intent, then ready bounds/controls and any OS consent; replies at once with `state: preparing`; poll `inspect` for `ready`. A region without `rect` opens the desktop's selector (Escape cancels; `inspect` shows `selecting: true`); the chosen bounds appear in `inspect` as `target.rect`, and `overlayExcluded` / `overlayPlacement` say whether the control bar stayed out of the recorded pixels. Refused with `disk_space_insufficient` (details `freeBytes`, `reserveBytes`, `availableBytes`, `requiredBytes`, `shortfallBytes`, `hint`) before anything is accepted when free disk above the reserve can't hold three 64 MiB budgets (#213) |
| `recording.select` | `{id, rect: [x, y, w, h]}` | completes an open region selector with that rectangle as a drag would (X11 root pixels, logical desktop pixels on Wayland); `recording_state` when no selector is open |
| `recording.start` / `pause` / `resume` / `stop` | `{id}` | acknowledged once the backend reached the state; a duplicate of the in-flight command returns the current state, a different one is `recording_busy`; an acknowledged user `stop` returns `durationUs` and `stopReason: user` and enters `reviewing`, including when stopped through CLI, MCP or the toggle shortcut. A `stop` that arrives after the helper stopped on its own (its budget) answers with the existing `reviewing` state and preserves `stopReason: budget` and `stopDetails`, like a duplicate stop |
| `recording.cancel` | `{id}` | discards the clip; the operation ends `cancelled` |
| `recording.inspect` | `{id}` | state, pending command, `selecting`, target, config, canvas, `durationUs`, `retainedUs`, edit, progress (`exportFraction` while exporting), `overlayExcluded`, `overlayPlacement`, `lastExport`, `stopReason` (`user` or `budget`) once the capture stopped, and after a budget stop `stopDetails` (`reason` `disk_space` or `max_file_bytes`, `message`, `hint`, `budgetBytes`, `maxFileBytes`, `durationUs`, and for the disk `freeBytes`, `reserveBytes`, `availableBytes`; see [Capture and export engine](#capture-and-export-engine-x11)), error; an ended session is `recording_unavailable` and its operation holds the outcome |
| `recording.raise` / `recording.overlay` | `{id}` / `{id, visible}` | overlay control; a hidden bar stays hidden through state changes until `raise` (a full-screen clip without the handle); the recording indicator stays visible |
| `recording.edit` | `{id, revision, commands[1–100]}` | atomic `setTrim {start, end\|null}`, `resetTrim`, `addMask {mask}`, `updateMask {maskId, mask}`, `removeMask {maskId}` |
| `recording.preview` | `{id, time, maxEdge? 64–1600}` | one PNG frame (base64 `png`) at clip time with current trim/masks, plus `retained`, `outputTimeUs`, `revision`; returned only to the requesting client |
| `recording.review` | `{id, action, time?, speed?, kind?, rect?, maskId?, strength?, format?, codec?}`; `action` also `saveCopyDialog` / `closeFileDialog` | works one review-window control as a user does and answers with the window's state and the resident's edit (see [Review window](#review-window)); `reviewing` only, `inspect` also while `exporting` |
| `recording.saveCopy` | `{id, path, overwrite?}` | exports to an absolute path with the format's extension; no upload. Replies once the export starts with `state: exporting`; poll `inspect` until `reviewing`, then `lastExport.status` is `saved` or `failed` |
| `recording.upload` | `{id, revision}` | the only publishing action. Replies once the export starts with `state: exporting` and the linked `operationId`; see "Exports" below |

If a helper budget-stop event reaches review before a pending `stop` command's
reply, that reply returns the current finalized session, including an export
already in progress. It preserves the clip duration, stop details, edit model
and current state. A subsequent `stop` while still reviewing returns that review.

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
`disk_space_insufficient`,
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
  selector restyle at once (#198, [appearance](appearance.md#recording-overlays)).
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

## Overlay (X11)

`prepare` for a region without a rect shows a translucent drag surface on every
screen ("Drag to choose the area to record. Esc cancels.", on a label in the
interface's scheme, as is the size shown while dragging); release keeps the
bounds and nothing records yet. The ready state draws a 3 px white ring with a
dark hairline just outside the recorded rectangle (never over its pixels).
While recording, four translucent strips dim everything outside the
rectangle; ring and strips are transparent for input, bypass the window
manager and stay on top, so the desktop inside and outside stays usable and
the capture never sees them.

`ximagesrc` records every visible pixel of the area, so the control bar is
placed where it is not recorded (`src/record/BarPlacement.cpp`, shared with
the KDE layer-shell path): below the rectangle, else above it (both on the
monitor holding most of it), else at the top or bottom centre of another
monitor, so a full-monitor region or a monitor target on a multi-monitor
desktop gets the bar on another monitor (`overlayPlacement: outside`). The
decision uses the bar's widest state, and the bar is placed again whenever
its size changes (ready, recording, paused, stopping), so it never runs past
its screen edge.

When nothing outside fits (a full single monitor, or a `desktop` target) the
ready bar sits in the rectangle's bottom-right corner, where it is not yet
recorded, and from Record on it collapses to a **handle**: the state dot and
Stop (Resume while paused), 88×35 px (168×35 with Resume), 10 px in from
the corner of the monitor. Record shrinks it first, or applies a hide from
`recording.overlay`, and starts capture 150 ms later, so the first frame never
shows the full bar. The handle is in the clip; `overlayPlacement`
is `handle` and `overlayExcluded` false. For a clip without it, hide the bar
with `recording.overlay {visible: false}` (CLI `recording.overlay`, MCP
`set_recording_overlay`), which keeps it hidden through state changes until
`recording.raise`, and stop with the recording shortcut, the tray,
`putgd-cli` or MCP.

The bar can be dragged by its background (not its buttons). A bar placed
outside the rectangle cannot be dragged into it; a handle can go anywhere on
its monitor. After a drag the bar keeps its spot, anchored by its right edge,
when its size changes.

The control bar shows a state dot (green ready, red recording, amber paused),
a green Record button before capture and a red Stop button during it,
the retained time, the target, a system-audio checkbox and a format/codec
choice (editable only before Record; GIF/WebP disable audio), and Record,
Pause/Resume, Stop and Cancel. From Record on the checkbox is locked but still
shows whether the clip records system audio: ticked with the redesign theme's
check on a muted fill, or empty, with a tooltip saying which (#120; before,
the locked box was drawn empty either way). Its actions run in the helper and are reported
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

`mode` defaults to `open`. The session boundary accepts a nonempty string of at
most 16 characters; the record action accepts only `open` or `toggle` and refuses
other values with `invalid_request`. `mode` belongs only to `record`; supplying
it to another action is also `invalid_request`.

```sh
putgd-cli action.invoke '{"action":"record","mode":"open"}'
putgd-cli action.invoke '{"action":"record","mode":"toggle"}'
```

Each invocation acts on the current session as described above. These calls do
not authorize upload; a stopped clip stays in private review.

## Review window

Every Stop (from the bar, the CLI, or the byte budget) opens the helper's
private review window. Since #283 the video takes the window and the tools sit
in three compact rows under it (960×600 by default, which fits a 1366×768
desktop, also with large text; the frame scales with the window):

- **The frame** at the playhead, with the masks active at that time drawn over
  it.
- **One timeline** with Play/Pause and the time: the whole clip with the
  retained interval between two trim handles, each mask's time span as a bar in
  a lane under it (overlapping masks take separate lanes, up to four), and the
  playhead. Click or drag to scrub; drag a trim handle, or a selected mask's
  end, to retime it; click a bar to select that mask. Arrows step 0.1 s (1 s
  with Shift), Home and End go to the kept interval's ends.
- **A tool row** of IconArt buttons with tooltips and accessible names:
  speed (0.5×, 1×, 2×, 4×), set trim start / end at the playhead, reset trim,
  the kept interval, Add blur / Add solid cover (then drag on the frame).
  The mask controls are contextual: the mask list appears once masks exist;
  the selected mask's start/end at the playhead and Remove appear when one is
  selected, and the blur strength only for a selected blur. A mask just added
  becomes the selection; clicking the frame outside every mask clears it.
- **The action row**: the output format (GIF/WebP drop audio; the capture rate
  cannot change), the status, Save copy… / Discard / Upload (primary).

Keys: Space plays and pauses, I and O set the trim start and end at the
playhead, Shift+I and Shift+O the selected mask's start and end, Delete removes
the selected mask, Escape drops a blur or cover not placed yet.

Previews are rendered in the helper by the same `previewFrame` the CLI and
MCP preview use, so timing and placement agree with the export. While a mask
is being dragged the pending area is drawn as a dashed rubber band. Play /
Pause plays the retained interval silently from the playhead at the chosen
speed: frames are decoded in order on a worker by `FrameSequence`, masks
applied as the export renders them, and shown at their clip time; frames that
fall behind the clock are decoded but not drawn, so a large clip keeps pace
with dropped frames. Scrubbing or any edit control pauses playback; the audio
track is not previewed (the export keeps it).

`recording.review {id, action, ...}` (MCP `drive_recording_review`) works the
window's controls as a user does, one action per call, in `reviewing` (and
`inspect` while `exporting`): `inspect`, `seek {time}`, `play`, `pause`,
`speed {speed}`, `setIn`, `setOut`, `resetTrim`, `addMask {kind, rect}` (from
the playhead to the trim end, as Add blur/cover and a drag), `selectMask
{maskId|null}`, `setMaskIn`, `setMaskOut`, `strength {strength}`, `removeMask`,
`format {format, codec}`, `saveCopyDialog` (opens **Save copy…** as the button
does: the desktop's chooser through the XDG portal, or the themed fallback; see
[file-dialogs.md](file-dialogs.md)) and `closeFileDialog` (closes it as a
cancel). While that chooser is open the other actions answer
`recording_state`, as the window takes no input then. A refused action answers
`invalid_request` with the message the window shows. The answer is the window's state after the
action (`visible`, `size`, `playheadUs`, `playing`, `speed`, `trim`, `masks`
with their timeline bars, `selectedMaskId`, `armed`, `maskControls {list,
times, strength, remove}`, `status`, `notice`, `video {rect, share}`,
`timeline {rect, lanes}`, `scheme`, `fileDialog {open, mode, title, backend,
parentWindow, fallbackReason, last}`) plus the resident's `state` and `edit`: an
edit a control proposes reaches the resident as an `edit` event first, and the
resident reads the window again after pushing the result back.

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
| `stopped {durationUs, canvas, reason: user\|budget}` | the clip is finalized and under review; after `budget` the resident sends the helper `notice {text}`, which the review window shows as its banner |
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

### Timestamps and A/V sync

`pipewiresrc` frames carry the compositor's own frame time in their PipeWire
header (CLOCK_MONOTONIC: Mutter's paint time, KWin's previous output
presentation). As a live `GstBaseSrc` it shifts every one of them by the first
frame's arrival delay and waits on the clock for faster frames, so before
#86 the whole video track was late by whatever the first frame waited (0 to
90 ms on Plasma, different every recording). The helper now uses a private
`pipewiresrc` subclass without those sync times, runs the pipeline on the
monotonic system clock (pulsesrc slaves to it) and stamps each frame when it
reaches the pipeline, as `ximagesrc` stamps a frame when it reads it. Arrival
rather than the header time is deliberate: KWin's header time runs about 40 ms
ahead of the frame, and recordings stamped with it played the sound 35–48 ms
late on Plasma 6.3, while arrival times agree with the X11 path; Mutter's
frames arrive about 8 ms after their header time, so both agree there.

The working clip's audio starts after its first video frame (the sink monitor
opens after the stream: 20–110 ms on Wayland, 0 on X11). Export cuts audio
from the same clip-timeline instants as video and fills any time before the
first sample with silence; before #86 it rebased audio to its own first
sample, which played every Wayland MP4 or WebM that much early (the −53 ms of
the issue). Measured with `--av-marker`/`--av-sync` through the built-in
speakers, the exported files now read −9 to +7 ms (KDE screencast and portal
on Debian 13 Plasma 6.3, Mutter screen cast and portal on Fedora 44 GNOME 50)
against −12 to −18 ms on Kubuntu 26.04 X11, all with GStreamer's `playbin`
as the player. `ffplay` reads 4–43 ms later on the same captures because
it ignores the audio server's latency, and it doesn't play on GNOME Wayland.
The raw numbers per path are in
[the receipt](evidence/2026-10-19-wayland-av-sync.md).

Across a pause the offset now holds: after a resume the working clip moves by
at most 0.6 ms and the export by at most 1.7 ms relative to the working clip
(f44 Mutter and d69 KWin, MP4 and WebM), where nightly `134a9be` stepped the
capture by up to 17 ms and moved every export 10 ms earlier
([receipt](evidence/2026-10-20-recording-reliability.md); portal paths: [closeout](evidence/2026-10-20-recording-closeout.md#166-pauseresume-av-on-the-portal-paths)).

Lab A/B runs: `PUTGD_RECORD_PIPEWIRE_TIMESTAMPS=compositor` maps each frame's
header time instead of stamping its arrival, and `=legacy` restores the
pipeline before #86 (`pipewiresrc do-timestamp=true` on its own stream clock).
With `PUTGD_RECORD_DEBUG_LOG` set, stopping logs the pipeline clock, the
lateness of video and audio at their encoders and the age of the compositor
frame times (min/mean/max).

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
  GNOME Shell runs the extension it loaded at login until the user logs out,
  so after a package upgrade the old one stays loaded. When the loaded
  `extensionVersion` (from `Hello`) is below the packaged `metadata.json`
  version, the resident refuses with `helper_outdated`, "The put.gd GNOME
  Shell extension was updated; log out and back in to load the new version",
  rather than asking for an extension that is already enabled (#131), and
  `capabilities` reports `helper.outdated` with both versions.
- Elsewhere on Wayland a region needs `target.rect`.

`recording.select {id, rect}` (MCP `select_recording_region`) completes an
open selector of either kind as a drag would; `recording.cancel` closes it.

The **ready ring** (3 px white, dark hairline, just outside the rectangle),
the **dimming** of everything else while recording or paused and the
**control bar** follow the X11 rules where the compositor allows them:

- KDE: the ring and the dim strips are input-transparent overlay layer
  surfaces (split per output). KWin stacks overlay surfaces in the order they
  map and `raise()` doesn't reorder them, so the strips are created clear
  with the ready ring, before the bar, and Record only darkens them; strips
  mapped at Record covered the bar until #127. The bar is an overlay layer surface placed
  by the X11 rules: below the rectangle, else above it, else at the top or
  bottom of another output, else inside its bottom-right corner, where it
  collapses to the handle from Record on, and placed again on its output when
  its size changes. A rectangle's stream contains none of them unless the
  bar had to sit inside. KWin 6.6 and later also leave every
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
  the area, onto another monitor, or, failing all, inside the corner. The
  extension follows the bar window's size changes and places it again with
  that first decision, so a widened bar stays on its monitor. A bar left
  inside makes the resident report `handle` and send the helper
  `overlay {handle: true}`: from Record on the bar is the dot-and-Stop handle,
  which the extension keeps in the corner.
  `HideRecordingFrame` removes everything when the clip is under review or
  the session ends; disabling the extension, locking or the resident leaving
  the bus does too. Mutter has no per-window screencast exclusion, so a whole
  single-monitor recording on GNOME contains the handle (or nothing, after
  `recording.overlay {visible: false}`).
- A `window` stream (portal) carries only that window: `overlayExcluded` is
  true and no ring is drawn.

`overlayPlacement` reports where the bar went: `outside`, `handle` (no room
outside: X11, KDE and the GNOME Shell keep it in the area's corner and it
shrinks to the dot-and-Stop handle while recording, so the handle is in the
clip), `hidden` (KWin 6.6 leaves it out of its own stream) or `compositor`
(nobody could place it; it may be in the clip). Releases before #108 also
reported `inside` for a full bar left in the clip. `overlayExcluded` is true
only for `outside` and `hidden`.

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
missing references that strict decoders such as OpenH264 refuse). Pause and
Resume record one interval of pipeline running time that both branches share
(`src/record/PauseTimeline.cpp`): a frame or audio buffer captured inside it
is dropped (an audio buffer reaching into it is dropped whole), and everything
after it moves back by the paused time, the same for both tracks, so the
working clip's timeline excludes paused time and the audio-minus-video offset
after a resume is the one before it. The interval lasts a whole number of frames (the
nearest to the paused time), so frames after it stay on the frame grid that
the export's `fps` filter samples; off the grid they moved by up to half a
frame in the export while the audio didn't. Before #130 each branch measured its own
gap from its first buffer after the resume and continued one step after its
last kept buffer (a frame for video, a fixed 20 ms for 10 ms audio buffers),
which stepped the offset by −16 to +30 ms and left a 10 ms hole in the
audio. A recording stopped while paused keeps nothing after the pause. The
retained duration is measured from the first retained frame because some
encoders carry a constant timestamp offset.

**Encoder order and the Skylake GPU hangs (#89).** The order stays GPU first.
The lab's Intel HD 530 hosts logged seven i915 hangs between 2026-10-17 and
2026-10-20: six on k72 and one on d69. All of them were `Resetting rcs0 for
preemption time out` with `ecode 9:1:85dffffb`, in the compositor's
context (Xorg or `kwin_wayland`) on the render engine. The video engine that
`vah264lpenc` uses (`vcs0`) never reset. Four of the seven came with no
VA-API encode running. One happened during a CPU GIF export, one at idle with
builds running, and two before any recording work. The other three, in two
events, overlapped a `vah264lpenc` recording at host load 5–9. m74 (the same GPU, Mint X11)
recorded with `vah264lpenc` without a hang, and k72 was taken out of the
lab for a hardware check on 2026-10-20. A software encoder first would cost
every Intel/AMD/NVIDIA user CPU time to work around one host's render-engine
fault, so the order is unchanged. The encoder actually used stays in
`recording.capabilities` (`gstreamer.usableH264Encoders`, the first is used)
and in `recording.inspect` progress. If a GPU's encoder fails at
`prepare`, the probe moves on to the next one. Kernel versions, drivers and
the per-host table are in
[the closeout receipt](evidence/2026-10-20-recording-closeout.md#89-gpu-first-encoder-order-kept).

Stop ends the clip at once: from that running time on nothing is kept, however
long the drain takes. It then sends EOS into each branch at the element after
its source, each from its own worker thread, and the clip is finished when the
muxer's EOS reaches the bus. Before #129 the EOS went through the pipeline to
the sources, and a source takes its streaming lock for it; `pulsesrc` holds
that lock while its thread waits, for audio or for room in the audio queue.
On a VM without a sound card the monitor of the suspended `auto_null` sink
sometimes stamps its buffers about 2³² ring-buffer segments (~497 days) in the
future: the muxer never takes them, the audio queue reads as full, the audio
thread blocks, and Stop blocked the helper's main thread until the resident gave
up with `recording_helper_timeout` (2 of 5 stops in the lab's Kubuntu 26.04
guest). The encoder probe now re-anchors audio stamped more than a second ahead
of the running time to its arrival and keeps that correction for the buffers
after it (`AudioClockGuard`; `PUTGD_RECORD_DEBUG_LOG` counts the corrections).
If a drain still doesn't finish within 10 s the capture stops anyway and keeps
what reached the file (its Matroska index is missing; the clusters decode);
`recording_stop_timeout` is left for a drain that wrote nothing. The helper stops on its own and reports `stopped` with reason
`budget` when the working clip nears its budget: `limits.maxFileBytes` minus a
reserve, and at most a third of the free disk above the system reserve (the
recording, its export and its upload copy each need room). `recording.prepare`
refuses with `disk_space_insufficient`, and the tray's Record action shows the
same message, when that third would be under 64 MiB, about a minute of a busy
1080p screen, instead of starting a recording that ends itself within seconds
(#213).
Progress events carry `durationUs`, `bytes`, `paused` and the encoder names.

When the helper stops a capture on its budget, the resident says why (#225). The
reason is `max_file_bytes` when the Largest file limit set the budget, and `disk_space`
when free disk set it or the disk has less than two minimum budgets left after the
capture. The review window shows a banner above the frame, the tray posts
**put.gd — recording stopped** with the same words, and `recording.inspect` returns
them as `stopDetails`, for example: "Recording stopped at 1:42: it reached the Largest
file limit (2 MiB; recording stops at 1 MiB to leave room to finish the file). The clip
so far is kept for review; raise Largest file in Settings > Uploads to record longer."
A disk stop gives the free space and the reserve and asks to free up space before the
next recording. Either way the clip recorded so far stays in review, ready to trim,
save or upload. The resident logs `Recording stopped on its budget` with the figures.

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
slice threads on every core. Audio is trimmed with `atrim` in seconds on the
same clip timeline as the video (it starts at the first video frame; a gap
before the first sample becomes silence, see "Timestamps and A/V sync"),
turned from the clip's timestamps into contiguous samples by `aresample`
(`async=1`, 1 ms hard compensation), so a hole in the clip's audio (such as
a pause leaves) becomes silence and an overlap is cut instead of later audio
moving earlier (#130), and encoded as AAC (MP4) or Opus (WebM). MP4 output is written with `faststart`.

Opus packets are 20 ms. The last one carries the end trim as Matroska
`DiscardPadding`, which `ffprobe` shows as `Skip Samples` side data, and
every packet decodes. FFmpeg 8.0.x (Kubuntu 26.04 ships 8.0.1) logs
`[opus] Error parsing Opus packet header.` once at the end of every Matroska
or WebM Opus stream. That includes files written by the `ffmpeg` CLI and
ones without padding. The message comes from the demuxer's Opus parser, which
checks the empty flush buffer at EOF, and the decoded audio is unchanged. It
came in with FFmpeg commit 37155d68ec and was fixed in 8.1 by 618fc15e65,
not backported to 8.0. FFmpeg 7.1 (Debian 13) and 8.1 (Fedora 44) print
nothing (#119). `export_av_sync_contract` decodes every audio packet of WebM
exports with the native and libopus decoders.

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

## History poster frames

History shows a still for every stored video (#112): screen recordings and
MP4, WebM, Matroska, QuickTime, AVI and Ogg files shared from the clipboard or
a file manager. The list draws a play glyph and the length over it, so a still
is never mistaken for a screenshot; the inspector shows it larger, with the
length beside the size. GIF and WebP recordings are images and keep the image
preview (their first frame, through the image job). Animated WebP reads
through Qt's WebP image plugin, which Debian, Ubuntu and Fedora package apart
from Qt GUI, so the DEB requires `qt6-image-formats-plugins` and the RPM
`qt6-qtimageformats` (#189); the AppImage bundles `libqwebp.so` from the
pinned Qt and the Flatpak's KDE runtime carries it.

The poster is made after the operation ends, so a slow or failing decode
never delays the upload, the copied link or the copied result. The resident
queues one item at a time and, for each video item without a preview, runs
`putgd-record --poster MAX_PIXELS` (`PosterJob`, `src/core/PosterJob.*`):

- the resident opens the stored artifact itself (`O_NOFOLLOW`, regular files
  only) and hands that descriptor over as the child's standard input; the
  child reads only it, through `/proc/self/fd/0`;
- the child runs at nice 10 with 2 GiB of address space (none under
  sanitizers), 21 s of CPU time, 64 file descriptors, no core file and a
  zero file-size limit, and is killed at the 10 s deadline or on shutdown;
- FFmpeg may use only the `file` protocol and the MP4/QuickTime,
  Matroska/WebM, AVI and Ogg demuxers, so a crafted file can't make it open a
  playlist, a URL or another file; frames over `limits.imagePixels` are
  refused before decoding (`max_pixels` also bounds every later frame);
- the PNG on standard output (long edge 256 px, the length in the
  `putgd-duration-ms` text chunk) is capped at 2 MiB and checked for the PNG
  signature and size before it is cached.

The poster comes from the file as stored, so blur and cover masks baked into
an export are in it (`poster_contract` checks a covered quarter). The frame is
1 s in (half-way into shorter clips); when that frame is flat (black, a fade
or a solid fill) the helper tries 25% and 50% in, then the first decodable
frame. Posters share the image previews' cache (`<data>/previews/ID.png`,
owner-only, `retention.previewCacheBytes`, cleared by cleanup), are recorded
as the item's `previewPath` and reach `history.list`, `history.detail`, CLI
`inspect` and MCP like image previews. A missing helper or codec, or a corrupt,
truncated past its first frame or oversized file, leaves "Preview unavailable"
and a `No preview` log record with the failure; the AppImage's FFmpeg has
no H.265 decoder, so H.265 MP4 files have no poster there.

### Previews made on selection

A video stored before posters existed, one whose poster failed and one whose
poster cleanup removed get their poster when History selects them (#190). So
does an image whose preview was never made or was removed, such as a WebP
recording stored before the WebP plugin was installed. History waits 400 ms
for the selection to settle, shows "Making a preview…" and sends
`history.preview {id}`, one request at a time. History reloads the selected
item whenever the resident reports a change; the preview it already shows
stays until a reload brings a different preview file, so the inspector never
flashes empty (#285). The resident:

- answers `ready` at once when the item's preview file exists;
- makes it from the retained artifact, or else from the local copy when that
  is still the same file (regular, not a symlink, the artifact's size and
  SHA-256, checked within 10 s), through the same poster queue: a selection
  goes ahead of after-operation posters, at most 3 wait, and one that waited
  20 s is refused as `preview_busy`, so the reply comes within 45 s
  (`historyPreviewReplyMs`);
- records the preview on the item, as an after-operation poster is, and
  answers `made` with `previewPath` and `source` (`artifact` or `local`).

Every failure is soft: the inspector keeps "Preview unavailable" and no
status message appears. The refusals are `preview_source_missing` (neither
file is left), `preview_source_changed` (the local copy differs),
`preview_unsupported` (not an image or video), `previews_disabled`
(`retention.previewCacheBytes` is 0), `operation_active`, `preview_busy` and
the decoder's own errors (`poster_failed`, `poster_timeout`,
`input_excessive`, `poster_helper_missing`, `preview_failed`). A selection
that failed is not decoded again until the resident restarts; History does
not ask again in that window. The CLI (`putgd-cli history.preview
'{"id":"..."}'`) and MCP (`make_history_preview`) make the same request.

Measured on d69 (Debian 13, FFmpeg 7.1, i5-6500T, load 1.2), three runs each
with identical results, through the same limits:

| Clip | Wall | Peak RSS | Peak address space |
| --- | --- | --- | --- |
| 1080p, 60 s, H.264 (x264 ultrafast) | 0.17 s | 78 MiB | 515 MiB |
| 1440p, 20 s, VP9 (libvpx realtime) | 0.29 s | 100 MiB | 509 MiB |
| 2160p, 20 s, H.265 (x265 ultrafast) | 0.62 s | 186 MiB | 570 MiB |

A 720p recording's poster took about 0.25 s on f44, most of it process
start-up. The PNG is 12 KiB for these clips.

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

The image's `pipewire` plugin therefore comes from PipeWire 1.0.5 and talks to
whatever daemon the host runs. Its `pipewiresrc` asks for at least 8 buffers by
default, while KWin's screen casts (Plasma 6.3 to 6.6, portal and
`kde-screencast` alike) offer 2-4. PipeWire up to 1.4 linked the two anyway;
1.5.81 and later refuse ranges that don't overlap, and every start failed with
`error alloc buffers: Invalid argument` on Plasma 6.6 with PipeWire 1.6 (#137).
The recorder sets `min-buffers=1` on the source, which newer plugins use by default,
so the compositor picks the count (3 or 4 with KWin, 16 with Mutter). That also
covers the distro packages whose own plugin still asks for 8: Debian 13's comes
from PipeWire 1.4.2. Fedora 44's and Ubuntu 26.04's plugins already default to 1.
`tests/desktop/pipewire/run_buffers.sh` reproduces the
negotiation with a private PipeWire daemon, a KWin- or Mutter-like source and
any plugin directory, no desktop session needed.

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
| `drive_recording_review` | `recording.review` |
| `save_recording_copy` | `recording.saveCopy` |
| `upload_recording` | `recording.upload` |

## Verification

- `recording_model_contract` (`tests/unit/test_recording_model.cpp`):
  transitions, target/config parsing, trim/mask rebasing, atomic invalid
  edits, frame alignment, animation budgets, settings fields, artifact
  admission of a `recording` payload with the naming template.
- `recording_service_integration` (`tests/integration/test_recording_service.cpp`):
  the full lifecycle through `LocalServer` with a fake backend, including
  user stop reasons (including while paused and through the record toggle),
  record `mode` forwarding and rejection at the session boundary, stale revisions,
  local copies, export failure, cancel,
  helper events, animation limits, malformed requests, editor exclusion and
  restart interruption, a fake desktop shell selecting a region and framing
  the bar (`overlayPlacement`), cancelling and driving a shell selection
  without ever starting the helper, and the helper's `selecting` event with
  `recording.select`. Its slow-export case holds the fake export open while the
  production `putgd-cli` runs `saveCopy` and `upload`: both answer `exporting`
  at once, `inspect` shows progress, the lock on edits and `lastExport`, and a
  cancel during the export admits nothing. Deferred stop replies (success or
  error) preserve a budget-finalized clip and edited revision during a pending
  Save copy or Upload export; a stale reply cannot mutate a cancelled session's
  successor. It also checks that the CLI's wait
  for every method that waits on the helper outlasts that helper's deadline.
- `recording_helper_integration` (`tests/integration/test_recording_helper.cpp`):
  the helper supervisor against a fixture helper (probe caching, lifecycle,
  delayed graceful cancel in ready/recording/paused/reviewing and successor
  sessions, active unknown/malformed replies and foreign shutdown envelopes,
  a budget-stop event before a delayed or adjacent success/error reply and a
  subsequent stop,
  progress and helper-initiated stop events, crash, silence, wrong build,
  export timeout, broken probes, missing helper) and, when built, the product
  `putgd-record`: its probe JSON, the identity handshake, the explicit
  `capture_not_implemented` refusal and its normal exit.
- `tests/integration/test_mcp.py`: tool → method forwarding parity, the
  preview image route against the synthetic peer, a `recording.stop` that the
  peer answers after 16 s still reaching the caller through the production CLI,
  and the adapter's default deadline staying above the CLI's longest wait.
- `cli_integration` (`tests/integration/test_cli.cpp`): the real `putgd-cli`
  waits for a toggle stop delayed 16 s by a fake backend, beyond the former
  `action.invoke` 15 s budget, and returns the same finalized user-stop state
  as `recording.inspect`. This is client/session evidence, not real capture.
- `gnome_ui_ownership_contract` (`tests/desktop/test_gnome_ownership.mjs`,
  GNOME hosts only): `SelectArea` answering and `Release` cancelling, bar
  placement order, dim strips cut around the area and the bar, and refusing
  a bar whose process is not the resident's child.
- `tests/integration/test_config_schema.py`: optional `record` action,
  template and limits.
