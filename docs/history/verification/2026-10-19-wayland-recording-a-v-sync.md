# Verification record

> **Historical record.** This is a dated run record, moved unchanged from [implementation evidence](../../testing/verification.md) on 2026-11-05 (#1222). It describes what that run found for the build it names, and its open or pending statements describe that time. "Above" and "below" refer to its neighbours on the old page, in the order of the [history index](../README.md#verification-run-records). For today, see the [current state](../../testing/verification.md#current-state). Do not use it as a work list.

## Wayland recording A/V sync — 2026-10-19

The [A/V sync receipt](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-19-wayland-av-sync.md) records #86 on
`claude/wayland-av-sync`, final build `0500748` (scratch builds on each desktop's
builder), measured with the `--av-marker`/`--av-sync` check through the built-in
speakers with a GStreamer player on every desktop.

- Cause: the export rebased audio to its own first sample, though a Wayland
  working clip's audio starts 20–110 ms after its video, so MP4/WebM played the
  sound that much early. PipeWire video was also anchored to the first frame's
  arrival delay (0–90 ms per run on KWin).
- Exported MP4 medians, before → after (n runs): d69 Plasma 6.3 `kde-screencast`
  −86.7 / −25.3 → −8.7 / +4.7 ms (n=2; ffplay +18.7), d69 portal −112.7 →
  −10.7..+7.3 (n=3), f44 GNOME 50 `mutter-screencast` −92.7 → −6.0..+3.3 (n=3),
  f44 portal (window) −54.0 → −4.7..+6.0 (n=3).
  k72 Kubuntu 26.04 X11 is unchanged at −12.0 / −18.0 (ffplay +7.3). Working
  clips and exports now agree within 0.7 ms.
- `test_export_av_sync`: MP4 and WebM keep a 110 ms audio start (nightly's
  exporter: −110 ms); silent MP4, GIF and WebP carry no audio, and their frames
  match nightly's exporter.
- Not covered: headphone or Bluetooth sinks, multi-monitor, the AppImage build
  (tracked in #87), and ffplay on GNOME Wayland (it doesn't play there).
  Follow-ups: #119 (an undecodable Opus packet in WebM exports, pre-existing),
  #120 (the bar's System audio box on GNOME).
