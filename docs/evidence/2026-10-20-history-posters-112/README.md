# History poster frames (#112): lab evidence

Taken 2026-10-20 ~06:45Z on f44 (Fedora 44, GNOME 50 Wayland, Qt 6.11.2, FFmpeg 8) in the physical
desktop session, from a scratch lab build of `feat/history-video-previews-112` (217d236) merged with
`feat/appearance-theme-60` (dcaa984 + the view-state allow-list fix), run with private config, data
and runtime folders.

- **The recording** is real: `putgd-cli recording.prepare/start/stop/edit/upload` against the lab
  build on a private Xvfb display (1280×720, only put.gd's own window and a test pattern on it), 4 s,
  with a solid cover mask over the top-left quarter added in review, saved locally. The resident then
  made the poster through `putgd-record --poster`.
- **Before** is the same build and history with the poster file removed, which is exactly what nightly
  stores for every video (no preview); the no-preview rendering is unchanged by #192.
- **Capture**: frames from the host's loopback VNC tile, read and cropped on the host. A before/after
  diff isolates put.gd's window; the crop is its client area, kept only when its border (≥96.8%) and
  area (≥97%) are the theme's token colours. Full frames were deleted on the host unopened.
- Annotations (boxes, arrows, labels) were added afterwards with Pillow.

| File | Shows |
| --- | --- |
| `history-poster-before-after.png` | The History row before (file-type icon) and after (poster, play glyph, length) |
| `poster-keeps-mask.png` | The 256 px poster itself, 3× nearest-neighbour: the export's cover mask is in it |
