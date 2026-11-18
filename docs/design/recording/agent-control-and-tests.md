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

**Errors.**

- `invalid_request`: unknown actions, missing or malformed fields, out-of-range values
  and unknown IDs; also an action that needs a selection the window lacks and checks
  itself (`placeMask` with nothing armed or selected, `strength`, `removeMask`,
  `useSolidCover`, `selectMask` with an unknown ID, and `setMaskIn`, `setMaskOut`,
  `setIn`, `setOut` when the edge would cross the other, or `setOut`, `setMaskOut` and
  `setCoverEdgeToPlayhead` with `edge: end` when no frame is shown for the playhead).
- `recording_state`: the window's state refuses the action. A cover being placed
  (`selectRange`, `cut`, `redo`, `saveCopyDialog`, `retryExport`), no failed export to
  retry (`retryExport`), no time selection
  (`cut`), no selected cover (`seekCoverStart`, `seekCoverEnd`, `setCoverEdge`,
  `setCoverEdgeToPlayhead`, `coverKind`), nothing to undo, redo or step through, no
  placement to cancel, an excluded cover (`seekCoverStart`, `seekCoverEnd`), playing
  (`skim`), a panel that is not folded (`panel`), the picture at Fit (`viewerPan`), an
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
  `coverKind`, `strength`, `removeMask`, `useSolidCover` (an `updateMask` for a selected blur), `armRedact` and `armBlur` (which commit a pending nudge first), `undo` and `redo`. An error does not mean no
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
| `placeMask` | `rect` | while a cover is armed, draws its box as a drag does (one `addMask`); otherwise moves or resizes the selected cover |
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
| `timelineZoom` | `level` 1 (Fit), 2, 4, 8 or 16 | the timeline's zoom |
| `timelineFit` | | back to Fit |
| `viewerZoom` | `mode` fit or actual | Fit or 100 % (source pixels) |
| `viewerPan` | `dx`, `dy` (source pixels) | pans at 100 %; `recording_state` at Fit |
| `skim` | `time` | paused hover on the timeline: the tile shows only when the preceding keyframe's tile is cached (otherwise the viewer keeps what it showed, possibly an earlier skim picture), then the exact frame; `recording_state` while playing |
| `skimEnd` | none | leaves the skim and restores the playhead frame; takes no time and also clears a skim while playing |
| `panel` | `open` (boolean) | the folded Edits overlay; `recording_state` while the panel is beside the viewer |
| `resize` | `width`, `height` (200–16000) | resizes the window, so the panel folds or unfolds |
| `format` | `format`, `codec` | the output format menu. Offered combinations: `mp4` with `h264` or `h265`, `webm` with `vp9`, and `gif` or `webp` with an empty or omitted `codec`; anything else is `invalid_request` |
| `saveCopyDialog`, `closeFileDialog` | | opens **Save a copy…** as the button does, or closes it as a cancel |
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
| `viewer` | `{zoom: fit\|actual, percent, pan {x, y}}` |
| `placement` | `null`, or `{kind, start, end, whole, banner}` while a box is being placed |
| `warning` | the blur banner's text, or `null` |
| `tools` | `{cut, undo, redo, pictureZoom, editsToggle}`: whether Cut, Undo and Redo are enabled, the toolbar's "Fit · n%" text, and whether the Edits toggle is shown |
| `folded` | whether the right panel is folded into the Edits overlay |
| `panel` | `{mode: empty\|selection\|cover\|cut\|edits, title, rows, buttons, folded, overlayOpen}`: the panel's state, its heading (`"Edits"` in `edits` mode, otherwise the panel's own title text), the Edits rows (the same as `edits`) and the visible buttons with their enabled state |
| `size` | the window's `[width, height]` |
| `timeline` | `zoom`, `visible {start, end}`, `selection {start, end}` or `null`, `selectedEdit {kind, id}` (empty strings when nothing is selected), `skim {time, exact}` or `null`, `lanes`, `hiddenCovers`, `tiles {shown, placeholders}`, and the rectangles `rect`, `strip` and `ruler` (window coordinates) |
| `lastRefusal` | `null`, or `{code, message, revision, proposal}` for the latest edit the resident refused |
| `uploadWaiting` | true while an Upload waits for the resident to answer every proposal. It is cancelled only when what it would export changes: the revision moves other than for the awaited answers, the output changes (even with the revision unchanged), any refusal received while it waits (including one for a later proposal), a trim or cover change the window refuses locally, or 2 s without an answer. A drag or placement started after it do not cancel it; a new press is blocked only while placing or mid-drag. Input, dialogs, menus, hover, `inspect`, `dismissStatus`, a raise and a saved-copy push do not cancel it (see [Commit and upload](overlay-and-review.md#interaction-rules)) |
| `playbackQueuedEnd` | true when the latest playback run played to the end and queued its last frame (so an agent can tell a finished run from a stopped one) |
| `preview` | `{error, stale, frameTimeUs, shownUs, frameSize [w, h], skimSize [w, h]}`: the shown frame's error, whether it is an earlier frame, `frameTimeUs` (the **requested** time while paused; `null` before the first frame), `shownUs` (the time of the picture actually shown for the current playhead, `null` when none was decoded for it), the frame's size in pixels (at 100 % the source size) and the size of the skim picture in the viewer (`[0, 0]` when not skimming) |
| `timeline.skim` | `null`, or `{time, exact}`: the skimmed time and whether the exact frame has arrived (`false` while only the tile shows) |
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
- `gui_filmstrip_timeline` and `gui_review_panel`: the timeline's drags (trim
  brackets, a time selection, cut and cover edges) propose one edit on release,
  its keys and zoom behave, the skim moves, rests and leaves; the panel shows the
  controls of every selection state and its buttons and fields emit their edits.
  `gui_review_window_*` covers the window's wiring: a cut lands the playhead before
  the seam, a cover spans the selection on whole frames, undo follows the
  resident's history and the length limit counts kept time.

None of these records a real microphone.
