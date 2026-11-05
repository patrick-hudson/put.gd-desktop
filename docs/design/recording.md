# Screen recording

Status: **slices 1 to 7 of the [recording plan](../history/plans/2026-10-17-2230-feat-screen-recording-plan.md)**,
with the open items listed in its slice status. Recording ships in the DEB,
RPM and AppImage; see "Packages and codecs".
The contract below is implemented and tested against a fake backend and the
real helper. On **X11 sessions** the `putgd-record` helper records monitors, the
desktop, regions and windows through `ximagesrc`, pauses and resumes on one
clip timeline, previews frames with masks, and exports MP4 (H.264, H.265), WebM
(VP9), GIF and animated WebP with trim and timed masks; the agent path (CLI
and MCP) is complete, and the drag selection, ready bounds, outside dimming
and floating control bar are in place, and every Stop opens the private review
window. The tray's **Capture Screen Recording** entry, the dashboard's **Record
screen** button and the `record` global shortcut start one (see "Tray and
shortcut" below). On Wayland the helper takes a PipeWire stream from the
compositor without a dialog where it can (KWin's screencast protocol on
Plasma, Mutter's screen cast service on GNOME) and otherwise through the
ScreenCast portal, whose picker is shown once per target kind thanks to
restore tokens; see "Wayland acquisition". A region without a rectangle is
chosen first with the desktop's own screenshot selector (KDE's layer-shell
selector, the GNOME Shell extension's), and the ready ring, the dimming and
the control bar stay outside the recorded area; see "Wayland selection, bounds
and bar". Desktop acceptance evidence is recorded
separately in `docs/testing/verification.md` when it exists; nothing here claims it.

## Workflow

prepare → ready → start → recording ⇄ paused → stop → **private review**
(trim, timed masks, preview, local copy) → explicit **Upload** → the ordinary
operation pipeline (immutable artifact, archive, destination, history,
clipboard). Cancel is available in every non-terminal state and uploads nothing.

- Stop **always** opens the private review, including an unedited clip and a
  stop while paused. No recording bytes reach the artifact store, archive or
  a destination before the separate `upload` command; preview, edits, local
  copies, closing the review, cancel and restart never authorize it.
- One recording session exists at a time, and it excludes the still-image
  editor (`recording_active` / `editor_active`). Its ID equals the operation
  ID that the journal holds in state `recording`. A restart turns that
  operation into `interrupted` (`recording_interrupted`), discards the
  private staging and never resumes or uploads it.
- Upload exports the current edit revision, validates the file (nonempty,
  within `limits.maxFileBytes`), then admits exactly those bytes as a
  `recording` payload: streamed from private staging, declared MIME, named
  by `naming.recordingTemplate`, category `videos` for MP4/WebM and `images`
  for GIF/WebP (animated bytes are stored as-is, never normalized). Retry
  reuses the stored artifact; a failed export keeps the review open and the
  unedited clip private.

## Topic pages

The rest of the contract is split by topic, with the text unchanged. Section names
quoted above and elsewhere are headings on these pages.

- [Sessions, exports and settings](recording/session-contract.md): Time, geometry and the edit model; Session methods; Exports; Settings; Frame rate.
- [Process boundary](recording/process-boundary.md): Process boundary; Live free-space stop (#254).
- [Overlay and review](recording/overlay-and-review.md): Overlay (X11); Tray and shortcut; Review window; Tray feedback; Helper events.
- [Wayland](recording/wayland.md): Wayland acquisition; Timestamps and A/V sync; Wayland selection, bounds and bar.
- [Capture and export engine](recording/capture-and-export.md): Capture and export engine (X11), including the lab diagnostics.
- [History posters](recording/history-posters.md): History poster frames; Previews made on selection.
- [Packages and codecs](recording/packages-and-codecs.md): Packages and codecs; AppImage runtime.
- [Agent control and tests](recording/agent-control-and-tests.md): Agent and MCP control; Verification.

## AppImage runtime

The AppImage builds `putgd-record` against a pinned upstream FFmpeg and tries the host's GPU encoders before its bundled x264 and x265. The full contract is under [AppImage runtime](recording/packages-and-codecs.md#appimage-runtime).

## Lab diagnostics

`putgd-record` has lab diagnostics for pipeline traces, kept clips, timed exports, motion scores and A/V sync (`PUTGD_RECORD_DEBUG_LOG`, `--export`, `--motion`, `--av-marker`, `--av-sync`). They are in the [capture and export engine](recording/capture-and-export.md) page, in the paragraph that starts "Lab diagnostics:".
