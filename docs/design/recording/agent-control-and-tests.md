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
  cancel during the export admits nothing. Deferred successful stop replies
  preserve a budget-finalized clip and edited revision during pending Save copy
  and Upload exports; Save copy also covers a late stop error. A stale reply
  cannot mutate a cancelled session's successor. It also checks that the CLI's wait
  for every method that waits on the helper outlasts that helper's deadline.
- `recording_helper_integration` (`tests/integration/test_recording_helper.cpp`):
  the helper supervisor against a fixture helper (probe caching, lifecycle,
  delayed graceful cancel in ready/recording/paused/reviewing and successor
  sessions, active unknown/malformed replies and foreign shutdown envelopes,
  a budget-stop event before a delayed or adjacent success/error reply and a
  subsequent stop,
  progress and helper-initiated stop events, crash, silence, wrong build,
  export timeout, broken probes, missing helper) and, when built, the product
  `putgd-record`: its probe JSON, the identity handshake, a direct `prepare`
  that either succeeds or fails with `capture_unavailable` or
  `recording_target` (the test accepts all three, so it does not prove that a
  real X11 session resolves the target), and its normal exit.
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
- #1364: `recording_model_contract` (the live microphone, its device name and
  `clickHighlights`); `recording_service_integration` (a microphone alone and
  mixed, refusal without an input or with a monitor as the default, the
  remembered toggle never blocking the record action, loss mid-recording, the
  click watch from Record to Stop with a fake desktop);
  `recording_helper_integration` (`--microphones` asked afresh, broken answers
  reading as no microphone); `bar_placement_contract` (the toggle, the meter
  and the facts); `click_overlay_contract`
  (`tests/unit/test_click_overlay.cpp`: the ring's pixels in I420 and NV12 and
  where a desktop click lands in the picture); `hyprland_contracts` (the bind
  script, token, unbind rule and which buttons still need a bind);
  `gnome_ui_ownership_contract` (the extension's click watch);
  `kwin_click_effect_contract` (`tests/integration/test_kwin_click_effect.cpp`:
  against a scripted KWin and KGlobalAccel, the effect comes back exactly as it
  was from each starting state, also after Record followed at once by Stop);
  `labwc_bridge_contract` (Budgie's mouse binds: `rc.xml` with them added and
  taken out byte for byte, the actions each bind stands in for, put back after
  the bridge's rewrite, removed after a crash, the D-Bus entry);
  `budgie_labwc_integration` (in a real headless labwc: a press over a window
  reaches put.gd and the window; the recorder's own-cursor capture keeps its
  frames); `export_av_sync_contract` (tone levels and the moving marker). None
  of these records a real microphone.
