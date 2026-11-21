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
  height) · transport (42) · timeline (168 px at normal text, growing with the
  font) · footer (56). Text sizes and row heights come from `QFontMetrics`, so 150 %
  text lays out the same way. The right panel is 280 px wide and runs from the
  tool row to the footer; the viewer, transport and timeline use the width left of it.
- **Folded** (the window is narrower than 1180 px or lower than 720 px): the panel is
  hidden and the toolbar's **Edits** toggle (focusable, labelled "Edits") opens it as
  a 300 px overlay under the button, in the checked style while open. The overlay
  ends 8 px above the transport and scrolls inside, takes focus on its default
  button, and Esc closes it with focus back on the toggle. While it is open the
  picture fits the uncovered width, left-aligned, so nothing plays under it; Play
  across closes it first. A press outside it on the picture only closes it; a
  press on the timeline, transport or tool row closes it **and** does what it
  would have done.
- **Title:** "Review recording" · "Not uploaded". Duration, resolution, frame rate and
  codec are not in the window; they stay in History details and `recording.inspect`.

### Tool row and viewer

The tool row has the image editor's tools and keys: Select **V** · Cut **C** ·
Redact **X** · Blur **B** · Undo · Redo, then the tool row's one message (below),
**Fit · n%** (Fit or 100 %; at 100 % the picture pans by drag and arrows, one
source pixel per device pixel) and the Edits toggle when folded. Selecting an
existing cover activates Select. Cut is enabled only with a time selection, and
not while a cover is being placed. Save a copy and Upload are disabled until the
box is placed or cancelled.

