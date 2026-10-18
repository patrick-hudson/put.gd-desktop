---
title: Screen recording (Linux first, Windows path) - Plan and decision record
type: feat
date: 2026-10-17
issue: https://github.com/patrick-hudson/put.gd-desktop/issues/50
execution: code
status: design approved 2026-10-17; slices 1-3.3 implemented (contract, helper, X11 capture/export, overlays, review window); Cinnamon run, Wayland and tray pending
---

# Screen recording

## Goal

Add recording to the capture → private review → explicit Upload → existing
share workflow, as issue #50 specifies: monitor, combined desktop, region
(including cross-monitor) and selected window; MP4 (H.264 baseline, with H.265
and VP9 as selectable codecs), animated GIF and animated WebP for every mode;
a control overlay with Start/Pause/Resume/Stop/Cancel and system-audio
inclusion; drag → ready bounds → Record → outside dimming; Stop always opens a
private preview with per-rectangle timed blur and start/end trim; nothing
uploads before the separate Upload action; everything reachable through
session methods, `putgd-cli` and the MCP helper.

This file is the decision record the issue asks for. Sections marked
**proposed** are engineering judgments awaiting Patrick's answer; sections
marked **measured** come from the receipts under
`work/verification/2026-10-17-recording-probe/` (private, not acceptance).

## Inventory (measured 2026-10-17)

| Target | Session | GPU / VA-API | GStreamer | H.264 encoders | FFmpeg | Portal ScreenCast |
| --- | --- | --- | --- | --- | --- | --- |
| Workstation, Mint 22.3 Cinnamon 6.6.4 | X11, three monitors (1920x1080 + 2x 2560x1440, mixed origins) | UHD 630 + Quadro T2000 | 1.24.2 (base/good/bad/ugly/libav/vaapi/pipewire installed) | x264enc, openh264enc; `va` plugin absent | not installed; 6.1.1 available | 1.20 (xapp/gtk backends) |
| Fedora 44 GNOME 50 (`.70`) | Wayland | HD 530, iHD 25.4.6 **without H.264 encode entrypoints** (stock Fedora driver) | 1.28.8 (dev installed) | openh264enc only; no x264, no va H.264 | ffmpeg-free 8.0.1: libopenh264, vp9/av1, hevc_vaapi listed but no HW profile | v5, monitor+window+virtual |
| Debian 13 Plasma 6.3 (`.69`) | Wayland | HD 530, iHD 25.2.3 with H.264 EncSliceLP | 1.26.2 (dev installed) | x264enc, openh264enc, vah264lpenc, vaapih264enc | 7.1.5 (installed now): libx264, h264_vaapi, libx265, libvpx-vp9 | v5, monitor+window |
| Kubuntu 26.04 Plasma 6.6.6 (`.72`) | X11 | HD 530, iHD 26.1.2 with H.264 EncSliceLP | 1.28.2 (dev installed) | x264enc, openh264enc, vah264lpenc, vaapih264enc | 8.0.1: libx264, h264_vaapi, libx265, libvpx-vp9 | v5 (gtk/kde backends; X11 session) |

Common to all four: `pipewiresrc`, `ximagesrc`, `pulsesrc`, `mp4mux`/`qtmux`,
`webmmux`/`matroskamux`, `compositor`, `videocrop`, `gaussianblur`, `videorate`,
`webpenc` with `animated=true`, `vp9enc` (libvpx), `av1enc`/`svtav1enc`, an AAC
encoder (`avenc_aac` everywhere, `voaacenc` on Debian/Ubuntu, `fdkaacenc` on
Fedora). **No GStreamer GIF encoder is packaged anywhere** (`gifenc` is in
gst-plugins-rs and not shipped). `x265enc` is packaged everywhere except stock
Fedora. No lab GPU encodes HEVC, VP9 or AV1 in hardware (Skylake/Coffee Lake
Intel; the workstation's NVIDIA T2000 could do NVENC but no `nvcodec` plugin or
`ffmpeg` is installed there yet). FFmpeg has no PipeWire input device in any of
these builds; `kmsgrab` needs DRM master. `libportal-dev` 0.9/0.10 is packaged
on Debian/Fedora; the GNOME adapter already talks D-Bus through QtDBus, so the
portal can be driven without libportal.

