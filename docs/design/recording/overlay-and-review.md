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
private review window. It is the image editor's cousin for time: a picture, a
timeline of thumbnails, and a panel that says what the selection can do. The
design was agreed by a council in the
[2026-11-17 record](https://github.com/patrick-hudson/put.gd-evidence/tree/e2d072343dc15e433648d1512d4f29fe45cd1833/records/2026-11-17-recording-review-redesign)
(#1613), which also holds its pictures and the filmstrip measurement.

### Size and layout

- **Opens at 1440×900**, bounded by the work area. The 1366×768 rule that every
  other put.gd window follows is lifted for this window only. Below about
  1180×720 the right panel folds into an overlay (below).
- Rows from the top: title (40 px) · tool row (52) · viewer (all the remaining
  height) · transport (42) · timeline (142 px at normal text, growing with the
  font) · footer (56). Text sizes and row heights come from `QFontMetrics`, so 150 %
  text lays out the same way. The right panel is 280 px wide and runs from the
  tool row to the footer; the viewer, transport and timeline use the width left of it.
- **Folded** (the window is narrower than 1180 px or lower than 720 px): the panel is
  hidden and the toolbar's **Edits** toggle (focusable, labelled "Edits") opens it as
  a 300 px overlay under the button, in the checked style while open. The overlay
  scrolls, takes focus on its default button, and Esc closes it with focus back on
  the toggle. While it is open the picture fits the uncovered width, left-aligned,
  so nothing plays under it; Play across closes it first.
- **Title:** "Review recording" · "Not uploaded". Duration, resolution, frame rate and
  codec are not in the window; they stay in History details and `recording.inspect`.

### Tool row and viewer

The tool row has the image editor's tools and keys: Select **V** · Cut **C** ·
Redact **X** · Blur **B** · Undo · Redo, then **Fit · n%** (Fit or 100 %; at 100 %
the picture pans by drag and arrows, one source pixel per device pixel) and the
Edits toggle when folded. Selecting an existing cover activates Select. Cut is
enabled only with a time selection.

The viewer shows the frame at the playhead, or the skim position while the pointer
hovers the paused timeline, with the covers active at that time drawn over it.
Banners take space from the well along its top edge and never sit over the picture:

- **Placing a cover:** "Draw the redaction · 0:08.2 – 0:12.6" (or "· whole
  recording") with a **Cancel** button (Esc). Save a copy and Upload are disabled
  until the box is placed or cancelled.
- **Blur armed or a blur selected:** "Blur may be reversible. Use Redact for private
  information." (the image editor's wording, in the warning colours).

### Transport

Play is a split button whose menu holds the speeds 0.5×–4× (1× by default).
Previous and next frame (`,` and `.`, named in their tooltips) follow, then **one
clock**: output time / output length (m:ss.s, the kept time, what the exported clip
will show). Hover it for the recording time, the source time the ruler and the trim
brackets use. Beside it a muted caption reads "clip time" (with "· Excluded from
output" while the playhead is on footage the export drops), then "Preview is silent"
with a speaker glyph. The timeline zoom button (magnifier and level) cycles Fit, 2×
and 4× and back; Ctrl+wheel steps through Fit, 2×, 4×, 8× and 16×, around the
pointer. Playback skips every cut and the footage outside the trim: the output clock
runs only over what the export keeps, and the preview never plays the audio.

### Timeline

- A ruler in recording (source) time, captioned "recording time" at its right end
  (ruler labels under the caption or the skim flag are skipped); a filmstrip of
  about 72 px thumbnails; the trim brackets in the accent colour with the part
  outside dimmed.
- **Cuts** are hatched in place with a "Cut · n s" chip (a scissors glyph when the
  chip does not fit). **Covers** have their own track under the filmstrip: two lanes
  of named bars (the selected cover in the accent colour with handles, idle ones
  neutral), names elided, "+n" for covers past the second lane, and "No covers yet"
  when empty. The selected cover's span is also tinted 20 % accent with a 3 px base
  line.
- A **time selection** is a dashed, accent-outlined range. A time selection and a
  selected edit never exist together.
- The **playhead** has a cap on the ruler and a two-tone stroke that stops above the
  covers track. While paused, hovering shows a thin **skim line** with its own time
  flag on the ruler, painted "≈0:05.3" until the exact frame has arrived.
- Every colour is a `Theme` token, so the timeline follows the light and dark schemes.

### Right panel

What it shows follows the selection:

| Selection | Panel |
| --- | --- |
| Nothing edited | "No edits yet" · "Upload sends the whole clip." and five gestures: brackets trim, drag selects time, C or Del cuts, X or B redacts or blurs, Ctrl+Z undoes. No clip facts or keyboard help. |
| A time selection | "Selected time", its span and length, and **Cut**, **Redact**, **Blur**. |
| A cover | Its name; "Covered pixels are removed from the saved and uploaded clip." (Blur shows the warning sentence instead); **Shows from →** and **Until →** with m:ss.s fields and **Set to playhead** (⇧I / ⇧O); a Redact / Blur choice, with strength for blur. The arrow buttons (Home / End) seek to the first and last *kept* covered frame, so you can check the frames either side; they are disabled with "Excluded from output" when no covered frame stays in the clip. |
| A cut | Its span and "n s left out of the clip", **Restore** (the default button) and **Play across** (from 1 s of kept footage before the join). |

Below that, once anything is edited, the **Edits** list shows every trim, cut and
cover in source order with an icon, a name and its span. A selected row shows its
verb in place of the span (Trim: Reset, cover: Remove) only when the panel above
does not already offer it; a selected cut keeps its span because Restore is above;
other rows show their verb on hover.

### Footer

**Format ▾** ("MP4 · with system audio"; codec and limits inside the menu; GIF and
WebP say "no sound") · **Discard…** (asks first; the window's Close button asks the
same question) · **Save a copy…** · a destination menu that says what Upload produces
("Project archive · Shared link", or "· Private upload") · **Upload** (the default
button; once the resident has accepted the last change it closes the window).

The Format menu also holds an **Include system audio** toggle (named for what was
recorded) that sets whether the export carries the captured sound: turning it off
removes captured system audio or microphone from the export, and a source is kept only
when it was captured **and** the window asks for it. It has no review action: `inspect`
reports it as `audioRequested` and `audioEffective`, and the format menu's `format`
action does not change it.

### Interaction rules

- The window opens paused on the first frame.
- **Select time** by dragging on the filmstrip (a half-open source-time span), or with
  `[` and `]` while the timeline has focus: `[` sets the selection's start at the
  playhead and `]` its end, and with no selection the other end is the trim's start or
  end. A click seeks and clears the selection; Esc clears it.
- **Cut** (C, or Delete or Backspace on a time selection). Delete and Backspace act in
  the viewer, the timeline and the Edits list, never in a focused field. The cut stays
  in place, hatched; playback jumps it; the output length drops. After making a cut the
  playhead lands 1 s before the join in output time, clamped to the first kept frame.
  Delete never restores a cut: use Restore or Ctrl+Z. Clicking a cut selects it, shows
  handles at its edges to adjust it, and offers Restore and Play across. Selecting a cut
  from Edits seeks to the first kept frame after it (the last kept frame before a
  trailing cut).
- **Cover** (X Redact, B Blur): arms a cover for the time selection (snapped outward to
  whole frames), or for the whole recording with no selection; the banner says which. An
  agent's `addMask` does the same, so with nothing selected it covers the whole
  recording. The viewer then pauses on the playhead frame if that lies in the selection's
  kept footage, else on the first kept frame in it, else (all excluded) on its first
  source frame marked "Excluded from output". Dragging a box places it; one command
  creates the cover's rectangle and span. Selecting a cover seeks to its first visible
  frame; one wholly outside the kept footage shows its start paused as "Excluded from
  output". Drags clamp to the clip and do not scroll. Delete on a selected cover in the
  viewer or timeline removes it; **Delete on a focused Edits row** removes that row's
  cover, except that with a time selection it cuts the selection instead.
- **Trim** with the brackets or I and O at the playhead (the end action keeps the playhead
  frame: its end is the next frame edge, round((*k* + 1) × 10⁶ / `fps`), from the frame's
  index, bounded by the recording's end); Shift+I and
  Shift+O retime the selected cover the same way. Dragging an edge of a cut or cover
  retimes it.
- **Keys.** The keys act on the window unless a text field has focus, which keeps its
  own. Space plays; `,` and `.` step a frame; ← and → move 0.1 s, with Shift 1 s, except
  in the focused viewer, where the arrows move the selected cover (Shift+arrows resize
  it, 1 px a step, 10 px with Ctrl) or, at 100 %, pan the picture. Home and End go to the
  selected cover's first and last kept frame, else the kept ends. Alt+← and Alt+→ step
  through edits in any layout. Ctrl+Z undoes and Ctrl+Shift+Z or Ctrl+Y redoes. Esc
  cancels a placement, or clears a selection.
- **Enter** depends on focus. A focused text field keeps it; a focused button presses
  itself; a focused row of the Edits list activates (selects) the row; while a cover is
  being placed it places a centred cover (half the frame's width and height); with a cut
  selected it restores the cut; otherwise it uploads. Ctrl+Enter always uploads, and does
  nothing with a menu open. Neither auto-repeats: a held Enter, which could otherwise
  trigger a second action, is ignored.
- **100 % view** shows source pixels: the helper decodes the clip at its own size rather
  than the 1280 px preview, so a cover and the picture can be checked pixel for pixel.
  Changing the zoom refreshes the paused frame or the skim frame at the new size, and
  playback at 100 % decodes at full size.
- **Undo and redo** run in the resident: one step per committed gesture (a run of
  nudges is one), covering trim, cuts and covers only. Every undo or redo is a new
  revision (see [the edit model](session-contract.md#time-geometry-and-the-edit-model)).
  Ctrl+Z while placing cancels the placement, and while a keyboard nudge is pending it
  drops the nudge.
- **Commit and upload.** A timeline drag proposes its edit when the pointer is released;
  a keyboard nudge of a cover is proposed 350 ms after the last key. **Upload, Ctrl+Enter
  and Enter do nothing during a drag** (the drag's edit has not been proposed), and
  Upload is disabled while a cover is being placed. Otherwise they send a pending nudge
  first, then **wait for every proposal's answer** from the resident, and send **that
  revision** with the output the window shows: an upload exports exactly the revision it
  started with, or it is refused. While the upload waits (`inspect` shows
  `uploadWaiting`), **anything cancels it**:
  - a mouse press, double-click, key press or wheel anywhere in the window, its dialogs or
    menus (auto-repeating keys do not count);
  - a dialog, menu or popup opening, including the destination menu and the **Save a
    copy…** chooser;
  - any `recording.review` action except `inspect`;
  - a format or audio change, or an export starting;
  - a push from the resident other than the awaited answer;
  - a refusal of one of its proposals, or 2 s without an answer.

  The status line then reads "Upload cancelled: … Press Upload again." (the reason fills
  the gap: "you kept editing", what was not applied, and so on). It is not a dialog, and
  it stays visible after the awaited answer arrives. The resident refuses an upload at any
  other revision (`recording_stale`), with an output other than the one shown
  (`recording_config`), and any export request while an export runs; it reopens the window
  with the reason and **no Try again** (`retry: false`), since pressing Upload again is the
  answer. A proposal the resident answers during an export is held and delivered when
  review resumes. Nothing is dropped silently.
- **Try again** after a failed Upload retries **that Upload's own revision**: if the edit
  changed since, it is refused ("Press Upload to upload them"). It is refused while a cover
  is being placed, and blocked while a change or gesture is still unanswered. A failed
  **Save a copy…** is retried at the current revision.
- **Held keys.** An auto-repeated Enter is ignored, so a held key places or restores once
  and never uploads, and Ctrl+Enter does not auto-repeat.

### Thumbnails and skimming

- The number of thumbnails follows the strip's width at every zoom level: 12 at the
  reference width (a 1120 px strip), at most 24. Zooming asks for thumbnails of the
  newly visible range, again one keyframe tile each. One cancellable worker opens the
  clip once, seeks backward to the keyframe before each thumbnail's time, decodes only
  that keyframe packet and scales straight to the thumbnail size; the two end
  thumbnails come first, and results are cached by keyframe time. A placeholder shows
  until each arrives. Thumbnails are keyframe pictures, not exact frames: exact frames
  come only from the skim worker, below. The strip never decodes the whole clip, and
  scrubbing, playback and Upload never wait for it. It draws the recorded picture
  **without covers**. (`putgd-record --filmstrip` times the production worker on a build
  in a standalone diagnostic, not through the review window, see
  [Lab diagnostics](capture-and-export.md#lab-diagnostics).)
- The timings below come from a **separate benchmark harness**, `bench.cpp` in the
  council record's
  [`filmstrip-test`](https://github.com/patrick-hudson/put.gd-evidence/tree/e2d072343dc15e433648d1512d4f29fe45cd1833/records/2026-11-17-recording-review-redesign/filmstrip-test)
  folder, which links the nightly's FFmpeg and runs the same keyframe method; they
  are not measurements of the shipped review window, which `putgd-record --filmstrip`
  measures. On m74 (i5-6500T) with a 3-minute 4K60 clip and the nightly's FFmpeg
  9.0.2: the two end thumbnails in 156 ms, 12 in 433 ms and 24 in 739 ms (p95, warm;
  490 ms and 846 ms cold), on one core and 55 MiB. Decoding every frame of that clip
  takes 24 s, which is why there is no whole-clip pass
  ([results](https://github.com/patrick-hudson/put.gd-evidence/blob/e2d072343dc15e433648d1512d4f29fe45cd1833/records/2026-11-17-recording-review-redesign/filmstrip-test/results.md)).
- **Skim.** Hovering the paused timeline puts that thumbnail in the viewer at once, but
  only when the tile of the keyframe before that time is already cached; otherwise the
  viewer keeps the playhead frame until the exact frame arrives. As a tile has no covers,
  **the viewer can show a picture without its covers** until the next step. After about 120 ms at rest, one exact-frame request goes out (at most one in
  flight, cancelled when the pointer moves, the decoder kept open) and replaces the
  thumbnail when it arrives, with the covers active at that time applied; the time flag
  then drops its "≈". On m74 that frame takes 217 ms median and 328 ms p95 at 4K
  (130 ms and 199 ms at 1080p), measured in the same harness. Leaving the strip restores
  the playhead frame, and skimming never moves the playhead. An edit made meanwhile
  cancels the skim frame and asks for it again, so a stale frame is never marked exact.

### Previews, edits and the resident

Previews are rendered in the helper by `previewFrame`, which the CLI and MCP preview
also use. The paused preview and the skim frame apply the covers active at the frame's
own time **and** at the requested time, so a held frame never shows uncovered at a time
the export covers. The export applies a cover to every output frame shown during its
time, by output time or by the time the shown picture was captured (see
[Covers](capture-and-export.md#kept-spans-cuts-and-frame-gaps)). **Playback is the one
exception:** `FrameSequence` decodes frames in order on a worker and masks each frame at
its own time only, so a cover wholly inside a frame gap is not drawn on the still
picture while playing. That is a limit of the local viewer; the export is correct. While
a cover is being dragged the pending area is drawn as a dashed rubber band. Play plays
the kept footage silently from the playhead at the chosen speed and follows edits made
while it plays (it resumes from the playhead, or pauses there when the edit leaves no
kept footage ahead of it; zoom, speed and edit restarts resume from the playhead, and only
Play itself wraps to the start): a cut is jumped, and frames that fall behind the clock are decoded but
not drawn, so a large clip keeps pace with dropped frames. Stopping playback drops its
queued frames. Scrubbing or any edit control pauses playback. The audio track is not previewed. **Check the saved export before
uploading sensitive footage:** the preview and the export are separate renderers.

`recording.review {id, action, ...}` (MCP `drive_recording_review`) works the
window's controls as a user does, one action per call, in `reviewing` (and
`inspect` while `exporting`). The answer is the window's state after the action plus
the resident's `state` and `edit`. Not every control has an action: the Format menu's
audio toggle (the footer's "with system audio") has none (see
[Review window actions](agent-control-and-tests.md#review-window-actions)). Errors:

- **`invalid_request`** for a value the window rejects (a missing or malformed field, a
  time outside the recording, an unknown ID) and for actions that need a selection the
  window does not have (`placeMask`, `strength`, `removeMask`, `useSolidCover`, `setMaskIn`
  and `setMaskOut`), with the message the window shows.
- **`recording_state`** when the window's state refuses the action: no time selection
  for `cut`, no selected cover for `seekCoverStart`, `seekCoverEnd`, `setCoverEdge`,
  `setCoverEdgeToPlayhead` and `coverKind`, nothing to undo or redo, a cover being placed,
  an export running, the discard confirmation or the **Save a copy…** chooser open (opened
  through the desktop's XDG portal or the themed fallback; see
  [file-dialogs.md](../file-dialogs.md)).
- **The outcome of the action's own proposals.** An edit a control proposes reaches the
  resident as an `edit` event first. If the resident refuses it (a cut that would leave
  no frame, a 65th cut, a stale revision), the action answers with the refusal as its
  error: the model's own code (for example `recording_cut_empty`, `recording_stale` or
  `invalid_request`) and message, with `details {revision, proposal}`. A proposal that
  was never answered is a `recording_state` error. `inspect`'s
  `lastRefusal {code, message, revision, proposal}` is for display only. An action that
  succeeds returns the window's state with the edit after it, so an agent can read the
  returned `edit.revision`; `recording.edit` returns the same refusal codes directly.

The window never owns the edit: each change is proposed to the resident as an
`edit {revision, commands}` event, the resident applies it to the authoritative
model and its undo history (or refuses a stale or invalid proposal, with its code and message) and pushes the
result back with the `setEdit` command, which names the proposal it answers and is also sent after every CLI/MCP
edit. "Save a copy…" and "Upload" send `saveCopyRequested {path, revision}` and
`uploadRequested {revision}`; the resident runs exactly the `saveCopy` and `upload`
paths, so a stale revision or an animation over budget is refused and the window
shows the resident's current edit again. Discard… and the window's Close button
open the same confirmation; confirming, or Escape during selection, ends the operation as
`cancelled`. Upload sends its request
once the resident has accepted every pending proposal (see "Commit and upload" above),
then closes the window at once and the export and upload continue in the background; the operation reports like any
other upload (tray, history, `operation.inspect`), and the resident shuts the helper
down when the export has been handed over. Save a copy keeps the window open with the
controls disabled and the status showing progress, so the user can still upload
afterwards. A refused export after Upload (a stale revision, or an animation over
its limits) reopens the window with the resident's reason, so the session never sits
silently in `reviewing`. `operation.cancel` on the recording's operation (the Activity
window's Cancel) ends the session the same way `recording.cancel` does, discarding the
working clip.

Choosing GIF or WebP is a format choice, not a pixel count: when the output would
exceed `limits.recordingAnimationEdge`, the resident sets `maxEdge` to the limit at
prepare and whenever the format changes, and pushes the result to the review. The
duration limit counts **kept** time (the trim minus the cuts), so a cut can bring a
long clip under it; when the kept time is still over, the status line says so ("GIF is
limited to 60 s: trim the clip") using the limits the resident sends with every
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
| `configChanged {config, microphoneRequested?, seq?}` | format/codec/audio changed on the bar (ready) or in review (where `seq` numbers the change; the resident echoes the last one it applied and the window ignores older config); `microphoneRequested` is the Microphone toggle as the user left it, which the resident remembers |
| `edit {revision, commands}` | a review-window proposal at the revision it displayed |
| `uploadRequested {revision, config}`, `saveCopyRequested {path, revision}` | the review's Upload (with the output it shows) / Save copy |
| `failed {error}` | the pipeline or helper failed; the operation ends `failed` without an upload |
