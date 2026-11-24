# Screen recording: agent control and tests

Part of [Screen recording](../recording.md).

## Agent and MCP control

| Tool | Session method |
| --- | --- |
| `get_recording_capabilities` | `recording.capabilities` |
| `list_recording_microphones` | `recording.microphones` |
| `prepare_recording` (with `microphone`, `microphone_device`, `click_highlights`) | `recording.prepare` |
| `control_recording` (start/pause/resume/stop/cancel/raise) | `recording.<command>` |
| `get_recording` | `recording.inspect` |
| `select_recording_region` | `recording.select` |
| `set_recording_overlay` | `recording.overlay` |
| `set_recording_audio` | `recording.audio` |
| `edit_recording` (trim, cuts, covers, `undo`, `redo`) | `recording.edit` |
| `preview_recording` (MCP image) | `recording.preview` |
| `drive_recording_review` | `recording.review` ([the actions](#review-window-actions)) |
| `save_recording_copy` | `recording.saveCopy` |
| `upload_recording` | `recording.upload` |

The review window's controls are reachable through `recording.review`, and each action
routes through the same handler as its UI (the rule in
[AGENTS.md](../../../AGENTS.md)), with two exceptions: the Format menu's **audio
toggle** has no review action (it is a footer control that changes whether the export
carries the captured audio), and there is **no fresh-Upload action**: a new upload is
`recording.upload` (`upload_recording`), and a failed one is repeated with `retryExport`.
The edit operations themselves (`addCut`, `updateCut`,
`removeCut`, `undo`, `redo` beside the trim and cover ones) go through `recording.edit`;
the model, limits and error codes are in
[the edit model](session-contract.md#time-geometry-and-the-edit-model).

### Review window actions

`recording.review {id, action, ...}` (MCP `drive_recording_review`) works one control
per call. Each action calls the function its button, key or gesture calls; the checks
only turn a refused state into an error. Times are integer microseconds on the
recorded-clip timeline (source time), rectangles `[x, y, width, height]` in canvas
pixels.

**Errors.** An edit-proposing action that is refused **by the window, in `drive()`**, with
whatever code below (bad values, nothing selected, a dialog open), **also cancels a waiting
Upload**, through the same path as a refused gesture, and keeps its own error code. During an
active export the resident rejects these API calls before `drive()` runs (`recording_state`),
so that refusal does not cancel a waiting Upload; an Upload cannot be sent during an export
anyway. These are `setIn`, `setOut`,
`resetTrim`, `addMask`, `placeMask`, `useSolidCover`, `trimToLimit`, `setMaskIn`, `setMaskOut`,
`strength`, `removeMask`, `cut`, `adjustCut`, `restoreCut`, `undo`, `redo`, `setCoverEdge`,
`setCoverEdgeToPlayhead`, `coverKind`, `setTrim`, `retimeCover` and `setCutEdge`.

- `invalid_request`: unknown actions, missing or malformed fields, out-of-range values
  and unknown IDs; also an action that needs a selection the window lacks and checks
  itself (`placeMask` with nothing armed or selected, with both or neither of `rect` and `at`, or with an `at` outside the canvas (whole pixels, like `rect`), `setCutEdge` or `scrollTimeline` with a bad value, `strength`, `removeMask`,
  `useSolidCover`, `selectMask` with an unknown ID, and `setMaskIn`, `setMaskOut`,
  `setIn`, `setOut` when the edge would cross the other, or `setOut`, `setMaskOut` and
  `setCoverEdgeToPlayhead` with `edge: end` when no frame is shown for the playhead).
- `recording_state`: the window's state refuses the action. A cover being placed
  (`selectRange`, `cut`, `redo`, `markSelection`, `setCutEdge`, `saveCopyDialog`, `retryExport`), a
  change or gesture still unanswered or a window state that blocks the retry (`retryExport`; with
  no failed export in the window it is `invalid_request`, and `recording_state` only when the
  resident itself lacks the failed request), a refusal detected or a chosen copy already
  waiting (`saveCopyDialog`), no time selection
  (`cut`), no selected cover (`seekCoverStart`, `seekCoverEnd`, `setCoverEdge`,
  `setCoverEdgeToPlayhead`, `coverKind`, `retimeCover`), nothing to undo, redo or step
  through, no placement to cancel, an excluded cover (`seekCoverStart`, `seekCoverEnd`),
  playing or skimming turned off (`skim`), every cover already shown in the lanes
  (`moreCovers`), the timeline at Fit (`scrollTimeline`), a timeline drag in progress
  (`timelineZoom`, `timelineFit`, `zoomIn`, `zoomOut`, which the timeline ignores then), the frame at the
  playhead not shown yet (`setCutEdge` with `edge: end`), a panel that is not folded (`panel`), the picture at Fit (`viewerPan`), an
  export running, the discard confirmation or the **Save a copy…** chooser open.
- **The outcome of the action's own proposals** is its result. Proposals are applied
  **individually**, and each action reports only the proposals it made: a proposal the
  resident refused is the action's error (the model's code and message, with
  `details {revision, proposal}`: for example `recording_cut_empty` for a cut that
  would leave less than one frame, `invalid_request` for a 65th cut, `recording_stale`
  for a stale revision), and a proposal that was never answered is `recording_state`
  ("The edit was not answered"). On success the answer lists the proposals the action made
  as `proposals: [n, ...]`, present **only when the action made one**. An action that proposes an edit is `cut`, `adjustCut`,
  `restoreCut`, `setTrim`, `setIn`, `setOut`, `resetTrim`, `trimToLimit`, `addMask`,
  `placeMask`, `setMaskIn`, `setMaskOut`, `setCoverEdge`, `setCoverEdgeToPlayhead`,
  `coverKind`, `retimeCover` (an `updateMask`), `setCutEdge` (an `updateCut`), `strength`, `removeMask`, `useSolidCover` (an `updateMask` for a selected blur), `armRedact` and `armBlur` (which commit a pending nudge first), `undo` and `redo`. An error does not mean no
  proposal was applied (an action may have made several, applied one by one): **after any
  error, `inspect` again and read the edit and revision before exporting or repeating the
  action.** `lastRefusal` in `inspect` is for display only: it is the latest refusal the
  window saw, which a later proposal may have replaced.
- **Snapping.** Times sent through raw `recording.edit` are snapped by the resident to the
  frame grid in the safe direction (covers and cuts outward, the trim inward). Review actions
  such as `setCoverEdge` round to the nearest frame first and the resident keeps that edge.
  The answer's `edit` shows the values kept, and an edit that snapping would leave with less than one frame is
  refused. A mask must be at least 2 × 2 pixels (`invalid_request`). See
  [the edit model](session-contract.md#time-geometry-and-the-edit-model).

| Action | Fields | What it does |
| --- | --- | --- |
| `inspect` | | the window's state, unchanged (below) |
| `seek` | `time` | moves the playhead |
| `play`, `pause` | | Space; `play` cancels a placement first |
| `speed` | `speed` 0.5, 1, 2 or 4 | the Play menu's speed |
| `frameStep` | `delta` 1 or −1 | `.` and `,`: one frame |
| `setIn` | | I: the trim's start at the playhead |
| `setOut` | | O: the trim's end after **the frame on screen**: the window takes that frame's stored time, rounds it to the nearest grid index, and ends at the next frame edge, round((*k* + 1) × 10⁶ / `fps`), bounded by the recording's end. If the seek has not been shown yet it decodes the frame first; if still none is shown for the playhead it is refused (`invalid_request`, "The frame at the playhead isn't shown yet. Try again once it is.") and never falls back to a nominal frame. `inspect.preview.shownUs` reports the frame's time |
| `setTrim` | `start`, `end` (null or omitted: the recording's end) | the brackets, in one step. The window needs `start < end` within the recording (`invalid_request`). A trim shorter than one nominal frame is always `invalid_request`; a trim long enough by itself that leaves less than one frame once the cuts are subtracted is `recording_cut_empty` |
| `resetTrim` | | the Edits row's Reset |
| `trimToLimit` | | trims the end so the kept time fits the GIF or WebP limit; refused when it already does |
| `selectRange` | `start`, `end` | drags a time selection on the filmstrip; `start < end` within the recording |
| `clearSelection` | | Esc: clears the time or edit selection |
| `cut` | | C, or the Cut button: cuts the time selection; the playhead then lands 1 s before the seam |
| `selectCut` | `cutId` | clicks a cut |
| `adjustCut` | `cutId`, `start`, `end` | drags a cut's edges |
| `restoreCut` | `cutId` | Restore |
| `playAcross` | `cutId` | plays from 1 s of kept footage before the cut |
| `undo`, `redo` | | the resident's history (Ctrl+Z, Ctrl+Shift+Z); `undo` while placing a cover cancels the placement, `redo` then is refused; nothing to step is `recording_state` |
| `armRedact`, `armBlur` | | X and B: commit a pending nudge, then arm a cover for the time selection, or for the whole recording |
| `placeMask` | exactly one of `rect` or `at` `[x, y]` | while a cover is armed, `rect` draws its box as a drag does (one `addMask`) and `at` is a click: it places the default box (about 20 % × 8 % of the frame, kept inside it) centred on that canvas point; otherwise `rect` moves or resizes the selected cover. `at` needs a cover armed and takes two whole numbers inside the canvas (`invalid_request`) |
| `cancelPlacement` | | Esc, or the banner's Cancel |
| `addMask` | `kind` blur or solid, optional `rect` | arms and places in one step: the box (default: centred, half the frame) over the time selection, **or over the whole recording when nothing is selected** (the same as X and B; widening the trim later never uncovers anything) |
| `selectMask` | `maskId`, or null to clear | selects a cover |
| `selectEdit` | `kind` cut, cover or trim, `editId` (none for a trim) | selects a row of the Edits list |
| `nextEdit`, `previousEdit` | | Alt+→ and Alt+← |
| `seekCoverStart`, `seekCoverEnd` | | Shows from → and Until →: the first or last *kept* covered frame of the selected cover |
| `setCoverEdge` | `edge` start or end, `time` | types a time into the cover's **Shows from** or **Until** field and commits it. The window rounds `time` to the **nearest** frame first, and the resident keeps that grid edge unchanged (raw `recording.edit` snaps directionally instead). It changes nothing (and succeeds) when the time rounds to the edge the cover already has, and is `invalid_request` when it rounds to or across the cover's other edge. Needs the cover's time fields to exist (`recording_state` when they are not shown) |
| `setCoverEdgeToPlayhead` | `edge` start or end | Set to playhead for the selected cover; `edge: end` ends after the frame on screen, like `setOut`, with the same refusal when none is shown |
| `setMaskIn`, `setMaskOut` | | the selected cover's start at the playhead, or its end after the frame on screen, like `setOut` (Shift+I and Shift+O) |
| `coverKind` | `kind` solid or blur | the panel's Redact \| Blur choice for the selected cover |
| `useSolidCover` | | turns the selected blur into a redaction (one `updateMask`, one undo step), or an armed Blur into an armed Redact (no edit yet) |
| `strength` | `strength` 2–64 | the selected blur's strength |
| `removeMask` | | removes the selected cover |
| `timelineZoom` | `level`: one of `inspect` `timeline.zoomLevels` (1 is Fit; the ladder 1, 2, 3, 4, 6, 8, 12, 16, 24, … depends on the clip) | the zoom menu's levels; any other level is `invalid_request` and names the valid ones |
| `timelineFit` | | back to Fit (Shift+Z) |
| `zoomIn`, `zoomOut` | | `=` and `-`: one level closer or farther. The anchor is the playhead when it is on screen, else the middle of the view |
| `setSkimming` | `on` (boolean) | S and the **Skim** toggle. Off, hovering never changes the viewer (`inspect` `timeline.skimming`) |
| `setSnapping` | `on` (boolean) | N and the **Snap** toggle (`inspect` `timeline.snapping`) |
| `retimeCover` | `start`, `end` | slides or retimes the selected cover, as dragging its bar's body does: one edit (`updateMask`) and one undo step; `start < end` within the recording (`invalid_request`), a selected cover needed (`recording_state`) |
| `setCutEdge` | `cutId`, `edge` start or end | the cut's **Set to playhead** (Starts or Ends): the frame on screen is the cut's first frame for `start` (falling back to the playhead's time when no frame can be resolved) and its last for `end` (which needs a resolved frame after the window's decode attempt), as for a cover; `invalid_request` for an unknown cut or edge, `recording_state` while a cover is being placed or when the frame is not shown yet (`end`) |
| `scrollTimeline` | `start` | scrolls the zoomed timeline's view to start at that source time, clamped to the duration minus the visible span's length (read the result in `timeline.visible.start`), as dragging the overview track does, and suspends playback's follow until playback restarts or the playhead is back in view; `start` within the recording (`invalid_request`), `recording_state` at Fit |
| `moreCovers` | | the "+n" chip: the Edits list, scrolled to the first cover the lanes hide; `recording_state` when none is hidden |
| `markSelection` | `edge` start or end | `[` and `]`: the time selection's start or end at the playhead, whatever is selected; `recording_state` while a cover is being placed |
| `editPoint` | `delta` 1 or −1 | Up and Down: the playhead to the next or previous edit point (a trim end, the kept side of a cut boundary, a cover edge); nothing happens when there is none |
| `viewerZoom` | `mode` fit or actual | Fit or 100 % (source pixels) |
| `viewerPan` | `dx`, `dy` (source pixels) | pans at 100 %; `recording_state` at Fit |
| `skim` | `time` | paused hover on the timeline, as the pointer after hover intent: the viewer keeps its picture until the exact frame with its covers arrives (it never shows a thumbnail), or a cached or nearby one when no cover edge lies between; `recording_state` while playing and while skimming is off (`setSkimming` first) |
| `skimEnd` | none | leaves the skim and restores the playhead frame; takes no time and also clears a skim while playing |
| `panel` | `open` (boolean) | the folded Edits overlay; `recording_state` while the panel is beside the viewer |
| `resize` | `width`, `height` (200–16000) | resizes the window, so the panel folds or unfolds |
| `format` | `format`, `codec` | the output format menu. Offered combinations: `mp4` with `h264` or `h265`, `webm` with `vp9`, and `gif` or `webp` with an empty or omitted `codec`; anything else is `invalid_request` |
| `saveCopyDialog`, `closeFileDialog` | | opens **Save a copy…** as the button does, or closes it as a cancel. `saveCopyDialog` commits typed changes first, like the press; it is `recording_state` when it detects a refusal ("No copy was saved: a change wasn't applied. Check the edits, then save again.") or when a chosen copy is already waiting for the last change to be saved |
| `dismissStatus`, `showAdvice` | | clears the status line, or opens an error's details |
| `showInFolder`, `copyPath`, `dismissSavedCopy` | | the saved-copy result's actions |
| `retryExport` | | Try again. It has **two layers of outcome, and an agent checks both**. The window's guard answers first: `invalid_request` when the window has no Try again to press (nothing failed there), `recording_state` while a cover is being placed or a change or gesture is still unanswered. Then the resident answers: `recording_state` when it has no failed request, `recording_stale` (an Upload whose edit changed since: "Press Upload to upload them"), `recording_config` (the output changed since: "Press Upload again" for an Upload, "Press Save a copy again" for a copy; both need the output unchanged), or the export-start reply (`state: exporting`, `pending`) as `recording.saveCopy` and `recording.upload` reply. It exports with **the output and revision the failed press stored**: a failed Upload at its own revision, a failed **Save a copy…** with the same path at the current revision. The agent then polls `lastExport`; if the retried request was an **Upload that hands off, `lastExport` disappears with the session, so follow it with `operation.inspect`**. A retried Upload **publishes** |
| `retryPreview` | | decodes the playhead frame again |
| `discard`, `keepReviewing`, `confirmDiscard` | | Discard… asks; the other two answer it. `confirmDiscard` ends the session, so its reply can time out after the recording was discarded: verify with `operation.inspect` (the operation ends `cancelled`) |

The mask-options actions of the old window (`showMaskOptions`, `closeMaskOptions`,
`nextMaskLanes`, `previousMaskLanes`, `dismissBlurTip`, `showFormatTips`) are
removed: those controls no longer exist.

**What `inspect` returns.** The answer is the window's state, plus the resident's
`state` and `edit`:

| Field | Meaning |
| --- | --- |
| `selection` | the time selection `{start, end, lengthUs}`, or `null`. **The shape changed in #1613:** it was `{kind, id, span?}` for any selection in the first redesign builds, and it now holds the time selection only; a selected cut, cover or trim is `selectedEdit` |
| `selectedEdit` | `{kind: cut\|cover\|trim, id}`, or `null`. A selected trim has `id: null`; its row in `edits` has `id: ""` |
| `edits` | the Edits list in source order: `{kind, id, name, span {start, end}, selected}` |
| `cuts`, `masks` | each with `id`, span and its rectangle on the timeline (`timeline`); masks also `name` and `kind` |
| `trim` | `{start, end}` |
| `playhead` | `{sourceUs, outputUs}`; `playheadUs` and `outputPlayheadUs` remain |
| `outsideKeptPart` | true while the playhead is on footage the export drops (a cut, or outside the trim) |
| `outputDurationUs` | the kept time |
| `undo` | `{canUndo, canRedo}` |
| `viewer` | `{zoom: fit\|actual, percent, pan {x, y}, live, arrowsClaimed, placeWaiting, selectedCover}`. `live` is `null` or `{maskId, pending}` for the cover being moved, resized or nudged (`pending` while the frame without it has not arrived); a blur-strength preview that the panel is still coalescing counts as live too; `arrowsClaimed` is true while the arrow keys belong to the picture; `placeWaiting` is true while a placement waits for its frame; `selectedCover` is `null` or `{box [x, y, w, h], grips [[x, y] × 8]}` in window coordinates, the grips in handle order (top-left, top, top-right, right, bottom-right, bottom, bottom-left, left) |
| `placement` | `null`, or `{kind, start, end, whole, banner}` while a box is being placed |
| `warning` | the blur banner's text, or `null` |
| `tools` | `{cut, undo, redo, pictureZoom, editsToggle}`: whether Cut, Undo and Redo are enabled, the toolbar's "Fit · n%" text, and whether the Edits toggle is shown |
| `folded` | whether the right panel is folded into the Edits overlay |
| `panel` | `{mode: empty\|selection\|cover\|cut\|edits, title, rows, buttons, folded, overlayOpen}`: the panel's state, its heading (`"Edits"` in `edits` mode, otherwise the panel's own title text), the Edits rows (the same as `edits`) and the visible buttons with their enabled state |
| `size` | the window's `[width, height]` |
| `timeline` | `zoom`, `zoomLevels` (the levels `timelineZoom` accepts; 1 is Fit), `visible {start, end}`, `selection {start, end}` or `null`, `selectedEdit {kind, id}` (empty strings when nothing is selected), `skim {time, exact}` or `null`, `snapping` and `skimming` (booleans), `hover {kind, id}` or `null` (the part under the pointer: `trim-start`, `trim-end`, `selection-start`, `selection-end`, `cut`, `cut-start`, `cut-end`, `cover`, `cover-start`, `cover-end`, `playhead`, `more` or `overview`), `pending {kind trim\|cut\|cover, id, start, end}` or `null` (a released drag painted where it was dropped until the resident answers; it clears when the timeline starts easing back, after 1 s), `lanes`, `hiddenCovers`, `tiles {shown, placeholders}`, and the rectangles `rect`, `strip` and `ruler` (window coordinates) |
| `lastRefusal` | `null`, or `{code, message, revision, proposal}` for the latest edit the resident refused |
| `uploadWaiting` | true while an Upload waits for the resident to answer every proposal. It is cancelled only when what it would export changes: the revision moves other than for the awaited answers, the output changes (even with the revision unchanged), any refusal received while it waits (including one for a later proposal), a trim or cover change the window refuses locally, or 2 s without an answer. A drag or placement started after it do not cancel it; a new press is blocked only while placing or mid-drag. Input, dialogs, menus, hover, `inspect`, `dismissStatus`, a raise and a saved-copy push do not cancel it (see [Commit and upload](overlay-and-review.md#interaction-rules)) |
| `playbackQueuedEnd` | true when the latest playback run played to the end and queued its last frame (so an agent can tell a finished run from a stopped one) |
| `preview` | `{error, stale, frameTimeUs, shownUs, frameSize [w, h], skimSize [w, h], edgePreviewUs, edgeHeld}`: the shown frame's error, whether it is an earlier frame, `frameTimeUs` (the **requested** time while paused; `null` before the first frame), `shownUs` (the time of the picture actually shown for the current playhead, `null` when none was decoded for it), the frame's size in pixels (at 100 % the source size) and the size of the skim picture in the viewer (`[0, 0]` when not skimming); `edgePreviewUs` is the **requested** preview time of a dragged bracket or edge (it may precede the decoded frame's own time), held until the proposal is answered (`null` otherwise), and `edgeHeld` is true while that held frame waits for the answer; it expires after 1.5 s |
| `toolNote` | the tool row's short note (an undo's "Undid: …", a speed, the arrows' owner) or `null` |
| `timeline.skim` | `null`, or `{time, exact}`: the skimmed time and whether the exact frame has arrived (`false` while the viewer still shows the earlier picture, or a nearby frame, and the flag reads "≈") |
| `statusFacts` | every status fact that holds for the current state, as `{text, tone, actions}`, each action being `{label, drive}` (`drive` is the call the action makes). The window shows one line at a time, an error first: `statusRow` is what is shown, and can hold a cancel notice ("Upload cancelled: …") on a line above one other status line |

The rest is unchanged: `visible`, `playing`, `speed`, `armed`, `audio`, `audioRequested`, `audioEffective`, `selectedMaskId`, `status`,
`statusRow`, `savedCopy`, `fileDialog`, `video {rect, share}`, `format` and
`discardConfirmation`.

## Verification

The second round of [removed tests](../../testing/removed-tests.md#second-round)
cut these suites to the recording guarantees whose failure would be silent and
costly. What they no longer check (wording, capability reasons, frame rate,
budgets, the control bar's placement, click highlights, microphones, helper
lifecycle walk-throughs, A/V sync tones) is left to desktop acceptance.

- `recording_model_contract` (`tests/unit/test_recording_model.cpp`): the live
  disk budget leaves room to finalize and copy; trim and masks use source time
  and are rebased at export; a recording artifact streams its exact bytes with
  its declared type and naming template.
- `recording_model_contract` also covers the cut rules (#1613): `addCut` and
  `updateCut` clamp, merge and keep the list sorted, a cut or trim that leaves
  less than one frame is refused, at most 64 cuts, and `undo` and `redo` step the
  history, always advance the revision, are refused when mixed with other commands,
  and drop the oldest step past 200. The kept-span map (trim minus cuts, both
  directions, seams, a cover across a cut) is checked beside it.
- `recording_service_integration` (`tests/integration/test_recording_service.cpp`):
  the lifecycle through `LocalServer` with a fake backend. Only the explicit
  Upload export is admitted; cancel and failure admit no bytes; review uses the
  destination snapshot accepted at the start; a stop on a full disk keeps the
  clip and its private review; a failed finalization claims neither review nor
  upload; retry from review repeats only the failed upload; a late stop reply
  keeps the edits of a pending export and cannot change a cancelled session's
  successor.
- `recording_helper_integration` (`tests/integration/test_recording_helper.cpp`):
  a helper crash and protocol faults fail the session without an upload.
- `recording_storage_integration` and `recording_pipeline_throughput`: a stopped
  bar cannot reappear over review, a pipeline failure and an automatic stop end
  the portal share, a recording produces a clip with frames, and a refused
  PipeWire request is retried with the plain one.
- `bar_placement_contract`: one click never discards a recording, and no
  microphone is recorded unless asked for.
- `gnome_ui_ownership_contract` (`tests/desktop/test_gnome_ownership.mjs`,
  GNOME hosts only): `SelectArea` answering and `Release` cancelling, bar
  placement order, refusing a bar whose process is not the resident's child,
  and `WatchClicks` reporting presses to the resident only, never key presses.
- `hyprland_contracts`: the click binds do not consume the press, need the
  token and are removed only when put.gd's bind is alone.
- `labwc_bridge_contract`: Budgie's click binds in `rc.xml` come out byte for
  byte, a broken file is left alone, and binds a crash left behind are removed.
- `tests/integration/test_config_schema.py`: optional `record` action,
  template and limits.
- `export_av_sync_contract`: an export with sound keeps the audio start and an
  audio gap, and a WebM's audio decodes every packet. Since #1613 it also exports
  cut clips (the kept footage joins on the frame grid, 60 fps included, and no tone from
  inside a cut is heard), frame-gap clips (a span whose first frame comes after a gap
  starts at its start instant and keeps its audio), covers inside a gap, on a frame
  held across a cut and ending inside a gap (decoded pixels in MP4 and GIF), a one-frame
  GIF or WebP, and the GIF length limit. Animated WebP is decoded by the test itself,
  because some FFmpeg builds read still WebP only.
- `filmstrip_contract`: the thumbnail worker returns tiles at the requested size
  from their keyframe, decodes the times in one keyframe's group once, drops
  queued times when a new request replaces them, stops promptly on cancel or
  destruction, reports a missing clip once, and the skimmer decodes the exact
  frame with covers applied and drops a cancelled answer.
- `gui_filmstrip_timeline` (`tests/gui/test_filmstrip_timeline.cpp`) pins the timeline's
  pointer rules: the hit priority and zones (`hitPriorityAndZones`,
  `cutBodiesAndCoincidentGrips`), press, click and drag (`stripPressSeeksDragSelectsClickClears`,
  `stripPressWhilePlaying`, `capClickAwayFromTheLineSeeks`), one edit on release with the
  grab offset and dead zone (`cutEdgeDragSelectsAndAdjustsOnceOnRelease`,
  `coverEndAndSlideRetimeOnceOnRelease`, `dragLimitsAndRestores`), cancel by Esc or a right
  press (`escapeAndRightPressCancel`), a released drag waiting for its answer
  (`releasedEdgeWaitsForItsAnswer`), the late press (`aLatePressKeepsTheGripItLeft`),
  snapping in screen pixels (`snappingTargets`, `snapReachIsInPixelsOnLongClips`),
  autoscroll (`dragAutoscrollsAboveFit`), the zoom ladder and trackpad zoom
  (`zoomLadder`, `trackpadZoomSettlesAndPanKeepsItsStart`), skim intent and its
  settling (`skimNeedsIntentThenMovesRestsAndLeaves`, `crossingOrSkimmingOffNeverSkims`,
  `skimIntentAndSeeks`, `noSkimAfterAClickUntilThePointerMoves`), hover reporting
  (`hoverReportsTheItemUnderThePointer`), that selecting a shown cover keeps the lanes
  (`selectingAShownCoverKeepsTheLanes`), the skim's drift and presses
  (`skimDriftAndPresses`, `skimEntryEndsAndCoverEdges`), a scrub's narrow snap and the cap
  following the pointer (`scrubSnapsNarrowlyAndTheCapFollowsThePointer`) and the ends of a
  recording and a cut press (`endsSeeksAndCutPressRestore`), and a slide keeping its frame count,
  the armed range of one frame and zoom ignored mid-drag (`slideFramesArmedRangeAndZoomMidDrag`).
- `gui_review_panel` shows the controls of every selection state, that its buttons and
  fields emit their edits, that Remove by pointer returns focus to the timeline and by key
  moves on (`removeByPointerReturnsFocusAndByKeyMovesOn`), and that the folded overlay's
  list keeps its fit after an edit (`overlayListKeepsItsFitAfterAnEdit`), and that a refused cut time shows the cut again
  (`aRefusedCutTimeShowsTheCutAgain`).
- `gui_review_window_*` (`tests/gui/test_review_window_actions.cpp`) covers the window's
  wiring: a cut lands the playhead before the seam, a cover spans the selection on whole
  frames, undo follows the resident's history and the length limit counts kept time; and
  the feel rules: plain Enter never uploads (`plainEnterNeverUploads`), the keys work from
  any focus (`windowKeysWorkFromAnyFocus`), timeline clicks keep playing and the selection
  (`timelineClicksKeepPlayingAndTheSelection`, `pressesWhilePlaying`), every picture handle
  resizes from the opposite side (`everyHandleResizesFromTheOppositeSide`,
  `handlesAnswerOnEverySide`), picture gestures have a dead zone and cancel
  (`viewerGesturesHaveADeadZoneAndCancel`), a moved cover stays covered until its frame
  arrives (`aMovedCoverStaysCoveredUntilItsFrameArrives`), edge frames are held until the
  answer (`edgeFramesHoldUntilTheAnswer`), the skim settles on the clip's last frame
  (`skimAtTheLastFrameSettles`, `skimPictureEndsCleanly`), a placement follows the
  selection (`placementFollowsTheSelection`), panel fields hand the keys back
  (`panelFieldsHandTheKeysBack`), typed times commit before an upload
  (`typedTimesAreCommittedBeforeUpload`) and a cut's times can be typed
  (`cutTimesAreTyped`), the transport shows the recording time and a steady speed button
  (`transportShowsRecordingTime`, `theSpeedButtonKeepsItsWidth`) and the arrows come back at
  100 % (`arrowsComeBackAtActualSize`), a cancelled press restores a trim selection and zoom is
  ignored during a drag (`timelineTrimSelectionAndZoomDuringADrag`), `placeMask` takes one shape in
  whole pixels (`placeMaskTakesOneShapeInWholePixels`) and Save a copy waits for typed changes
  (`saveACopyWaitsForTypedChanges`).
- The surface-driver recipe `review.wheel-lanes` (`tools/lab/surface_driver/recipes/review.py`) waits for `viewer.placeWaiting` to clear before pressing Enter to place each cover, because Enter placement expires after 1 s while the frame loads, and places five covers over 10–40 % of the recording to check that two lanes show with a "+3" chip.
- `tests/integration/test_mcp.py` checks the MCP helper strictly: every
  `drive_recording_review` action and field maps to its session field, including `on`,
  `at`, `retimeCover`, `markSelection`, `editPoint` and any `timelineZoom` level, and bad
  values are refused before anything is sent.

None of these records a real microphone.