**Floors implied by the matrix:** GStreamer ≥ 1.24 (Mint 22.3), FFmpeg
libraries ≥ 6.1 (Mint 22.3), PipeWire ≥ 1.0, portal ScreenCast v4+ (v5
observed everywhere).

## First X11 probe (measured on `.72`, Plasma X11, i5-6500T, 6-7 s runs)

Region 1280x720 at 30 fps from `ximagesrc` unless stated; CPU is user+sys
seconds divided by wall seconds, i.e. fraction of one core; RSS is `time -v`
peak of the `gst-launch-1.0`/`ffmpeg` process.

| Pipeline | CPU (cores) | Peak RSS | Output |
| --- | ---: | ---: | --- |
| x264enc veryfast zerolatency 4 Mbit/s → mp4mux faststart | 0.73 | 52 MiB | H.264 High 4:2:0, 208 frames |
| openh264enc 4 Mbit/s | 0.42 | 38 MiB | Constrained Baseline |
| vah264lpenc 4 Mbit/s (Intel iHD) | 0.32 | 47 MiB | H.264 High |
| vaapih264enc (legacy plugin) | failed | | `Internal data stream error` from ximagesrc; use the `va` plugin |
| 1920x1080 x264enc 8 Mbit/s | 0.96 | 77 MiB | |
| 1920x1080 vah264lpenc 8 Mbit/s | 0.47 | 53 MiB | |
| 1920x1080 openh264enc | 0.49 | 55 MiB | |
| x264 720p + `pulsesrc` on the default sink monitor → voaacenc 128 kbit/s, one mp4mux | 0.79 | 54 MiB | h264 + aac LC 48 kHz stereo, one file |
| webpenc animated, 10 fps 640x360, direct from capture | 0.12 | 28 MiB | animated WebP, loop 0 (webpinfo) |
| ffmpeg x11grab + libx264 veryfast | 0.79 | 92 MiB | |
| ffmpeg export from the x264 clip: `-ss 1 -to 4`, boxblur 0.5-2 s, palettegen/paletteuse 10 fps 640 px → GIF | 0.7 s wall | 126 MiB | 30 frames, 3.0 s |
| ffmpeg export: same trim, libwebp_anim 10 fps 640 px | 0.2 s wall | 98 MiB | animated WebP |
| ffmpeg export: same trim + boxblur, libx264 → MP4 faststart | 0.7 s wall | 230 MiB | 90 frames, 3.000 s |

Observations: VA-API H.264 roughly halves CPU against x264 on this box;
OpenH264 sits between them but only produces Constrained Baseline. Export
passes for a short clip are sub-second. `ffprobe` cannot read animated WebP
(FFmpeg has no animated WebP decoder); verification must use `webpinfo` or
libwebp's demux API. `nb_frames`/`r_frame_rate` from VA/OpenH264 runs show
timestamp quirks (240/1, 120/1 reported rates) that a real helper must fix by
owning timestamps rather than trusting `ximagesrc` + muxer defaults.

Not measured yet: Wayland portal capture on GNOME/KDE (needs an interactive
consent dialog on the wall), cross-monitor composition, pause/resume, long
runs/drift, GSR, OBS, Windows.

## Decisions

### D1. Process boundary: one transient `putgd-record` helper (**proposed**)

A new Qt Widgets executable, launched by the resident exactly like
`putgd-capture`/`putgd-kde-region`/`putgd-ui --edit-image`
(`CaptureProcessController` / `EditorJob::supervise` pattern: bootstrap with
build manifest + nonce + generation, `ready` must echo the build, gapless
newline-delimited JSON frames on stdio, normal zero exit required, pinned
staging directory with inode/digest re-verification). It owns:

