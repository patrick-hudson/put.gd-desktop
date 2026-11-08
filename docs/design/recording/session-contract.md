# Screen recording: sessions, exports and settings

Part of [Screen recording](../recording.md).

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
| `recording.windows` | | KDE Plasma and GNOME Wayland: the windows a `window` target can follow, `windows [{identity, title, appId, rect, active, minimized}]` and `activeExcluded` (the focused window is put.gd's own); put.gd's windows are never listed. `recording_unsupported` elsewhere, `helper_outdated` while an older GNOME extension is loaded ([Window recordings](wayland.md#window-recordings)); no capture, no prompt |
| `recording.prepare` | `target {kind: monitor\|desktop\|region\|window, identity?, rect?}`, `format mp4\|webm\|gif\|webp`, `codec?`, `fps? (1–60; omitted: preferences.recordingFrameRate)`, `adaptiveFps? (boolean, default true)`, `audio? {system?, microphone?}`, `maxEdge?` | durable intent, then ready bounds/controls and any OS consent; replies at once with `state: preparing`; poll `inspect` for `ready`. A region without `rect` opens the desktop's selector (Escape cancels; `inspect` shows `selecting: true`); the chosen bounds appear in `inspect` as `target.rect`, and `overlayExcluded` / `overlayPlacement` say whether the control bar stayed out of the recorded pixels. Refused with `disk_space_insufficient` (details `freeBytes`, `reserveBytes`, `availableBytes`, `requiredBytes`, `shortfallBytes`, `hint`) before anything is accepted when free disk above the reserve can't hold three 64 MiB budgets (#213) |
| `recording.select` | `{id, rect: [x, y, w, h]}` | completes an open region selector with that rectangle as a drag would (X11 root pixels, logical desktop pixels on Wayland); `recording_state` when no selector is open |
| `recording.start` / `pause` / `resume` / `stop` | `{id}` | acknowledged once the backend reached the state; a duplicate of the in-flight command returns the current state, a different one is `recording_busy`; an acknowledged user `stop` returns `durationUs` and `stopReason: user` and enters `reviewing`, including when stopped through CLI, MCP or the toggle shortcut. A `stop` that arrives after the helper stopped on its own (its budget) answers with the existing `reviewing` state and preserves `stopReason: budget` and `stopDetails`, like a duplicate stop |
| `recording.cancel` | `{id}` | discards the clip; the operation ends `cancelled` |
| `recording.inspect` | `{id}` | state, pending command, `selecting`, target, config, canvas, `durationUs`, `retainedUs`, edit, progress (`frameRate`, optional `frameRateReduced`, `framesDelivered`, `framesCaptured`; `exportFraction` while exporting; see [Frame rate](#frame-rate)), `overlayExcluded`, `overlayPlacement`, `lastExport`, `window {identity, title, appId, rect}` for a window the resident resolved, `stopReason` (`user`, `budget`, `target_closed` when the recorded window closed, `source_lost` when its stream could not be reopened) once the capture stopped, and after a budget stop `stopDetails` (`reason` `disk_space` or `max_file_bytes`, `message`, `hint`, `budgetBytes`, `maxFileBytes`, `durationUs`, and for the disk `freeBytes`, `reserveBytes`, `availableBytes`; see [Capture and export engine](capture-and-export.md#capture-and-export-engine-x11)), error; an ended session is `recording_unavailable` and its operation holds the outcome |
| `recording.raise` / `recording.overlay` | `{id}` / `{id, visible}` | overlay control; a hidden bar stays hidden through state changes until `raise` (a full-screen clip without the handle); the recording indicator stays visible |
| `recording.edit` | `{id, revision, commands[1–100]}` | atomic `setTrim {start, end\|null}`, `resetTrim`, `addMask {mask}`, `updateMask {maskId, mask}`, `removeMask {maskId}` |
| `recording.preview` | `{id, time, maxEdge? 64–1600}` | one PNG frame (base64 `png`) at clip time with current trim/masks, plus `retained`, `outputTimeUs`, `revision`; returned only to the requesting client |
| `recording.review` | `{id, action, time?, speed?, kind?, rect?, maskId?, strength?, format?, codec?}`; `action` also `saveCopyDialog` / `closeFileDialog` | works one review-window control as a user does and answers with the window's state and the resident's edit (see [Review window](overlay-and-review.md#review-window)); `reviewing` only, `inspect` also while `exporting` |
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
`disk_space_insufficient`, `recording_disk_full` (capture could not be finalized),
`recording_unsupported`, `recording_active`, `recording_state`,
`recording_busy`, `recording_stale`, `recording_mask_unknown`,
`region_unavailable` (a Wayland desktop without a region selector),
`recording_animation_too_long`, `recording_animation_too_large`,
`recording_export_failed`, `file_exists`, plus the shared `invalid_request`,
`queue_full`, `busy_selecting`, `editor_active` and `output_required`.

## Settings

| Field | Meaning |
| --- | --- |
| `preferences.recordingFrameRate` | Optional, `30` (default) or `60`; Settings → Capture → Screen recording → Frame rate. Used by `recording.prepare` when `fps` is omitted; an explicit `fps` wins |
| `actions.record` | Optional route like the other actions; absent documents inherit the region route with no shortcut |
| `naming.recordingTemplate` | Optional, default `Screen-Recording-{yyyy}-{MM}-{dd}-{HH}-{mm}-{ss}.{ext}` |
| `limits.recordingAnimationSeconds` | Optional, default 60 |
| `limits.recordingAnimationEdge` | Optional, default 1280 |

### Frame rate

Settings → Capture → **Screen recording → Frame rate** chooses 30 (default)
or 60 frames per second for new recordings. An untouched configuration keeps
no key and uses 30; an existing explicit key is preserved when saved. CLI `settings.patch` and MCP `patch_configuration` set
`preferences.recordingFrameRate`; an explicit `recording.prepare` `fps` overrides it.

![Settings → Capture → Screen recording → Frame rate, with the new control annotated](https://github.com/patrick-hudson/put.gd-desktop/blob/fd139d3349b0dddb6a24bb889b2f0b1a55ceba78/docs/evidence/2026-10-31-recording-rate/settings-frame-rate-annotated.png?raw=true)

This is a synthetic UI catalog render, not desktop acceptance. Its provenance
and the bounded physical measurements are in the [frame-rate receipt](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-31-recording-rate/README.md).

The adaptive rate is enabled by default. The helper samples one window per
second, waits two warm-up windows, then lowers the rate after three bad windows
in a row. A good window resets that count. A window is bad when:

- the capture queue leaked at least three frames and more than 5% of delivered
  frames (`encoder`);
- X11's `ximagesrc` grabbed under 80% of the configured rate (`capture`);
- the machine was at least 90% busy while the recorder used at least half a
  CPU core (`cpu`, sampled from `/proc`).

Queue leaks and late grabs step 60 → 30 → 15; other explicit rates halve to a
15 fps floor. CPU pressure alone only lowers rates above 30, so it never takes
30 fps below that floor. Each reduction restarts the warm-up. The rate never
rises during the recording. X11 gates frames after the source; PipeWire changes
the cadence lock's period, retaining its configured-frame tolerance.
`videorate` follows `videoconvert`, so repeated frames are not converted again.
The clip keeps its configured frame rate and full timeline; repeated frames
fill the lowered capture cadence. Software x264 still encodes those repeats,
so lowering capture rate alone has a limited CPU benefit ([#1045](https://github.com/patrick-hudson/put.gd-desktop/issues/1045)).

`recording.inspect` (MCP `get_recording`) reports these under `progress`:

| Field | Meaning |
| --- | --- |
| `frameRate` | Current adaptive capture rate, not a measured distinct-frame rate |
| `frameRateReduced` | Present after a reduction: latest `{from, to, reason, atUs}`; reason is `encoder`, `capture` or `cpu`, and `atUs` is retained clip time in microseconds |
| `framesDelivered` | Cumulative frames delivered by the source to the gate |
| `framesCaptured` | Cumulative frames kept by the gate before `videorate` adds repeats |

For lab A/B runs, set `recording.prepare` `adaptiveFps: false` (MCP
`adaptive_fps: false`) or `PUTGD_RECORD_ADAPTIVE_RATE=off` in the helper's
environment. `PUTGD_RECORD_DEBUG_LOG` includes one `rate window:` line per
second with delivered/leaked counts and machine/recorder CPU shares, plus
`rate lowered …` when it changes. Use synthetic content and the desktop lock
when running the [fluidity harness](../../lab/remote-builders.md).
