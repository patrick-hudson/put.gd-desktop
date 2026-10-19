# Wayland recording A/V sync (#86) — 2026-10-19

Issue #86: Wayland recordings with system audio played the sound 30–53 ms ahead
of the picture. Branch `claude/wayland-av-sync` (pull request into `nightly`);
the behaviour is described in [recording](../recording.md), "Timestamps and
A/V sync".

## Cause

Two separate faults, both measured on the physical desktops:

1. **Export (the main one).** A Wayland working clip's audio starts 20–110 ms
   after its first video frame (the sink monitor opens after the screen stream;
   X11 clips start both at 0). `ClipExporter` trimmed audio from the audio
   stream's own start and rebased it to zero, so every MP4/WebM played the
   sound early by exactly that gap. On every run below, the old export's error
   equals the working clip's audio start time.
2. **PipeWire video timestamps.** `pipewiresrc` puts the compositor's header
   PTS on every frame, so `do-timestamp=true` never applies, and `GstBaseSrc`'s
   live sync then adds one offset to all frames: the first frame's arrival
   delay. The video track was late by 0–90 ms depending on the run (KWin), and
   the pipeline ran on `GstPipeWireClock`.

## Fix

- Export: audio is cut from the same clip-timeline instants as the video, and
  the gap before its first sample becomes one silent frame fed into the
  existing filter graph (no new FFmpeg filters for the AppImage) (12abbb9).
- Capture: a private `pipewiresrc` subclass drops the base-class sync, the
  pipeline runs on the monotonic system clock, and each frame is stamped on
  arrival at the source pad (0500748). The compositor's header times were tried
  first and rejected: KWin's is its previous presentation, about 40 ms ahead of
  the frame, which made Plasma recordings +35..+48 ms. Lab modes
  `PUTGD_RECORD_PIPEWIRE_TIMESTAMPS=compositor|legacy` keep both for A/B runs.

## Method

