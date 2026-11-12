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
| `edit_recording` | `recording.edit` |
| `preview_recording` (MCP image) | `recording.preview` |
| `drive_recording_review` | `recording.review` |
| `save_recording_copy` | `recording.saveCopy` |
| `upload_recording` | `recording.upload` |

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
  audio gap, and a WebM's audio decodes every packet.

None of these records a real microphone.
