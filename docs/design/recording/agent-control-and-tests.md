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
| `drive_recording_review` | `recording.review` (the action list: see below) |
| `save_recording_copy` | `recording.saveCopy` |
| `upload_recording` | `recording.upload` |

Every interactive control of the review window is reachable through
`recording.review`, and each routes through the same handler as its UI (the rule in
[AGENTS.md](../../../AGENTS.md)). The edit operations themselves (`addCut`,
`updateCut`, `removeCut`, `undo`, `redo` beside the trim and cover ones) go through
`recording.edit`; the model, limits and error codes are in
[the edit model](session-contract.md#time-geometry-and-the-edit-model).

> **TODO (#1613, coordinator):** the table of `recording.review` actions (name,
> fields, what each does, and what `inspect` returns for the selection, cuts,
> Edits list, zoom, panel state, placement banner and the source and output
> playheads) goes here once the final list is confirmed.

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