`putgd-record --av-marker` (black 1280×720, a 100 ms white flash and a 1 kHz
burst at each whole second) played full screen through the built-in card's
speakers (`alsa_output.pci-0000_00_1f.3.analog-stereo`, port
`analog-output-speaker`, 40%), recorded at 30 fps with system audio (monitor
target; GNOME portal: the player's window), then `--av-sync` on the working
clip and on the MP4 that `putgd-record --export` writes the way the review
does. Offsets are audio minus video, positive when the sound is late; one frame
is 33.3 ms. The recorder was driven over its control pipes by a lab driver
(no resident), scratch builds under `~/work/put.gd-avsync` on each builder,
desktop locks held for every run, input through `tools/lab/host_input.py`.
Players: GStreamer (`matroskademux ! jpegdec ! waylandsink fullscreen=true` /
`xvimagesink`, `pulsesink`), the common player across all three desktops, plus
`ffplay -fs` (the old X11 baseline's player) where it runs; ffplay doesn't play
on Fedora GNOME Wayland (1 pair, video frozen). The first pair of a run is
often one frame off while the player starts.

Desktops: d69 Debian 13 Plasma 6.3 Wayland (KWin, PipeWire 1.4.2, `vah264lpenc`),
f44 Fedora 44 GNOME 50 Wayland (Mutter, PipeWire 1.6.9, `openh264enc`), k72
Kubuntu 26.04 Plasma 6.6 X11 (PipeWire 1.6.2, `vah264lpenc`). Host load (1 min)
during the runs: 0.12–1.16; no other jobs ran on the host in the slot.

## Results (exported MP4 medians unless noted, ms)

Final build `0500748` (both fixes, the default):

| Desktop / path | Player | Working clip | Exported MP4 | Audio start | Load |
| --- | --- | --- | --- | --- | --- |
| d69 `kde-screencast` | GStreamer | −9.0 | −8.7 | 100 ms | 0.12 |
| d69 `kde-screencast` | GStreamer | +5.0 | +4.7 | 20 ms | 0.15 |
| d69 `kde-screencast` | ffplay | +19.0 | +18.7 | 10 ms | 0.17 |
| d69 portal (monitor) | GStreamer | +7.0 | +7.3 | 100 ms | 0.86 |
| d69 portal (monitor) | GStreamer | −10.7 | −10.7 | 110 ms | 0.44 |
| d69 portal (monitor) | GStreamer | −5.0 | −4.7 | 110 ms | 0.04 |
| f44 `mutter-screencast` | GStreamer | +3.0 | +3.3 | 90 ms | 0.56 |
| f44 `mutter-screencast` | GStreamer | −6.0 | −6.0 | 90 ms | 0.62 |
| f44 `mutter-screencast` | GStreamer | −3.0 | −2.7 | 90 ms | 0.71 |
| f44 portal (window) | GStreamer | −5.0 | −4.7 | 60 ms | 1.12 |
| f44 portal (window) | GStreamer | +6.0 | +6.0 | 60 ms | 0.06 |
| f44 portal (window) | GStreamer | −4.0 | −4.0 | 50 ms | 0.49 |

Per path (GStreamer player, exported MP4): `kde-screencast` n=2, medians −8.7
and +4.7 (range 13.4); d69 portal n=3, −10.7..+7.3 (range 18.0); Mutter n=3,
−6.0..+3.3 (range 9.3); GNOME portal n=3, −4.7..+6.0 (range 10.7); k72 X11
n=2, −18.0 and −12.0 (range 6.0). Within a run, pairs after the first agree
exactly or differ by one frame (33.3 ms); the first pair is up to three
frames off while the player starts.

Before (nightly behaviour: first-frame-anchored video, old export), same
desktops, scratch build `81d42ab` in `legacy` mode for capture; d69 MP4s from
the old exporter at capture time, f44 and k72 MP4s re-exported from the kept
working clips with nightly's `ClipExporter.cpp`:

| Desktop / path | Player | Working clip | Exported MP4 | Audio start |
| --- | --- | --- | --- | --- |
| d69 `kde-screencast` | GStreamer | +13.0 | −86.7 | 100 ms |
| d69 `kde-screencast` | GStreamer | −15.0 | −25.3 | 10 ms |
| d69 portal (monitor) | GStreamer | −2.7 | −112.7 | 110 ms |
| f44 `mutter-screencast` | GStreamer | −3.0 | −92.7 | 90 ms |
| f44 portal (window) | GStreamer | −4.3 | −54.0 | 50 ms |
| k72 X11 | GStreamer | −12.0 / −18.0 | −12.0 / −18.0 | 0 |
| k72 X11 | ffplay | +7.0 | +7.3 | 0 |

The issue's d69 speaker clip (`avsync-speakers.mp4`, −52.7 ms) re-measures the
same with this branch's `--av-sync`. X11 is unchanged by both fixes (its audio
starts at 0 and `ximagesrc` already stamps on capture): k72 at `102f00a`
read −12.0 and −18.0 (GStreamer) and +7.3 (ffplay), working clip and MP4 alike.

Timestamp A/B on the same build (`102f00a`/`81d42ab` lab modes, fixed export,
GStreamer player unless noted):

| Path | legacy | compositor header times | arrival (now default) |
| --- | --- | --- | --- |
| d69 `kde-screencast` | +13.3, −15.3; ffplay +40 | +42, +34.7, +38, +48; ffplay +68 | −1.3, −1.3; ffplay +2.7 |
| d69 portal | −2.7 | +18.7; ffplay +62 | +7.3 (final) |
| f44 `mutter-screencast` | −2.7 | −2.7 | −2.7 |
| f44 portal (window) | −4.0 | — | −8.0 |

Frame age (arrival minus the compositor's header time, min/mean/max) from the
helper's lab log: KWin 0/36–42/60–98 ms, Mutter 5/8/75–94 ms, GNOME portal
window 7/11/39–42 ms. Re-exporting all 14 kept d69 working clips with the fixed
exporter gave MP4 medians within 0.6 ms of their working clips (before: off by
the audio start, 10–110 ms).

## Unchanged exports

`test_export_av_sync` (new) writes the marker with its audio starting 110 ms
late and exports it: MP4 and WebM, whole and trimmed by 1 s, read within half a
frame (nightly's exporter: −110 and −109.5 ms, 4 failures); MP4 without sound,
GIF and WebP export with no audio stream. On k72, a no-audio MP4, a GIF, a
WebP and a WebM exported from the k72 X11 and f44 Mutter working clips by
nightly's and the fixed exporter have identical video frames (`framemd5`;
WebP byte-identical). Only the audio track of an export with sound changes.

## Visuals

Frames from the wall VNC of each desktop during recording (the marker is black
between flashes), with host, session and build in the name:
[d69 KDE screencast](wayland-av-sync/d69-plasma63-wayland-kde-screencast-recording-0500748.png),
[d69 portal picker](wayland-av-sync/d69-plasma63-wayland-portal-picker-81d42ab.png),
[d69 portal recording](wayland-av-sync/d69-plasma63-wayland-portal-recording-0500748.png),
[f44 Mutter recording](wayland-av-sync/f44-gnome50-wayland-mutter-recording-0500748.png),
[f44 portal window recording with the bar](wayland-av-sync/f44-gnome50-wayland-portal-window-recording-0500748.png),
[k72 X11 recording](wayland-av-sync/k72-plasma66-x11-recording-102f00a.png).
Motion: every clip paired 14–15 flashes with bursts over 14–17 s; `--motion`
read 10.1–96.9 on the monitor working clips (the bar and overlays are in some), 0.000 on the
GNOME window clips, where the eight sampled frames all fall between flashes.

Raw driver reports (`*.json`, `*.out`, helper debug logs) are private under
`work/avsync86/runs-{69,70,72}` on the workstation; clips stay in
`~/putgd-lab/avsync86` on each builder.

## Lab changes, undone

d69: `~/.local/share/applications/putgd-record-avsync86.desktop` naming the
scratch helper (KWin screencast permission; Patrick's "Yes, temporarily"),
19:56–20:05Z and 20:21–20:23Z, `kbuildsycoca6` after each change. The helper's
portal token file (`~/.local/share/putgd`) was removed on d69 and f44; the
GNOME picker's "Remember" and the final KDE picker's "Allow restoring" were
unticked. Input agents were stopped and every desktop released
`--verified-clean`.

## Not covered

- Headphone or Bluetooth sinks, multi-monitor, and pause/resume across a gap.
- `ffplay` on GNOME Wayland (it doesn't play there).
- The AppImage build; this ran distro-linked scratch builds.
- One Opus packet in every WebM export fails to decode in FFmpeg's CLI with
  nightly's exporter and this one alike (pre-existing, not this change).