- the region ready overlay and dimming, the floating control overlay;
- the capture pipeline (GStreamer) and the private working clip;
- the preview/edit window (scrubber, trim handles, blur rectangles with
  per-rectangle intervals), playing the working clip through the same edit
  model the export uses;
- the export engine (FFmpeg libav* in-process) producing the final MP4/GIF/WebP;
- nothing else: no settings, providers, credentials, history or network.

The resident keeps: durable intent (operation journal checkpoint before any
frame is retained), the one-active-recording rule (shares `selectingId_`/
`editingId_` style ownership with the still editor: a recording and an image
editor never coexist), session methods, and the hand-off of the final file into
`ArtifactStore` as a new payload kind `video` (streamed from the private staged
path, explicit mime, `originalFile=false`, new `naming.recordingTemplate`
defaulting to `Screen-Recording-{yyyy}-{MM}-{dd}-{HH}-{mm}-{ss}.{ext}`).
Restart turns `recording`/`reviewing` into `interrupted` and discards staging,
as the editor does (issue review item E, rank 1).

Why one helper rather than capture helper + separate export worker: the
preview must play the same bytes the export reads, the edit model lives in one
place, and a single process family is what the memory protocol charges.
Why not libraries in the resident: the resident is a `QCoreApplication`
listener with a 40 MiB quiet budget; GStreamer registry scanning alone
breaks that.

### D2. Backend: GStreamer acquisition + FFmpeg libav* export (**proposed**, issue rank 1)

- **Acquisition** in GStreamer: `pipewiresrc fd=… path=…` from a ScreenCast
  portal session (GNOME/KDE Wayland, driven over QtDBus like the GNOME
  adapter), `ximagesrc` per monitor on X11 (Cinnamon, Plasma X11), `compositor`
  for combined desktop and cross-monitor regions with explicit source-to-canvas
  geometry from the existing `CaptureFrame`/`KdeOutputs`/RandR identities,
  `videocrop` for regions, `pulsesrc device=<default-sink>.monitor` (works on
  PipeWire-Pulse and PulseAudio) for system audio, one shared pipeline clock,
  pause implemented as a valve + timestamp rebasing (not `PAUSED` state,
  because live sources stop producing and EOS-on-paused is undefined).
- **Working clip**: H.264 in a recoverable Matroska file, encoder chosen at
  Record time in this order and disclosed in capabilities: `vah264lpenc`/
  `vah264enc` → `x264enc` → `openh264enc`; AAC audio when system audio is on.
  Chosen so the working clip is always decodable by the FFmpeg export side and
  cheap on the CPU where hardware exists. Fedora stock gets OpenH264.
- **Export/preview** through FFmpeg libraries (libavformat/libavcodec/
  libavfilter/libswscale) linked into the helper: trim with exact re-encode
  when the cut is not on a keyframe, `boxblur`/`gblur` with `enable=between(t,…)`
  timeline expressions built from validated numbers (never agent strings),
  `palettegen`+`paletteuse` for GIF, `libwebp_anim` for WebP, `libx264`/
  `h264_vaapi` for MP4/H.264, `libx265` for MP4/H.265 where packaged,
  `libvpx-vp9` for WebM/VP9, `movflags=+faststart`. This is the only route
  that gives GIF on every target and a single filter graph for preview and
  export parity.
- **GStreamer-only (issue rank 3)** is rejected for export because no GIF
  encoder is packaged. **FFmpeg-only (rank 2)** is rejected for capture
  because distro FFmpeg has no PipeWire input and `kmsgrab` needs DRM master.
  **GSR** stays a Linux performance challenger to benchmark later; it adds a
  GPL-3 process and still needs the same export engine. **OBS** is a
  reference only.

### D3. Codec and format policy (**proposed**)

| Output | Container / codec | Audio | Browser note |
| --- | --- | --- | --- |
| MP4 (default) | H.264 High or Constrained Baseline, yuv420p, faststart | AAC LC optional | Plays everywhere |
| MP4 H.265 | HEVC Main (`libx265` software; hardware only where VA/NVENC exists) | AAC LC | Chrome/Edge need platform HEVC decoding, Firefox limited; disclose in the picker |
| WebM VP9 | VP9 (libvpx, `deadline=realtime` not needed since export is offline) | Opus | Chrome/Firefox; Safari 14.1+ |
| GIF | palette per clip (`stats_mode=diff`), ≤ 15 fps, max edge configurable | none | |
| Animated WebP | lossy quality 70 default, loop forever | none | |

