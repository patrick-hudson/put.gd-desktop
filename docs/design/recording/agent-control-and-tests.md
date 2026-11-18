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

Every interactive control of the review window is reachable through
`recording.review`, and each routes through the same handler as its UI (the rule in
[AGENTS.md](../../../AGENTS.md)). The edit operations themselves (`addCut`,
`updateCut`, `removeCut`, `undo`, `redo` beside the trim and cover ones) go through
`recording.edit`; the model, limits and error codes are in
[the edit model](session-contract.md#time-geometry-and-the-edit-model).

### Review window actions

`recording.review {id, action, ...}` (MCP `drive_recording_review`) works one control
per call. Each action calls the function its button, key or gesture calls; the checks
only turn a refused state into an error. Times are integer microseconds on the
recorded-clip timeline (source time), rectangles `[x, y, width, height]` in canvas
pixels. Unknown actions, missing or malformed fields and out-of-range values are
`invalid_request`; a state that refuses an action (nothing selected, nothing to undo,
a cover being placed, an export running, the discard confirmation or the Save a copy…
chooser open) is `recording_state`.

| Action | Fields | What it does |
| --- | --- | --- |
| `inspect` | | the window's state, unchanged (below) |
| `seek` | `time` | moves the playhead |
| `play`, `pause` | | Space; `play` cancels a placement first |
| `speed` | `speed` 0.5, 1, 2 or 4 | the Play menu's speed |
| `frameStep` | `delta` 1 or −1 | `.` and `,`: one frame |
| `setIn`, `setOut` | | I and O: the trim's start or end at the playhead |
| `setTrim` | `start`, `end` (null or omitted: the recording's end) | the brackets, in one step; `start < end` within the recording |
| `resetTrim` | | the Edits row's Reset |
| `trimToLimit` | | trims the end so the kept time fits the GIF or WebP limit; refused when it already does |
| `selectRange` | `start`, `end` | drags a time selection on the filmstrip; `start < end` within the recording |
| `clearSelection` | | Esc: clears the time or edit selection |
| `cut` | | C, or the Cut button: cuts the time selection (`recording_state` without one); the playhead then lands 1 s before the seam |
| `selectCut` | `cutId` | clicks a cut |
| `adjustCut` | `cutId`, `start`, `end` | drags a cut's edges |
| `restoreCut` | `cutId` | Restore |
| `playAcross` | `cutId` | plays from 1 s of kept footage before the cut |
| `undo`, `redo` | | the resident's history (Ctrl+Z, Ctrl+Shift+Z); `undo` while placing a cover cancels the placement, `redo` then is refused; nothing to step is `recording_state` |
| `armRedact`, `armBlur` | | X and B: arm a cover for the time selection, or for the whole recording |
| `placeMask` | `rect` | while a cover is armed, draws its box as a drag does (one `addMask`); otherwise moves or resizes the selected cover |
| `cancelPlacement` | | Esc, or the banner's Cancel; `recording_state` when nothing is being placed |
| `addMask` | `kind` blur or solid, optional `rect` | arms and places in one step: the box (default: centred, half the frame) over the time selection or the whole recording |
| `selectMask` | `maskId`, or null to clear | selects a cover |
| `selectEdit` | `kind` cut, cover or trim, `editId` (none for a trim) | selects a row of the Edits list |
| `nextEdit`, `previousEdit` | | Alt+→ and Alt+← |
| `seekCoverStart`, `seekCoverEnd` | | Shows from → and Until →: the first or last *kept* covered frame of the selected cover (`recording_state` when it is excluded from the output) |
| `setCoverEdgeToPlayhead` | `edge` start or end | Set to playhead for the selected cover |
| `setMaskIn`, `setMaskOut` | | the selected cover's start or end at the playhead (Shift+I and Shift+O) |
| `coverKind` | `kind` solid or blur | the panel's Redact \| Blur choice for the selected cover |
| `useSolidCover` | | turns the selected blur, or an armed Blur, into a redaction |
| `strength` | `strength` 2–64 | the selected blur's strength |
| `removeMask` | | removes the selected cover |
| `timelineZoom` | `level` 1 (Fit), 2, 4, 8 or 16 | the timeline's zoom |
| `timelineFit` | | back to Fit |
| `viewerZoom` | `mode` fit or actual | Fit or 100 % |
| `viewerPan` | `dx`, `dy` (source pixels) | pans at 100 %; `recording_state` at Fit |
| `skim`, `skimEnd` | `time` | paused hover on the timeline: the tile at once, the exact frame later; `skimEnd` leaves. `recording_state` while playing |
| `panel` | `open` (boolean) | the folded Edits overlay; `recording_state` while the panel is beside the viewer |
| `resize` | `width`, `height` (200–16000) | resizes the window, so the panel folds or unfolds |
| `format` | `format`, `codec` | the output format menu |
| `saveCopyDialog`, `closeFileDialog` | | opens **Save a copy…** as the button does, or closes it as a cancel |
| `dismissStatus`, `showAdvice` | | clears the status line, or opens an error's details |
| `showInFolder`, `copyPath`, `dismissSavedCopy` | | the saved-copy result's actions |
| `retryExport`, `retryPreview` | | Try again after a failed Save a copy or Upload (an Upload retry publishes); decode the playhead frame again |
| `discard`, `keepReviewing`, `confirmDiscard` | | Discard… asks; the other two answer it |

The mask-options actions of the old window (`showMaskOptions`, `closeMaskOptions`,
`nextMaskLanes`, `previousMaskLanes`, `dismissBlurTip`, `showFormatTips`) are
removed: those controls no longer exist.

**What `inspect` returns.** The answer is the window's state, plus the resident's
`state` and `edit`:

| Field | Meaning |
| --- | --- |
| `selection` | the time selection `{start, end, lengthUs}`, or `null`. **The shape changed in #1613:** it was `{kind, id, span?}` for any selection in the first redesign builds, and it now holds the time selection only; a selected cut, cover or trim is `selectedEdit` |
| `selectedEdit` | `{kind: cut\|cover\|trim, id}`, or `null` |
| `edits` | the Edits list in source order: `{kind, id, name, span {start, end}, selected}` |
| `cuts`, `masks` | each with `id`, span and its rectangle on the timeline (`timeline`); masks also `name` and `kind` |
| `trim` | `{start, end}` |
| `playhead` | `{sourceUs, outputUs}`; `playheadUs` and `outputPlayheadUs` remain |
| `outputDurationUs` | the kept length |
| `undo` | `{canUndo, canRedo}` |
| `viewer` | `{zoom: fit\|actual, percent, pan {x, y}}` |
| `placement` | `null`, or `{kind, start, end, whole, banner}` while a box is being placed |
| `warning` | the blur banner's text, or `null` |
| `folded`, `panel` | whether the right panel is folded into the Edits overlay; `panel` holds `folded`, `overlayOpen`, `rows` and the visible buttons |
| `size` | the window's `[width, height]` |
| `timeline` | `zoom`, `visible {start, end}`, `skim {time, exact}` or `null`, `lanes`, `hiddenCovers`, `tiles {shown, placeholders}`, and the rectangles `rect`, `strip` and `ruler` (window coordinates) |

The rest is unchanged: `visible`, `playing`, `speed`, `armed`, `selectedMaskId`, `status`,
`statusRow`, `savedCopy`, `fileDialog`, `preview`, `video {rect, share}`, `format` and
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
  cut clips (the kept footage joins on the frame grid, video and audio cut at the
  same instants) and frame-gap clips (a span whose first frame comes after a gap
  starts at its start instant and keeps its audio).
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
