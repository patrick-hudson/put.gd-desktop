# Screen recording: capture and export engine

Part of [Screen recording](../recording.md).

## Capture and export engine (X11)

`putgd-record` resolves the target against the current RandR layout
(`X11Targets`): monitors by output name, the desktop as the root window, a
region as root coordinates inside the root, a window by decimal or `0x` XID
(captured through `ximagesrc xid=`, so it keeps its identity when moved).
Dimensions shrink to even values rather than growing into unselected pixels.

The live pipeline is `ximagesrc → queue (leaky) → videoconvert → videorate →
queue → H.264 encoder → h264parse → matroskamux` with `pulsesrc
device=@DEFAULT_MONITOR@ → AAC` when system audio is requested (#6, PR #981).
Capture, conversion and encoding run in their own threads: the capture queue
leaks its oldest frame rather than block the source when the stages behind it
fall behind, `videorate` restores the constant rate, and a frame the queue
had to leak is counted (`framesDropped` in `status()`, `frames:` in the debug
log). `videorate` also matters on X11 because ximagesrc skips frame numbers
when a grab runs late and the `va` encoders stamp their output contiguously
regardless: without it a 10 s recording on a three-monitor desktop played in
7.4 s. x264 runs frame threads with a short lookahead and a preset chosen by
pixel rate (veryfast up to about 1080p30, superfast to 1440p30 or 1080p60,
ultrafast above); zerolatency's two sliced threads stalled the capture at
1080p. The CPU stages take about two thirds of the cores, bounded, so the
desktop being recorded keeps its own. Encoders are tried in order `vah264lpenc, vah264enc,
vaapih264enc, x264enc, openh264enc` (AAC: `avenc_aac, fdkaacenc, voaacenc`)
by actually starting the pipeline, GPU first, and the chosen names are
reported in `recording.inspect` progress. The VA encoders take NV12, the CPU
encoders I420. Bitrate for the bitrate-driven encoders is about 0.1 bit per
pixel per frame, bounded to 1.5–20 Mbit/s; the legacy `vaapih264enc` runs
at constant QP 24 because Intel's iHD driver refuses CBR and VBR for it
(that refusal, surfacing as "Internal data stream error", was why it used
to fail on Mint 22.3 and Ubuntu 26.04); on the workstation it captures 720p
at half the CPU of `x264enc veryfast`. Pause drops raw frames and samples on the
encoders' sink pads (never encoded buffers, which would leave P-frames with
missing references that strict decoders such as OpenH264 refuse). Pause and
Resume record one interval of pipeline running time that both branches share
(`src/record/PauseTimeline.cpp`): a frame or audio buffer captured inside it
is dropped (an audio buffer reaching into it is dropped whole), and everything
after it moves back by the paused time, the same for both tracks, so the
working clip's timeline excludes paused time and the audio-minus-video offset
after a resume is the one before it. The interval lasts a whole number of frames (the
nearest to the paused time), so frames after it stay on the frame grid that
the export's `fps` filter samples; off the grid they moved by up to half a
frame in the export while the audio didn't. Before #130 each branch measured its own
gap from its first buffer after the resume and continued one step after its
last kept buffer (a frame for video, a fixed 20 ms for 10 ms audio buffers),
which stepped the offset by −16 to +30 ms and left a 10 ms hole in the
audio. A recording stopped while paused keeps nothing after the pause. The
retained duration is measured from the first retained frame because some
encoders carry a constant timestamp offset.