H.265 and VP9 are export-time transcodes of the H.264 working clip, not live
capture codecs, because none of the lab GPUs encodes them and software x265/
libvpx cannot keep 1080p30 real time on an i5-6500T. Live capture stays H.264.
Duration/size: stop automatically when the working clip reaches the admitted
`maxFileBytes` minus a finalization reserve; GIF/WebP additionally cap duration
(proposed 60 s) and dimensions (proposed 1280 px long edge) with explicit
errors, never silent downscaling.

### D4. Edit model (**proposed**, issue review item D)

- Time unit: integer microseconds on the recorded-clip timeline (pauses already
  removed), half-open intervals `[start, end)`, rounded to the nearest source
  frame boundary at export, boundary rule documented in `docs/recording.md`.
- Canvas: fixed output canvas chosen at Record (region size, monitor size, or
  the composed desktop bounds); rectangles in canvas pixels; later monitor
  topology changes end the recording with `source_changed`, they never move
  the canvas.
- `EditRevision`: every mutation returns a new monotonically increasing
  revision; `preview`, `saveCopy` and `upload` take the revision they were
  built from and are rejected as `edit_stale` when it moved.
- Mask: `{id, rect, start, end, kind: blur|solid}`; `solid` is the opaque
  cover the issue leaves open (same model, trivial to add; waiting on Q3).
- Trim: one contiguous `[in, out)`; masks are stored in source time and
  intersected/rebased at export exactly as the issue's 10-30 s example states.

### D5. Session, CLI and MCP contract (**proposed**)

Session methods (`LocalServer`), all forwarded unchanged by `putgd-cli`:

| Method | Params | Effect |
| --- | --- | --- |
| `recording.capabilities` | | sources, encoders, formats, audio sources, overlay exclusion per mode; no capture, no portal prompt |
| `recording.prepare` | `{target: monitor|desktop|region|window, identity?, rect?, format, codec?, audio: {system: bool}, fps?}` | durable intent, overlay/ready bounds shown, portal consent may be requested; returns `{recordingId, operationId, state: ready}` |
| `recording.start` / `.pause` / `.resume` / `.stop` / `.cancel` | `{id}` | acknowledged after the helper reaches the state; duplicates are no-ops with the current state |
| `recording.inspect` | `{id}` | state, retained duration, bytes, drops, audio state, overlay visibility |
| `recording.raise` / `recording.overlay` | `{id, visible?}` | overlay control; the recording indicator stays visible |
| `recording.edit` | `{id, revision, commands: [...]}` | `setTrim`, `resetTrim`, `addMask`, `updateMask`, `removeMask`, `setFormat` |
| `recording.preview` | `{id, time, maxEdge?}` | one PNG frame at clip time with current trim/masks applied, plus the frame's clip timestamp; same-user explicit request only |
| `recording.saveCopy` | `{id, path, overwrite?}` | exports locally, no upload |
| `recording.upload` | `{id, revision}` | exports, validates, admits to `ArtifactStore`, publishes through the existing operation; returns the linked operation |

MCP tools mirror the editor set: `get_recording_capabilities`,
`prepare_recording`, `control_recording` (start/pause/resume/stop/cancel),
`get_recording`, `edit_recording`, `preview_recording` (MCP image),
`save_recording_copy`, `upload_recording`, `raise_recording`. The Python
helper validates shapes only; the resident validates semantics.

### D6. Windows path (**proposed**, not built)

