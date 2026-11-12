# Screen recording: overlay and review

Part of [Screen recording](../recording.md).

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
`recording.raise`, and stop with the recording hotkey, the tray,
`putgd-cli` or MCP.

The bar can be dragged by its background (not its buttons). A bar placed
outside the rectangle cannot be dragged into it; a handle can go anywhere on
its monitor. After a drag the bar keeps its spot, anchored by its right edge,
when its size changes.

The control bar shows a state dot (green ready, red recording, amber paused),
a green Record button before capture and a red Stop button during it,
the retained time, the target, a system-audio checkbox, a microphone
checkbox with a level meter (#1364) and a format/codec
choice (editable only before Record; GIF/WebP disable audio), and Record,
Pause/Resume, Stop and Cancel. The microphone checkbox is off by default and
remembered; ticked, it opens the input and the meter beside it shows its level,
before and during the recording. With no input to open (none connected, or the
system default is an output's monitor) it reads **No microphone**, its box is
empty and unavailable, and its tooltip says why; the remembered choice is
kept. From Record on the bar's facts name what is recorded (**With system
audio**, **With microphone**, **System audio and microphone**, **Click
highlights**) and, if the input goes away, **Microphone disconnected**. From
Record on the two checkboxes are hidden: what the clip records can no longer be
changed, and the facts line says it. Its actions run in the helper and are reported
to the resident as events (`started`, `paused`, `resumed`, `stopped` with
reason `user`, `cancelled`, `configChanged`), which the service applies to the
same state machine the CLI drives; `recording.overlay` hides or shows the bar
and `recording.raise` brings it forward. Stop and Cancel from the bar are
mouse actions; the recording hotkey is the keyboard stop route (see
"Tray and hotkey").

### Tray and hotkey

`action.invoke {action: "record", mode?}` is the entry point behind the tray,
the dashboard rail and the hotkey (`actions.record.shortcut`, unbound
by default; recorded in Settings → Hotkeys like the others). With no active
recording it runs `recording.prepare` for a dragged region with the default
configuration (MP4, no system audio; the bar changes both before Record), the
remembered microphone toggle and the click-highlight preference
([Microphone and click highlights](microphone-and-clicks.md)). With one
active, mode `open` (the tray and the rail) raises the bar or review window,
and mode `toggle` (the hotkey) starts a `ready` session, stops a `recording`
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
[file-dialogs.md](../file-dialogs.md)) and `closeFileDialog` (closes it as a
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
for any operation. No automated test has walked this chain since the second
round of [removed tests](../../testing/removed-tests.md#second-round).

### Helper events

| Event | Meaning |
| --- | --- |
| `progress {durationUs, bytes, paused, encoder, audioEncoder, audioSources, recordingMicrophone, microphoneLost?, frameRate, frameRateReduced?, framesDelivered, framesCaptured}` / `{exportFraction}` | retained time and file size while recording (once a second); export progress (at most ten a second, so a fast GPU export stays inside the control channel's frame rate) |
| `selecting {selector: x11\|kde-layer-shell}` | the helper's region selector is open; `recording.select` can complete it |
| `started`, `paused`, `resumed` | the bar's Record / Pause / Resume |
| `stopped {durationUs, canvas, reason: user\|budget}` | the clip is finalized and under review; after `budget` the resident sends the helper `notice {text}`, which the review window shows as its banner |
| `cancelled` | Cancel on the bar, Discard in the review, the window closed |
| `configChanged {config, microphoneRequested?}` | format/codec/audio changed on the bar (ready) or the format changed in review; `microphoneRequested` is the Microphone toggle as the user left it, which the resident remembers |
| `edit {revision, commands}` | a review-window proposal at the revision it displayed |
| `uploadRequested {revision}`, `saveCopyRequested {path, revision}` | the review's Upload / Save copy |
| `failed {error}` | the pipeline or helper failed; the operation ends `failed` without an upload |