**Encoder order and the Skylake GPU hangs (#89).** The order stays GPU first.
The lab's Intel HD 530 hosts logged seven i915 hangs between 2026-10-17 and
2026-10-20: six on k72 and one on d69. All of them were `Resetting rcs0 for
preemption time out` with `ecode 9:1:85dffffb`, in the compositor's
context (Xorg or `kwin_wayland`) on the render engine. The video engine that
`vah264lpenc` uses (`vcs0`) never reset. Four of the seven came with no
VA-API encode running. One happened during a CPU GIF export, one at idle with
builds running, and two before any recording work. The other three, in two
events, overlapped a `vah264lpenc` recording at host load 5–9. m74 (the same GPU, Mint X11)
recorded with `vah264lpenc` without a hang, and k72 was taken out of the
lab for a hardware check on 2026-10-20. A software encoder first would cost
every Intel/AMD/NVIDIA user CPU time to work around one host's render-engine
fault, so the order is unchanged. The encoder actually used stays in
`recording.capabilities` (`gstreamer.usableH264Encoders`, the first is used)
and in `recording.inspect` progress. If a GPU's encoder fails at
`prepare`, the probe moves on to the next one. Kernel versions, drivers and
the per-host table are in
[the closeout receipt](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-20-recording-closeout.md#89-gpu-first-encoder-order-kept).

Stop ends the clip at once: from that running time on nothing is kept, however
long the drain takes. It then sends EOS into each branch at the element after
its source, each from its own worker thread, and the clip is finished when the
muxer's EOS reaches the bus. Before #129 the EOS went through the pipeline to
the sources, and a source takes its streaming lock for it; `pulsesrc` holds
that lock while its thread waits, for audio or for room in the audio queue.
On a VM without a sound card the monitor of the suspended `auto_null` sink
sometimes stamps its buffers about 2³² ring-buffer segments (~497 days) in the
future: the muxer never takes them, the audio queue reads as full, the audio
thread blocks, and Stop blocked the helper's main thread until the resident gave
up with `recording_helper_timeout` (2 of 5 stops in the lab's Kubuntu 26.04
guest). The encoder probe now re-anchors audio stamped more than a second ahead
of the running time to its arrival and keeps that correction for the buffers
after it (`AudioClockGuard`; `PUTGD_RECORD_DEBUG_LOG` counts the corrections).
If a drain still doesn't finish within 10 s the capture stops anyway and keeps
what reached the file (its Matroska index is missing; the clusters decode);
`recording_stop_timeout` is left for a drain that wrote nothing. The helper stops on its own and reports `stopped` with reason
`budget` when the working clip nears its budget: `limits.maxFileBytes` minus a
reserve, and at most a third of the free disk above the system reserve (the
recording, its export and its upload copy each need room). `recording.prepare`
refuses with `disk_space_insufficient`, and the tray's Record action shows the
same message, when that third would be under 64 MiB, about a minute of a busy
1080p screen, instead of starting a recording that ends itself within seconds
(#213).
Progress events carry `durationUs`, `bytes`, `paused` and the encoder names.

When the helper stops a capture on its budget, the resident says why (#225). The
reason is `max_file_bytes` when the Largest file limit set the budget, and `disk_space`
when free disk set it or the disk has less than two minimum budgets left after the
capture. The review window shows a banner above the frame, the tray posts
**put.gd — recording stopped** with the same words, and `recording.inspect` returns
them as `stopDetails`, for example: "Recording stopped at 1:42: it reached the Largest
file limit (2 MiB; recording stops at 1 MiB to leave room to finish the file). The clip
so far is kept for review; raise Largest file in Settings > Uploads to record longer."
A disk stop gives the free space and the reserve and asks to free up space before the
next recording. Either way the clip recorded so far stays in review, ready to trim,
save or upload. The resident logs `Recording stopped on its budget` with the figures.

Export runs in the helper with the FFmpeg libraries on the working clip:
`trim` in stream ticks, `setpts` rebase, RGB conversion, per-mask
`crop→avgblur→avgblur→overlay` (two box passes, as the preview draws them;
`boxblur` is GPL-only in FFmpeg) or `drawbox` enabled on `gte(t,a)*lt(t,b)` in output
time, optional `scale` to `maxEdge`, `fps` (at most 15 for animations), then
the first encoder that opens from a preference list: MP4/H.264 tries
`h264_nvenc` (NVIDIA GPU, system-memory frames), then `h264_vaapi` (Intel
and AMD GPUs through libva), then `libx264` (`veryfast`, CRF 23) and
`libopenh264`; H.265 tries `hevc_nvenc`, `hevc_vaapi`, then `libx265`
(`faster`, CRF 26); WebM uses `libvpx-vp9` (`realtime` deadline, `cpu-used`
5, row threads); GIF is a two-pass `palettegen`/`paletteuse`; WebP is
`libwebp_anim`. NVENC is opened with `p4`/`hq`, VBR and a constant quality
target (26 for H.264, 28 for HEVC). VA-API opens a device on the default
render node, then `/dev/dri/renderD128`… in turn (the display GPU may be an
NVIDIA card without a VA driver), allocates an NV12 surface pool, uploads
each filtered frame with `av_hwframe_transfer_data` and encodes with constant
QP (24 for H.264, 26 for HEVC), the one rate control every VA driver offers;
AMD's `radeonsi` and Intel's `iHD` share this path, with no B-frames. An
encoder that does not open (no driver, no entry point, as HEVC on a UHD 630)
is skipped for the next candidate, so other machines keep the CPU path. HEVC
in MP4 is tagged `hvc1` for Apple players. The working clip is decoded with frame and
slice threads on every core. Audio is trimmed with `atrim` in seconds on the
same clip timeline as the video (it starts at the first video frame; a gap
before the first sample becomes silence, see "Timestamps and A/V sync"),
turned from the clip's timestamps into contiguous samples by `aresample`
(`async=1`, 1 ms hard compensation), so a hole in the clip's audio (such as
a pause leaves) becomes silence and an overlap is cut instead of later audio
moving earlier (#130), and encoded as AAC (MP4) or Opus (WebM). MP4 output is written with `faststart`.

Opus packets are 20 ms. The last one carries the end trim as Matroska
`DiscardPadding`, which `ffprobe` shows as `Skip Samples` side data, and
every packet decodes. FFmpeg 8.0.x (Kubuntu 26.04 ships 8.0.1) logs
`[opus] Error parsing Opus packet header.` once at the end of every Matroska
or WebM Opus stream. That includes files written by the `ffmpeg` CLI and
ones without padding. The message comes from the demuxer's Opus parser, which
checks the empty flush buffer at EOF, and the decoded audio is unchanged. It
came in with FFmpeg commit 37155d68ec and was fixed in 8.1 by 618fc15e65,
not backported to 8.0. FFmpeg 7.1 (Debian 13) and 8.1 (Fedora 44) print
nothing (#119). `export_av_sync_contract` decodes every audio packet of WebM
exports with the native and libopus decoders.

On the workstation (i9-9880H, UHD 630, Quadro T2000, 60 s 2238×1318 clip
with audio, `putgd-record --export`): H.264 and H.265 on NVENC export at
about 3.9× real time (15.6 s), H.264 on the Intel iGPU through VA-API at
2.6× (23.5 s), `libx264` at 2.1×, VP9 at 1.7×, a 10 s GIF at 0.9×; `libx265`
stays below real time (0.68× at `faster`), which is why the GPU paths come
first. On an AMD Radeon 740M (Aurora Fedora 45, Plasma 6.8.2 Wayland, Mesa 26.2.6 from
negativo17), live H.264 used `vah264enc`, and H.264 and H.265 exports used
`h264_vaapi` and `hevc_vaapi`, with the GPU's encode engine counters confirming
the work (#138). Fedora's stock Mesa on AMD has not been tested. H.265 links do not play in Firefox or Chrome on Linux; both format
menus say so in the item tooltip and the review status, and the default
stays H.264. Each decode pass
opens its own demuxer and verifies that a backward seek did not overshoot the
trim start (Matroska clips whose first cluster has no cue point otherwise
start at the next keyframe). Preview decodes the frame at or after the
requested time and applies the masks active at that source time with a box
blur or a solid fill, so preview and export agree on timing and placement.

Selectors (the X11 native overlay, the Qt fallback, the KDE Wayland layer
surfaces and the recorder's own) repaint only the outline strips and the pills
of the previous and the next state, and the native overlay presents at most
once per display refresh (#6). `PUTGD_REGION_DEBUG_LOG=<file>` appends one
line per selection with the pointer moves, paints or presents and their
timing. `tools/lab/fluidity.py` and `tools/lab/fluidity_run.py` measure a
recording's distinct frames per second and a drag's cost per build on a
physical desktop ([receipt](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-31-fluidity-campaign.md)).

Lab diagnostics: `PUTGD_RECORD_DEBUG_LOG=<file>` appends pipeline and command
traces, `PUTGD_RECORD_KEEP_WORKING=<dir>` keeps a copy of each working clip,
`putgd-record --export CLIP OUT.ext [--codec h264|h265|vp9] [--max-edge N]
[--trim startUs endUs] [--no-audio]` exports a kept clip the way the review
does and prints the encoder, frame count, bytes and wall time, and
`PUTGD_RECORD_EXPORT_ENCODERS=<comma list>` replaces the encoder preference
order for such timing runs; `putgd-record --motion CLIP [SAMPLES]` prints the
mean frame difference between frames sampled across a clip (about 0.01 for a
still picture), so a recording test can require that it captured movement.
For system audio, `putgd-record --av-marker OUT.mkv SECONDS` writes a
synthetic marker clip (MJPEG and PCM in Matroska, built-in FFmpeg encoders
only): black 1280×720 frames with a white flash and a 1 kHz tone burst, both
100 ms, at the start of every second. Play it in the recorded area with
system audio on, then `putgd-record --av-sync CLIP` decodes the recording in
one demux pass, finds the flash onsets (mean luma rising edges) and burst
onsets (2 ms RMS envelope), pairs each flash with the nearest burst within
half a period and prints JSON with every offset (`offsetsMs`, audio minus
video, positive when the sound is late), `medianMs` and `spreadMs`; the
marker itself measures 0 ms. None is read by the resident and none carries
frames.

The AppImage's own helper runs all of these on its working clips and on saved
MP4 (H.264), WebM and GIF copies: its FFmpeg adds the `mov` and `gif`
demuxers, the Opus, GIF, MJPEG and PCM decoders and libvpx's VP9 decoder
(libvpx is already bundled for VP9 export), and the MJPEG and PCM encoders for
`--av-marker`. Extract the image and run it headless with `APPDIR` set, for
example `./putgd-*.AppImage --appimage-extract`, then
`APPDIR=$PWD/squashfs-root squashfs-root/usr/bin/putgd-record --motion COPY.mp4 8`.
It can't read H.265 MP4 copies (FFmpeg's HEVC decoder would add 0.34 MiB to
the image) or WebP ones (the image's FFmpeg builds no animated WebP demuxer or
decoder): score those with a distro build of `putgd-record`, and WebP motion
with Pillow. Each CI
AppImage build runs `tools/releases/appimage_diagnostics.py` on the extracted
image: the helper writes a marker clip, exports it to MP4, WebM and GIF, and
`--motion`, `--preview` and, for the clips with sound, `--av-sync` must read
every copy.