Same helper, same control protocol and export engine. Acquisition via
GStreamer `d3d11screencapturesrc capture-api=wgc` (monitor/window handles,
crop) and `wasapi2src loopback=true`; H.264 through `nvd3d11h264enc`/`qsvh264enc`/
Media Foundation where present, else `openh264enc`/`x264enc`. The physical
Windows 11 box on the wall (`.71`, `W11-MJ04ZP61`) has no working remote
execution yet (SSH timed out, WinRM rejected on 2026-10-17); Windows work needs
that fixed first. Windows 11 is the primary target; Windows 10 stays "evaluate".

## Dependency-ordered slices

1. **Contract and model** (no media): `src/core/RecordingSession.*` (state
   machine, edit model, revisions, time math with unit tests), session methods
   and CLI forwarding, MCP tool schemas + synthetic-peer tests, settings/schema
   additions (`actions.record`, `naming.recordingTemplate`, `limits.recording*`),
   `ArtifactStore` `video` payload with naming and category tests.
2. **Helper skeleton**: `putgd-record` with bootstrap/ready/identity, CMake
   target, package dependencies, CheckBuildIdentity entry; GStreamer and FFmpeg
   found via pkg-config; `recording.capabilities` truthfully enumerates
   elements/encoders.
3. **X11 slice on the workstation Cinnamon desktop**: region ready overlay
   (reusing `NativeRegionOverlay` geometry), dimming, control overlay,
   `ximagesrc` capture, H.264 working clip, pause/resume, Stop → preview → trim/
   blur → MP4/GIF/WebP export → explicit Upload into the existing pipeline.
   Cinnamon functional and memory gates here, before anything else expands.
4. **System audio** (`pulsesrc` monitor), A/V sync checks with a synthetic
   marker clip, speakers and headphones.
5. **Multi-monitor**: combined desktop and cross-seam regions on the
   workstation's three mixed monitors; window capture on X11.
6. **Wayland**: portal session on GNOME (`.70`) and KDE (`.69`), `pipewiresrc`,
   region crop, overlay strategy per compositor (KDE layer-shell reuse; GNOME
   through the existing Shell bridge or a disclosed gap), window capture.
7. **H.265 / VP9 export**, Fedora OpenH264-only path, packaging (DEB/RPM
   dependencies, AppImage runtime), docs (`docs/recording.md`, `docs/mcp.md`,
   configuration, acceptance matrix rows), full MCP end-to-end on each desktop,
   browser playback of real share URLs.

## Decisions taken 2026-10-17

Patrick approved D1 and D2 (GStreamer capture plus FFmpeg libav export in one
transient `putgd-record` helper) and asked for H.265 and VP9 as selectable
codecs. The remaining recommendations were adopted as working assumptions:
H.264 is the only live capture codec with H.265/VP9 as export transcodes (VP9
in WebM); masks are `blur` or `solid`; the microphone stays reserved and
refused; animated exports default to 60 s and 1280 px with explicit
`maxEdge` downscaling; Windows stays a design path until the Windows box has
remote execution; the Cinnamon slice runs on Patrick's workstation desktop
with a Mint-compatible package built in the Cinnamon VM on the Fedora
builder, local build only as fallback.

## Slice status

1. Contract and model: implemented (`docs/recording.md`), tested with a fake
   backend; `recording.capabilities` reports `available: false` in production.
2. Helper skeleton: implemented. `putgd-record` (GStreamer + FFmpeg linked,
   `--capabilities` probe, UiControl handshake and command/event framing) and
   `RecordingHelperBackend` in the resident; capture commands are refused as
   `capture_not_implemented`. AppImage builds without the helper for now.
3. X11 slice: 3.1 (capture, pause/resume, preview, export, upload through the
   CLI) implemented and exercised on the Kubuntu Plasma X11 desktop for all
   four formats; 3.2 (drag selection, ready ring, dimming, control bar with
   helper events) implemented and exercised on the same desktop with saved
   screenshots; 3.3 (review window with scrubber, trim, timed blur/solid
   masks, format choice, Save copy and Upload, round-tripping edits through
   the resident) implemented and exercised on the same desktop.
4. System audio: captured and exported for MP4 (AAC) on the Plasma desktop;
   speaker/headphone and synthetic A/V-sync checks pending.