The viewer shows the frame at the playhead, or the skim frame while the pointer
skims the paused timeline, with the covers active at that time drawn over it. A
skim frame is an exact decoded frame with its covers applied, never a thumbnail
(see [Skimming](#skimming)). Nothing changes the picture's size or position while
the window is open: the notice and status lines overlay the viewer's top edge, and
the placement prompt, the blur warning, a frame error and short notes share the
tool row's free middle, one at a time (see [Tool-row messages](#tool-row-messages)).
While a frame is being decoded the viewer keeps the last one, never black and never
an upscaled tile. Seeking by a scrub, an edge drag or a held key delivers frames
continuously: one decode in flight on a worker with its decoder kept open, the
newest time asked for next, and an exact frame after the release; nothing decodes
on the interface thread.

### Transport

Play and its speed button are two buttons side by side, each answering over its whole
visible half. The speed button always names its speed ("1×", "2×") and keeps one width,
wide enough for "0.5×", so the transport never shifts; it opens the speeds 0.5×–4× (1×
by default). Previous and next frame (`,` and `.`, named in their
tooltips) follow, then **one clock**: output time / output length (m:ss.s, the kept
time, what the exported clip will show). Hover it for the recording time, the source
time the ruler and the trim brackets use. Beside it a muted caption reads "clip time"; once the clip has a trim or a cut it
also gives the recording time ("clip · rec 0:08.2"), and while the playhead is on
footage the export drops it reads "rec 0:08.2 · excluded". Then "Preview is silent" with a speaker glyph. At the right are the **Skim** and
**Snap** toggles (S and N, see [Skimming](#skimming) and
[Snapping, readouts and autoscroll](#snapping-readouts-and-autoscroll)) and the
timeline zoom button, which shows Fit or the level ("4×") and opens a menu of Fit and
every level of the [zoom ladder](#zoom). Playback skips every cut and the footage
outside the trim: the output clock runs only over what the export keeps, and the
preview never plays the audio.

### Timeline

- A ruler in recording (source) time, captioned "recording time" at its right end
  (ruler labels under the caption are skipped; the skim flag has its own background,
  so labels never blink under it); a filmstrip of about 72 px thumbnails; the trim
  brackets in the accent colour with the part outside dimmed.
- **Cuts** are hatched in place with a "Cut · n s" chip (a scissors glyph when the
  chip does not fit). **Covers** have their own track under the filmstrip: two lanes,
  each at least 20 px tall, of named bars that fill the lane (the selected cover in
  the accent colour with grips, idle ones neutral), names elided, a "+n" chip in its
  own reserved space at the lower lane's right end for covers past the second lane
  (it opens the Edits list at the first hidden cover), and "No covers yet" when
  empty. The selected cover's span is also tinted 20 % accent with a 3 px base line.
- A **time selection** is a dashed, accent-outlined range. A time selection and a
  selected edit never exist together.
- The **playhead** has a cap on the ruler and a two-tone stroke that runs down
  through the covers track; a selected cut's grips are drawn above it. While paused,
  skimming shows a thin **skim line** with its own time flag on the ruler, painted
  "≈0:05.3" until the exact frame has arrived (see [Skimming](#skimming)).
- Above Fit, an **overview track** under the lanes shows where the visible span sits
  in the recording and drags it (see [Zoom](#zoom)).
- Hovering a part highlights it without changing any layout: edges and trim grips
  brighten and widen from 2 to 3 px, a bar or cut body lifts. Timeline parts have
  no tooltips; they keep their accessible names and descriptions.
- Every colour is a `Theme` token, so the timeline follows the light and dark schemes.

### Right panel

Once anything is edited the **Edits** list comes first, in its own scroll area about
4½ rows tall (it scrolls beyond that), and the details of what is selected are pinned
below it and always fully visible, so selecting a row never moves a row under the
pointer. In the folded overlay the list shrinks (to as little as 1½ rows) rather than
push the details out of view. The details follow the selection:

| Selection | Panel |
| --- | --- |
| Nothing edited | "No edits yet" · "Upload sends the whole clip." and six gestures: I and O trim at the playhead (or drag a bracket), drag selects time, `[` and `]` start or end the selected time at the playhead, C or Del cuts, X or B redacts or blurs, Ctrl+Z undoes. No clip facts or keyboard help. |
| Edited, nothing selected | The Edits list alone. |
| A time selection | "Selected time", its span and length, and **Cut**, **Redact**, **Blur**. |
| A cover | Its name; "Covered pixels are removed from the saved and uploaded clip." (Blur shows the warning sentence instead); **Shows from →** and **Until →** with m:ss.s fields and **Set to playhead** (⇧I / ⇧O); a Redact / Blur choice, with strength for blur. The arrow buttons (Home / End) seek to the first and last *kept* covered frame, so you can check the frames either side; they are disabled with "Excluded from output" when no covered frame stays in the clip. |
| A cut | Its span and "n s left out of the clip", **Restore** (the default button) and **Play across** (from 1 s of kept footage before the join), then **Starts** and **Ends** with m:ss.s fields and **Set to playhead**, as a cover has: a typed time snaps to a whole frame and goes through the same adjustment as dragging the cut's edge. Set to playhead uses the frame on screen: it is the cut's first frame for Starts and its last for Ends. |

The **Edits** list shows every trim, cut and cover in source order with an icon, a
name and its span. Each row's verb (trim: Reset, cut: Restore, cover: Remove) is a
28 px trailing icon button in space the row always keeps, 8 px clear of the span, so
it never replaces or squeezes the span. It shows on the selected row, on a focused
row, and on a hovered row only after 150 ms of hover (no flicker while the pointer
crosses the list). The name has priority; when name and span do not both fit, every
row switches to the short span form. Controls update in place:

- Selecting a row changes it without rebuilding the list, and a second activation of
  the same row within the double-click interval is ignored. A click, Enter or Space
  on a row selects it; Enter never restores a cut (Restore is its own button, with
  its own Enter and Space). Up and Down move between rows. Delete on a row cuts a
  time selection, else removes that cover, and never restores a cut.
- After Restore or Remove by key (or Delete on a row), focus goes to the next surviving
  row and its focus ring shows; after a pointer click on the button, or when no row is left,
  it goes to the timeline. A cut or cover selected on the timeline, the picture or with
  Alt+← and Alt+→ scrolls its row into view. Closing the folded overlay with a press
  outside it also returns the keys to the timeline.
- Hovering a cover's bar, its box on the picture or its row highlights the other
  two (a cut's bar and row likewise); nothing changes layout.
- **Shows from**, **Until** (and a cut's **Starts** and **Ends**) commit on Enter and hand
  the keys back to the timeline (the folded overlay when it is open). A time or strength
  typed but not yet committed is committed first by anything that acts on the clip: Upload,
  Save a copy…, and a press anywhere in the window outside the field (which also hands the
  keys to the timeline before the pressed control acts). Esc reverts changed text, and a second Esc
  (or Esc on unchanged text) hands the keys back and keeps the cover selected. The
  blur **Strength** field coalesces its steps for 350 ms into one edit (one undo
  step; a held arrow keeps stepping), the picture previews the new strength at once,
  Ctrl+Z during the wait takes back only that step, and Enter commits it.

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

### Timeline pointer rules

Sizes are logical pixels at 100 % scale. The code is `src/record/FilmstripTimeline.cpp`;
the window turns its signals into edits.

**What a point hits.** One hit test serves hover, cursor, press and the accessible
parts, and painting uses the same geometry. In priority order it finds (1) the nearest
edge grip within its zone (a trim bracket, a cut edge, a cover-bar edge, a time
selection's edge), (2) the playhead's cap on the ruler, (3) a cut or cover-bar body,
(4) the overview track, (5) empty ruler, empty strip or an empty cover lane. The "+n"
chip owns its reserved space before any of these. The zones:

| Target | Zone |
| --- | --- |
| Trim bracket | its 13 px body plus 8 px each side, from 3 px above the strip to 4 px below it, and both 16 px arms; never the ruler |
| Playhead | the cap, 24 px wide over the whole ruler height. On the strip a press seeks or selects, even on the playhead's line |
| Time selection edge | 6 px each side, from 3 px above the strip to the middle of the gap above the lanes |
| Cut or cover edge (item 20 px or wider) | 8 px inside the edge and 8 px outside it when the item is selected, without selecting first; an unselected cut's or cover's edge reaches only 4 px outside; shrunk toward the outside so the body keeps 8 px; overlapping zones split at the midpoint |
| Item narrower than 20 px | unselected: only its body, at least 18 px wide, so a press selects it. Selected: its edges just outside it (10 px outside, 2 px inside) and the body between them, so a short cover can still be slid |
| Cover bar | its whole lane row for its x-range (the upper lane reaches up to the middle of the gap above the lanes) |
| Overview track | drawn 8 px, hit 16 px |

Two grips within 1 px of each other (abutting covers, a cut on the trim start, a
selection on a trim edge) are both grabbable: the selected item's grip wins, otherwise
the side of the seam the pointer is on (left of it the `*-end` grip, right of it the
`*-start` grip). A selected cut's grip wins over a trim bracket's body within 5 px of
its edge. The cursor follows the hit: a split cursor on every edge and bracket, a
horizontal-resize cursor on the playhead's cap and the ruler, an I-beam on the empty
strip (playing or not, since a drag there selects), an open hand on the selected cover's
bar (closed while sliding) and on the overview's thumb, a pointing hand over an
unselected bar, a cut body or the "+n" chip, the arrow elsewhere.

**Press, click and drag.**

- **Ruler.** A press seeks at once (on release when playing, see below); dragging
  past 3 px scrubs. Dragging the playhead's cap scrubs the same way: once it arms, the
  playhead goes to the pointer's time, as on the ruler. A plain click on the cap more than
  2 px from its line seeks to the click. A scrub keeps the selection.
- **Empty strip.** A press seeks at once. Dragging past 6 px, mostly horizontally, makes
  a time selection anchored at the press: the playhead stays at the anchor while the
  viewer shows the moving edge's frame. On release the selection stays, whatever its
  length (one frame at least), and the playhead lands on the moving edge's frame (for
  an end, the frame before it). A click that never armed is a click: it clears a **time
  selection** only, and a selected cut or cover stays selected, so Set to playhead and
  Shift+I or Shift+O work after seeking. Shift+click moves the selection's nearer edge
  (with none, from the playhead). Esc, a click on an empty cover lane (which also
  seeks) or a new drag selection clears a selected cut or cover.
- **Cuts.** A press on a cut's body selects it, and a drag from it past 6 px (mostly
  horizontal) selects time from the press. A click inside an unselected cut's painted
  span selects it; in the 4 px outside, it seeks. A press on an unselected cut's edge
  selects the cut only once the drag arms; released without arming it is a strip click.
- **Covers.** A press on an unselected cover's edge selects it and starts the edge drag
  in the same gesture. A press on an unselected bar only selects it; sliding the whole
  cover in time needs a press on the already selected bar (4 px dead zone) or 8 px of
  travel on the first press, clamps to the recording, and is one `coverRetimed` on
  release. A press on a cover's bar or edge selects it and leaves the playhead where it
  is (the Edits list and the keys seek to a selected cover; the timeline does not); a click
  never scrolls the timeline under the pointer. An unselected cover's edge reaches only 4 px
  outside it, so a seam between covers stays easy to hit. Selecting a hidden cover
  brings it into a lane; selecting a shown one never re-lays the lanes out.
- **Edge drags** (a trim bracket, a cut edge, a cover edge, a selection edge). Nothing
  changes until the pointer has moved 4 px horizontally from the press (the **dead
  zone**; a scrub's is 3 px). From then on the edge moves by the pointer's **delta** from the
  press, never to the pointer's absolute x (the **grab offset** is kept). The delta uses
  the unclamped pointer position and only the resulting time is clamped, so a bracket
  grabbed by its body moves from the first pixel past the dead zone and can be dragged
  back to 0:00 or the clip's end. Trim drags also snap to 0 and the duration. A cut's
  edge stops at a neighbouring cut's edge. Every edge keeps at least one frame between
  its two ends. The edit is proposed once, on release (one undo step); a release outside
  the widget uses the clamped position. A click on a trim bracket does not move it: on
  the kept side of the bracket's zone it is a strip click (it seeks), on the bracket's body
  it does nothing.
- **After the release** the item is painted where it was dropped until the resident
  answers (a new revision), refuses (the same revision again) or 1 s passes; the last
  two ease back over 120 ms, so nothing snaps back. A new revision that leaves the
  dragged item alone does not stop the drag; one that changes it ends the drag and
  proposes nothing.
- **A late press.** A press on empty background (empty ruler, strip or lane, never
  another target such as a cut or cover bar, the playhead or a selection edge) that
  arrives within 60 ms of the pointer leaving a grip, and within 8 px of where it left,
  takes that grip (an input stack that holds the button back, such as middle-button
  emulation, delivers it late at a moved position). The grip arms only after 4 px of
  travel past the **real** press, and the drag counts from the real press, so a click with
  no movement never edits and the edge never jumps by the pre-press offset. The picture's
  handles follow the same rule (empty picture only, 60 ms and 8 px).
- **Playback hold.** A press on the ruler, the strip or the playhead's cap while playing
  pauses playback for the press. A plain click seeks (on release) and resumes playing; a
  scrub or a time-selection drag stays paused. An edge drag pauses for its duration and
  does not resume.

**Cancel.** Esc, a right-button press and Ctrl+Z each cancel the drag in progress and
do nothing else, as does a release the widget never saw (a move arriving with the left
button up), the window losing activation, or focus going to a popup. The pre-drag state
comes back, nothing is proposed and the selection is kept; the scroll position from
before the press is restored (an autoscroll included); a cover or cut press that
selected its item gives back the earlier selection and the playhead; held playback resumes.

### Snapping, readouts and autoscroll

- **Snapping.** Edge, selection and scrub drags snap to the playhead (not for scrubs),
  the trim ends (a trim drag excludes its own), cut edges, the edges of covers shown in
  the lanes and the selection's edges, and to the recording's ends, 0:00.0 and the
  duration, which stay reachable even when the duration is not a whole number of frames.
  Never to the dragged item, and never to a target within one frame of the item's own
  starting point, so a drag that starts on the playhead moves from its first armed pixel.
  Reach is in **screen pixels at every zoom**: an edge or selection drag takes a target
  within 6 px and holds it until the pointer is 3 px beyond that (hysteresis); a **scrub**
  (the ruler or the playhead's cap) snaps within 3 px and never holds, so the frames next
  to an edit point stay reachable. Frames still snap underneath. While snapped, a 1 px
  accent guide spans the ruler to the lanes at the snapped frame. **N**, or the **Snap**
  toggle, turns snapping off and on (on by default, remembered for the session);
  holding **Ctrl** during a drag bypasses it, hides the guide, and uses the Ctrl state
  of the last move, so releasing Ctrl just before the mouse-up does not snap on the
  drop. Picture boxes do not snap.
- **Readouts.** While a drag is armed a small flag in the skim flag's style names
  what is being set; its width is reserved from the widest possible text (tabular
  digits), so it never jitters. A scrub shows the recording time above the playhead; a
  trim bracket shows `0:08.2 · 24.1 s clip` beside it (never covering it); a cut or
  cover edge shows `0:08.2 · +0.4 s` (`+3 f` in whole frames once a frame spans 2 px or
  more) and, for a cover, sits just above the Covers track near the pointer; a time
  selection shows the moving edge and the length, `0:12.6 · 4.4 s`.
- **Autoscroll.** Above Fit, a drag within 12 px of a strip end (or past it) waits 120 ms
  and then scrolls on a 16 ms timer, on a quadratic ramp from about 0.05 visible
  spans a second at the zone's inner edge to 2.5 at 60 px past the strip end. The
  dragged edge keeps following and is clamped to the visible whole frames, so
  autoscroll never pushes it out of view; a cover slide whose far end is off screen moves
  by the pointer's travel only, with no jump when it arms.

### Zoom

- **The ladder** is Fit (1), then 2, 3, 4, 6, 8, 12, 16, 24, 32, 48 and so on, up to the
  first level at which a frame is about 8 px wide for the clip's length, frame rate and
  the strip's width. `recording.review` `inspect` lists the levels as
  `timeline.zoomLevels`, and the zoom button's menu offers Fit and every one.
- **Ctrl+wheel** moves one level a notch, anchored at the pointer's time. A trackpad pinch
  or Ctrl with a two-finger scroll zooms continuously about the pointer and settles on the
  nearest level 150 ms after the gesture ends. **`=`** (or `+`) and **`-`**, plain or with Ctrl,
  zoom one level and **Shift+Z** fits; with no pointer to anchor to (keys, the menu,
  `zoomIn`, `zoomOut`, `timelineZoom`) the anchor is the playhead when it is on screen,
  else the middle of the view. Zooming never loses an on-screen playhead.
- A zoom step eases over 120 ms with the anchor holding its place on screen; tiles are
  requested when it settles, not per animation frame. A press or a scroll ends the
  animation at once.
- Above Fit the wheel and Shift+wheel scroll a tenth of the visible span a notch, and a
  trackpad scrolls by its pixels along the axis it latches after 6 px (the travel buffered
  before the latch counts). While playing, the view follows the playhead with a 10 % lead;
  a manual scroll or an overview drag suspends that until playback restarts or the
  playhead is back in view. A seek that lands off screen centres the view on it. Undo and
  redo scroll the item they brought back into view.
- The **overview track** draws 8 px tall (16 px to hit) with a thumb of at least 24 px and
  a mark at the playhead. Dragging the thumb scrolls; a press beside it centres the view
  there and carries on as a drag.

### Skimming

Skimming shows the frame under the pointer on the paused timeline without moving the
playhead.

- **Intent.** It starts only on purpose: with the pointer over the ruler or the strip
  and off every grip, either net horizontal travel of at least 12 px and at least three
  times the vertical travel, or 150 ms resting there (within 3 px). A mostly vertical step
  (coming down from the viewer) starts the measure again. Before intent nothing shows, so a
  pointer crossing the timeline on its way to something else changes nothing in the viewer.
  After a press or release in the timeline **only travel** re-engages it (resting does
  not, until the pointer is 12 px from where it was pressed), and after any key step or seek
  the skim needs fresh intent. Once engaged it ends when the pointer leaves the ruler and
  strip, rests 150 ms on a grip, or departs vertically by 8 px (measured from a vertical
  reference that sideways travel carries along, and only while the last 40 ms of motion are
  mostly vertical); passing over a grip while moving horizontally does not end it. There is
  no skimming while playing or during any drag.
- **What the viewer shows.** The viewer keeps its picture until an exact decoded frame
  with its covers applied arrives; it never shows a thumbnail, so a skim frame is never
  uncovered. The skim line and its flag move at once, and the flag reads "≈0:05.3"
  until the exact frame for that time is shown. One skim request is in flight at a time
  and the newest time replaces a queued one; while the pointer moves, an exact-frame
  request goes out at most every 50 ms and one more 40 ms after it rests. An answer
  that arrives after the pointer has moved on is shown (the flag keeping "≈") when its
  frame lies within 2 strip pixels, or 250 ms, whichever is wider, of the skim time **and
  no cover starts or ends between the two times**; the newest time is asked for at once,
  so a steady sweep updates the viewer several times a second. An exact answer for the
  time asked is accepted even when its stored frame time is slightly earlier
  (Matroska rounding, the clip's last frame). The last 30 exact frames (at most 64 MB) are
  kept, and one answers any time from its request to its own time when no cover edge
  lies between.
- **When it ends.** A skim that ends (the pointer leaves the strip, rests on a grip or
  departs vertically) returns to the playhead's picture after 250 ms with a 100 ms
  crossfade; a skim that resumes within those 250 ms carries on. A press that seeks (on the
  ruler, the strip or a lane) ends it silently, so the seek keeps the skimmed picture as the
  playhead's with no flash; any other press ends it as leaving does. An edge drag drops
  the skim at once, since it shows its own edge frame. An edit made meanwhile cancels the
  skim frame and asks again.
- **S**, or the **Skim** toggle, turns skimming off and on (on by default, remembered for
  the session). Off, hovering shows only the hairline and its time and the viewer never
  changes on hover.
- **A live skim wins for keys.** When a skim that moved within the last 1.5 s is showing
  and I, O, Shift+I, Shift+O, `[`, `]`, Space, L, J, the arrows, `,`, `.`, Up, Down or a
  timeline click arrives, the playhead first goes to the frame-snapped skim time and the skim
  picture becomes the playhead's picture, with no flash of the old frame; then the action
  runs. A skim older than that is dropped and the key acts at the playhead, which is what the
  viewer then shows.

### Covers on the picture

- **Select, hover and handles.** Hovering a cover active at the playhead outlines it
  (as does hovering its bar or row); its handles appear only when it is selected. A
  selected cover has **eight handles** drawn 8 px (10 px when hovered or dragged):
  corners resize both sides, edge middles one, and the opposite edge or corner stays
  fixed. Hit targets are generous: a corner has a 16 × 16 px target, and a band along
  each side (6 px outside the box, 6 px inside when the box is at least 40 px across that
  way, 4 px otherwise) resizes that side anywhere along it; a box too small keeps a body of
  at least 6 px at its centre. The nearest handle wins, and the selected cover's handles
  win over other covers'. Dragging a handle past the opposite side flips the box; a box
  keeps at least 2 × 2 source pixels and stays in the canvas. Cursors: diagonal resize on
  corners, vertical or horizontal resize on edge middles, an all-direction cursor over a
  movable body, a crosshair while placing, an open hand over the picture at 100 %
  (closed while panning), the arrow elsewhere.
- **First press selects.** A press on an unselected cover only selects it (and gives the
  viewer the arrow keys); moving it needs a press on the selected cover (4 px dead zone)
  or 8 px of travel on the first press. A press never grabs a handle it cannot see.
  Moves and resizes apply by the pointer's delta, also at the release point. Esc, a right
  press, Ctrl+Z or a lost window put the box back (a pan too) and keep the selection.
  While playing, the picture grabs nothing: a click only pauses. A press on empty picture
  takes the handle the pointer left within 60 ms and 8 px, and the drag counts from the real
  press, so a press that never moves changes nothing (see
  [Timeline pointer rules](#timeline-pointer-rules)).
- **Live effect.** While a cover is moved, resized or nudged its effect (the redaction's
  fill or the blur) follows the box, and the old place is not left covered: the window
  decodes the frame without that cover (`inspect` `viewer.live`), and until it arrives the
  shown frame keeps the old place covered under a dotted outline that marks it pending.
  The committed preview replaces it once the edit is answered. An answer that arrives
  mid-drag does not move the box unless that cover itself changed.
- **Placement.** X or B arms a cover (crosshair; a busy cursor while the frame it will be
  drawn on is still loading, and a placement waits for that frame on every path,
  including after a seek, so you never draw on the wrong frame). A drag may start up to
  8 px outside the picture and is clamped to it. What the drag will make is previewed as
  it goes, and the release makes exactly that: a drag under 8 × 8 view px is a click and
  places the default box, about 20 % × 8 % of the frame, centred at the press (kept in the
  frame) and selected with its handles; a larger drag, a thin one included (for example
  100 × 5 px over a line of text), creates the drawn box, at least 2 source pixels each
  way. A placement keeps its time span: a seek click or clearing the selection does not
  change it, only a range gesture does.
- **Keyboard.** After a click on a cover, or Tab into the viewer, the arrows belong to the
  viewer and the tool row says "Arrows move the box · Esc for frames". They move the
  selected cover 1 px (10 px with Ctrl), Shift+arrows resize it from the bottom-right,
  and at 100 % with no cover selected they pan (40 px, or a quarter of the view with
  Shift). The nudge shows at once and is proposed as one edit 350 ms after the last key.
  The first Esc during a nudge takes it back, the next gives the arrows back to the frames
  (focus returns to the timeline; with or without a cover selected, so the arrows are never
  trapped at 100 %), and the next deselects. While placing, Esc is the window's (it
  stops placing) and also gives the arrows back. A click on empty picture gives them back
  too. After a mouse placement the arrows keep stepping frames.
- **At 100 %** a click outside a selected cover deselects it; a drag there pans.

### Keys

Keys act from any focus inside the window except a text field or spin box, which keeps its
own keys (no single-key action fires while one has focus, even one it passes on). Buttons
and Edits rows take focus by Tab only, so a click never steals the keys; a Tab-focused
button or row takes Space and Enter as its own activation.

| Keys | What they do |
| --- | --- |
| Space | play or pause, from the skim frame when one is showing |
| K, L, J | K pauses and returns to 1×. L plays, and again steps to the next faster speed (a note "Speed 2×" shows for 1 s). J mirrors L: while playing above 1× it steps the speed down, at 1× it pauses ("Paused"), and paused it steps back one second; it does not play backward |
| `,` `.`, ← → | previous or next frame |
| Shift+← → | one second back or forward |
| ↑ ↓ | the previous or next edit point: the trim ends, the kept side of each cut boundary, every cover's edges (never a point inside a cut) |
| Alt+← Alt+→ | the previous or next entry of the Edits list |
| Home, End | the selected cover's first or last kept frame, else the first or last kept frame of the clip |
| I, O | the trim's start or end at the playhead: the start is the frame on screen, the end is after it. They set the export's in and out; `[` and `]` mark a range |
| Shift+I, Shift+O | the selected cover's start or end at the playhead, the same way |
| `[`, `]` | the time selection's start or end at the playhead, always a time selection whatever is selected (a cover's times are Shift+I and Shift+O, a cut's edges are dragged); with no selection the other end is the trim's start or end; not while placing |
| V, C, X, B | Select (cancels a placement), Cut, Redact, Blur |
| Delete, Backspace | from any focus but a text field (an Edits row handles its own first), not while placing: cut a time selection, else remove the selected cover; with a cut selected it only says "Delete doesn't restore a cut — use Restore or Ctrl+Z". In the Edits list, see [Right panel](#right-panel) |
| S, N | skimming and snapping on or off |
| `=` or `+`, `-`, Shift+Z | zoom the timeline in, out, or to Fit |
| Ctrl+Z, Ctrl+Shift+Z or Ctrl+Y | undo, redo (see "Undo and redo" below) |
| Enter | presses a focused button or row; while placing, places a centred cover (half the frame's width and height), waiting up to 1 s if the frame it is drawn on is still loading; a field keeps it. It **never uploads** unless the Upload button itself has focus |
| Ctrl+Enter | uploads, from anywhere; not with a menu open |
| Esc | cancels a drag, else a placement, else clears the selection; in the picture it also takes back a nudge, gives the arrows back, then deselects |
| Arrows, in the viewer once it has them | move the selected cover, Shift+arrows resize it, at 100 % pan |

### Tool-row messages

The tool row's free middle shows one message at a time; a lower one waits instead of being
squeezed in, and nothing is truncated to an unreadable fragment at 1440 px. In order:

1. **Placing a cover:** "Draw the redaction · 0:08.2 – 0:12.6" (or "· whole recording"), or
   "Draw the blur (may be reversible) · …", with a **Cancel** button (Esc); "Loading the frame
   to draw on…" while the frame waits more than 100 ms.
2. **A frame error** with **Retry preview**. While a placement waits for its frame the error
   comes before the prompt, so the reason shows.
3. **A short note,** for its time only: what an undo or redo changed ("Undid: Cut 0:13.0–0:19.5",
   "Redid: …", 1.5 s), the new speed ("Speed 2×", 1 s; J at 1× says "Paused"), the Delete note above (2 s), and
   "Arrows move the box · Esc for frames".
4. **The blur warning** "Blur may be reversible. Use Redact for private information.", when a
   blur is armed or selected; not repeated here while the side panel already shows it.

### Known limits

These were weighed and left as they are:

- A fast sweep of a 4K clip updates the viewer about once a second when the decode is in
  software; faster needs hardware decode or shorter keyframe intervals.
- J steps back one second; there is no reverse playback.
- Picture boxes have no snapping or alignment guides, and keyboard nudges move in source
  pixels.
- While a cover is moved, the uncovered frame takes a moment to decode: for under a second
  the old place can show covered as well, under the dotted pending outline.
- A release over another widget after leaving the timeline vertically still commits the
  clamped edge, as in other editors.
- Two time bases are shown, both labelled: clip time in the transport, recording time on
  the ruler.

### Interaction rules

- The window opens paused on the first frame.
- **Select time** by dragging on the filmstrip (a half-open source-time span), or with
  `[` and `]` from any focus but a text field: `[` sets the selection's start at the
  playhead and `]` its end, and with no selection the other end is the trim's start or
  end. A click seeks and clears the time selection; Esc clears it. The gestures are in
  [Timeline pointer rules](#timeline-pointer-rules).
- **Cut** (C, or Delete or Backspace on a time selection). Delete and Backspace act in
  the viewer, the timeline and the Edits list, never in a focused field. The cut stays
  in place, hatched; playback jumps it; the output length drops. After making a cut the
  playhead lands 1 s before the join in output time, clamped to the first kept frame.
  Delete never restores a cut: use Restore or Ctrl+Z. Clicking a cut selects it, shows
  handles at its edges to adjust it, and offers Restore and Play across. Selecting a cut
  from Edits seeks to the first kept frame after it (the last kept frame before a
  trailing cut).
- **Cover** (X Redact, B Blur): arms a cover for the time selection (snapped outward to
  whole frames), or for the whole recording with no selection; the tool row's prompt says
  which. An agent's `addMask` does the same, so with nothing selected it covers the whole
  recording. The viewer then pauses on the playhead frame if that lies in the selection's
  kept footage, else on the first kept frame in it, else (all excluded) on its first
  source frame marked "Excluded from output". Dragging a box, or clicking for a default
  one, places it ([Covers on the picture](#covers-on-the-picture)); one command
  creates the cover's rectangle and span. Selecting a cover seeks to its first visible
  frame only when the playhead is outside its span, and only from the Edits list or a key (a
  press on its timeline bar selects it and leaves the playhead); one wholly outside the kept footage
  shows its start paused as "Excluded from output". Drags clamp to the clip and do not
  scroll. Delete on a selected cover in the
  viewer or timeline removes it; **Delete on a focused Edits row** removes that row's
  cover, except that with a time selection it cuts the selection instead.
- **Trim** with the brackets or I and O at the playhead. The end actions (Set Out, O,
  Shift+O, **Set to playhead** on a cover's or cut's end, `setOut`, `setCoverEdgeToPlayhead`,
  `setMaskOut`) end after **the frame on screen**; the start actions (I, Shift+I and Set to
  playhead on a start) begin at that frame too. The window takes that frame's stored
  time, rounds it to the nearest grid index (Matroska keeps 33,333 µs as 33,000, and the
  exporter puts that frame on 33,333), and the end is the next frame edge,
  round((*k* + 1) × 10⁶ / `fps`), bounded by the recording's end. If a seek has not been
  shown yet, the window decodes the frame first; if there is still no frame shown for the
  playhead it refuses with "The frame at the playhead isn't shown yet. Try again once it
  is." and never falls back to a nominal frame. `inspect.preview.shownUs` reports the shown
  frame's time. Shift+I and Shift+O retime the selected cover the same way. Dragging an edge
  of a cut or cover retimes it, and dragging a cover's bar slides it. After a trim drag
  (or `setTrim`) that leaves the playhead outside the kept footage, the playhead moves to
  the edge just set, the frame the drag showed.
- **Keys.** The keys, their focus rules and what each does are in the [Keys](#keys) table.
- **Enter** depends on focus. A focused text field keeps it; a focused button presses
  itself (so Enter uploads only when the Upload button has focus); a focused row of the
  Edits list activates (selects) the row; while a cover is being placed it places a
  centred cover (half the frame's width and height); otherwise it does nothing. It never
  restores a cut: Restore is its own button. Ctrl+Enter always uploads, and does nothing
  with a menu open. An **auto-repeated** Enter is ignored (and Ctrl+Enter does not
  auto-repeat), so a held key cannot trigger a second action.
- **100 % view** shows source pixels: the helper decodes the clip at its own size rather
  than the 1280 px preview, so a cover and the picture can be checked pixel for pixel.
  Changing the zoom refreshes the paused frame or the skim frame at the new size, and
  playback at 100 % decodes at full size.
- **Undo and redo** run in the resident: one step per committed gesture (a run of
  nudges is one), covering trim, cuts and covers only. Every undo or redo is a new
  revision (see [the edit model](session-contract.md#time-geometry-and-the-edit-model)).
  Ctrl+Z while placing cancels the placement, while a keyboard nudge is pending it
  drops the nudge, during a timeline or picture drag it cancels only that drag, and
  during the blur strength's 350 ms wait it takes back only that step. A step the
  resident answers says what it changed in the tool row ("Undid: Cut 0:13.0–0:19.5"),
  selects the item it brought back and scrolls it into view; the playhead stays.
- **Commit and upload.** A timeline drag, or a move or resize on the picture, proposes its
  edit when the pointer is released; a keyboard nudge of a cover is proposed 350 ms after
  the last key. Two different things
  can stop an upload:
  - **What blocks a new Upload press** (Upload, Ctrl+Enter, Enter on the focused Upload
    button): a cover being placed, a
    drag in progress (its edit has not been proposed), an export running or the window
    closing. Pressing does nothing then.
  - **What cancels an Upload that is already waiting** (`inspect` shows `uploadWaiting`):
    only a change to what it would export. That is the edit's revision moving for anything
    other than the awaited answers (a later edit, undo or redo, by the user or an agent); the
    output changing (format, codec, audio or size, from the window or the resident); **any
    refusal received while it waits, including one for a later proposal**; a trim or cover
    change the window itself refuses locally, **whether a gesture or an agent's
    `recording.review` action** (see below); and 2 s without an answer. The output change
    counts even when the revision stays the same, because the helper applies the config
    (with `answer.configSeq`) before it calls the window's `setEdit`. A drag or a
    placement *started* after Upload was pressed does not cancel it: the upload exports
    what you had when you pressed it (a drag that then finishes and is accepted moves the
    revision, which does cancel).

  A press sends a pending nudge first, then **waits for every proposal's answer** from the
  resident, and sends **that revision** with the output the window shows: an upload exports
  exactly the revision it started with, or it is refused. Input, dialogs and menus opening,
  hover, `inspect`, `dismissStatus`, a raise and a saved-copy push do **not** cancel it. A
  second Upload while one waits does nothing and says "Upload already waiting for your last
  change to be saved." (the notice clears when the upload is sent), and appears only when the
  press gets past the guard above: a new press is blocked while placing, mid-drag, during an
  export and while the window is closing, and says nothing then. When it is cancelled the
  status line reads "Upload cancelled: … Press Upload again." on a line of its own above
  any other status line (so the row can hold two lines). It is not a dialog, has no
  Dismiss, and stays until Upload is pressed again, including after the awaited answer
  arrives. **An agent's refused edit cancels it too:** any edit-proposing `recording.review`
  action that is refused **by the window, in `drive()`** (bad values, nothing selected, a
  dialog open) cancels a waiting Upload through the same path as a refused gesture, and
  keeps its own error code. During an active export the resident rejects these API calls
  before `drive()` runs, so that refusal does not cancel a waiting Upload (an Upload cannot
  be sent during an export anyway). The actions are `setIn`, `setOut`, `resetTrim`, `addMask`, `placeMask`,
  `useSolidCover`, `trimToLimit`, `setMaskIn`, `setMaskOut`, `strength`, `removeMask`, `cut`,
  `adjustCut`, `restoreCut`, `undo`, `redo`, `setCoverEdge`, `setCoverEdgeToPlayhead`,
  `coverKind` and `setTrim`.
  **On the window path** the resident refuses an upload at any other revision
  (`recording_stale`) and, for the window's own upload, an output other than the one shown
  (`recording_config`). It also refuses an export request in **two different situations**:
  while another export is running ("Another export is running, so this one didn't start. Try
  again when it finishes."), and when an export timed out but the helper may still be
  finishing it ("An export is still finishing. Try again when it's done."); both are
  `recording_state`. It reopens the window with the reason and **no Try again**
  (`retry: false`), since pressing Upload again (or Save a copy again) is the answer: **retry
  your original action once the export finishes**. A direct API call (`recording.upload`,
  `recording.saveCopy`) just gets the error: during an export it fails the ordinary
  `recording_state` check, and after a timeout it gets "An export is still finishing"; it
  reopens nothing. `recording.upload` takes no output snapshot. A proposal answered during an
  export is held and delivered when review resumes, and so are a format or audio change made
  then; after an export timeout, answers and pushes stay held until the helper's late result
  arrives. Nothing is dropped silently.
- **Try again** after a failed export uses **the output and revision stored with the
  failed press** (each export request stores the output current when it was pressed, with its
  revision). A failed Upload is retried at its own revision, and if the edit changed since
  it is refused ("Press Upload to upload them"). **Both an Upload and a Save a copy retry
  need the output unchanged:** if the output changed since, it is refused with
  `recording_config` and no Try again, with "Press Upload again" for an Upload and "Press
  Save a copy again" for a copy. A failed **Save a copy…** is retried with its stored output
  at the current revision. Try again is refused while a cover is being placed, and blocked
  while a change or gesture is still unanswered.
- **Held keys.** The first Enter press can place or press a focused button, as the Enter
  rules above say, and on a focused Edits row it activates the row. An **auto-repeated**
  Enter is ignored, so a held key acts once and never uploads on repeat, and Ctrl+Enter
  does not auto-repeat.

### Thumbnails and skimming

- The number of thumbnails follows the strip's width at every zoom level: 12 at the
  reference width (a 1120 px strip), at most 24. Zooming asks for thumbnails of the
  newly visible range, again one keyframe tile each. One cancellable worker opens the
  clip once, seeks backward to the keyframe before each thumbnail's time, decodes only
  that keyframe packet and scales straight to the thumbnail size; the two end
  thumbnails come first, and results are cached by keyframe time. A placeholder shows
  until each arrives. Thumbnails are keyframe pictures, not exact frames: exact frames
  come only from the frame workers, below. The strip never decodes the whole clip, and
  scrubbing, playback and Upload never wait for it. It draws the recorded picture
  **without covers**. (`putgd-record --filmstrip` times the production worker on a build
  in a standalone diagnostic, not through the review window, see
  [Lab diagnostics](capture-and-export.md#lab-diagnostics).)
- The timings below come from a **separate benchmark harness**, `bench.cpp` in the
  council record's
  [`filmstrip-test`](https://github.com/patrick-hudson/put.gd-evidence/tree/e2d072343dc15e433648d1512d4f29fe45cd1833/records/2026-11-17-recording-review-redesign/filmstrip-test)
  folder, which links the nightly's FFmpeg and runs the same keyframe method; they
  are not measurements of the shipped review window or of the production worker.
  `putgd-record --filmstrip` is a separate standalone worker benchmark (it runs the
  production `Filmstrip` worker outside the window) and is how a build is rechecked
  against these numbers. On m74 (i5-6500T) with a 3-minute 4K60 clip and the nightly's FFmpeg
  9.0.2: the two end thumbnails in 156 ms, 12 in 433 ms and 24 in 739 ms (p95, warm;
  490 ms and 846 ms cold), on one core and 55 MiB. Decoding every frame of that clip
  takes 24 s, which is why there is no whole-clip pass
  ([results](https://github.com/patrick-hudson/put.gd-evidence/blob/e2d072343dc15e433648d1512d4f29fe45cd1833/records/2026-11-17-recording-review-redesign/filmstrip-test/results.md)).
- **Skim.** The viewer never shows a thumbnail: see [Skimming](#skimming) for when it
  skims and what it shows. Each exact frame is decoded by a worker (one for the skim, one
  for the playhead's frames) that keeps its input open, with the covers active at that
  time applied; a later time in the same group of pictures decodes forward from the last
  frame without a seek, and a time the last frame already answers (a new edit at the same
  playhead) is drawn again without decoding. On m74 a seek and decode took 217 ms median and
  328 ms p95 at 4K (130 ms and 199 ms at 1080p), measured in the same harness before the
  frame was scaled straight to the preview size. Leaving the strip restores the playhead
  frame, and skimming never moves the playhead.

### Previews, edits and the resident

Previews are rendered in the helper by `previewFrame`, which the CLI and MCP preview
also use. The paused preview and the exact skim frame check a cover against **two
instants**: the requested time and the captured time of the picture they show. The export
checks those too and also display-interval overlap, so a preview can show a later picture
where the export holds an earlier one, and a held picture in the export can come from inside
a cut (see [Covers](capture-and-export.md#kept-spans-cuts-and-frame-gaps)). The preview
and the export are separate renderers and do not promise the same frame at a given time.
**Playback masks differently again:** `FrameSequence` decodes frames in order on a worker and masks each frame at
its own time only, so a cover wholly inside a frame gap is not drawn on the still
picture while playing. That is a limit of the local viewer. While
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
the resident's `state` and `edit`. Every user control of the timeline and the picture is
an action too: skimming and snapping (`setSkimming`, `setSnapping`), zoom levels
(`timelineZoom`, `zoomIn`, `zoomOut`, `timelineFit`), sliding a cover (`retimeCover`), the
"+n" chip (`moreCovers`), `[` and `]` (`markSelection`), Up and Down (`editPoint`) and a
placement click (`placeMask` with `at`). Not every control has an action: the Format menu's
audio toggle (the footer's "with system audio") has none, and neither does a fresh Upload
(`recording.upload`; see
[Review window actions](agent-control-and-tests.md#review-window-actions)). Errors:

- **`invalid_request`** for a value the window rejects (a missing or malformed field, a
  time outside the recording, an unknown ID) and for actions that need a selection the
  window does not have (`placeMask`, `strength`, `removeMask`, `useSolidCover`, `setMaskIn`
  and `setMaskOut`), and for `retryExport` when the window has no Try again to press, with
  the message the window shows.
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

The window never owns the edit. Its wire contract, between the helper and the resident:

- **`edit {revision, commands, proposal}`** is a proposal at the revision the window showed,
  numbered by `proposal`. The resident applies it to the authoritative model and its undo
  history, or refuses it (stale revision, invalid commands, an export running), and answers
  with the `setEdit` command, whose parameters are `{edit, config, limits, answer}`: the
  current edit, the current output config, the limits, and an `answer` object
  `{proposal?, refused? {code, message, revision}, configSeq}`. The helper applies `config`
  (with `answer.configSeq`) before it calls the window's `setEdit()`. `answer.proposal` names the
  proposal answered, `refused` appears only for a refusal, and `configSeq` is the last output
  choice (the `seq` of a `configChanged`) the resident applied. A `setEdit` is also pushed
  after every CLI or MCP edit, and at once when the resident clamps the animation size
  after a format change.
- **`configChanged {config, seq}`** is the window's output choice; the window ignores any
  older config in a push.
- **`uploadRequested {revision, config}`** has one shape: the revision the upload started
  with and the output the window shows. **`saveCopyRequested {path, revision}`** and
  **`retryExportRequested`** (no fields) are the other two. The resident runs exactly the
  `saveCopy` and `upload` paths (a retry uses the stored request). Only an upload is refused
  for a stale revision: **Save a copy exports the resident's current edit** at the moment the
  resident handles the request, whatever revision the window sent. A changed output (for an
  upload) or an animation over budget is also refused, and the window shows the resident's
  current edit again.

Discard… and the window's Close button open the same confirmation; confirming, or Escape
during selection, ends the operation as `cancelled`. Upload sends its request once the
resident has accepted every pending proposal (see "Commit and upload" above), then closes
the window at once and the export and upload continue in the background; the operation
reports like any other upload (tray, history, `operation.inspect`), and the resident shuts
the helper down when the export has been handed over. Save a copy keeps the window open with
the controls disabled and the status showing progress, so the user can still upload
afterwards. A refused export after Upload reopens the window with the resident's reason, so
the session never sits silently in `reviewing`. `operation.cancel` on the recording's
operation (the Activity window's Cancel) ends the session the same way `recording.cancel`
does, discarding the working clip.

Choosing GIF or WebP is a format choice, not a pixel count: when the output would
exceed `limits.recordingAnimationEdge`, the resident sets `maxEdge` to the limit at
prepare and whenever the format changes, and pushes the result to the review. The
duration limit counts **kept** time (the trim minus the cuts), so a cut can bring a
long clip under it; when the kept time is still over, the status line says so ("GIF is
limited to 60 s: trim the clip") using the limits the resident sends with every
`setEdit`.

### Tray feedback

The tray follows the operation's observations. Before Upload the icon keeps its
idle glyph: it carries the recording badge while recording and the pause badge
while paused, and otherwise only the tooltip changes ("Ready to record",
"Reviewing the recording", "Saving a copy of the recording"). Upload is
the hand-over: the helper's export then reports the `encoding` phase and the
icon shows the Processing stage (the blue glyph, see
[appearance](../appearance.md#activity-stages)) while the clip is encoded and
stored and the local copy saved. Once the upload phase starts the ordinary
stages take over: the green fill climbing with the transferred fraction, then
Complete (green with the check badge) and idle. A failed export goes back to
review, which shows the error (`lastExport`), with no tray badge. A failure after
the clip has entered the upload pipeline shows the error badge as for any operation. No automated test has walked this chain since the second
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
| `edit {revision, commands, proposal}` | a review-window proposal at the revision it displayed, numbered by `proposal` |
| `uploadRequested {revision, config}`, `saveCopyRequested {path, revision}`, `retryExportRequested` | the review's Upload (with the output it shows) / Save copy / Try again |
| `failed {error}` | the pipeline or helper failed; the operation ends `failed` without an upload |
