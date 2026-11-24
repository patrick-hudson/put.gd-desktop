# Screen recording: packages and codecs

Part of [Screen recording](../recording.md).

## Packages and codecs

Recording keeps an H.264 working clip, so capture needs a working H.264
encoder and every preview and export needs FFmpeg's H.264 decoder.
`recording.capabilities` tests both instead of trusting the registry: each
registered live encoder encodes two synthetic 320×240 frames through
`h264parse`, each export encoder opens on a 320×240 frame (VA-API ones on a VA
device and frame pool, as the export does; NVENC only by its driver libraries
being loadable, because opening it initialises CUDA and wakes a sleeping GPU),
and the H.264 decoder opens. The results are `gstreamer.usableH264Encoders`
with `gstreamer.encoderFailures`, `exportEncoders` (openable ones only, which
also decide `formats` and `codecs`) with `ffmpeg.encoderFailures`, and
`ffmpeg.h264Decoder {name, usable, failure}`. Without a usable encoder or
decoder the reason is `live_encoder_missing` (none registered),
`h264_encoder_unusable` (installed, none works) or `h264_decoder_unusable`,
and the probe adds `message` and a distribution-specific `hint`.
`recording.prepare` then refuses with `recording_unavailable`, whose message
is the two together and whose details carry `reason` and `hint`; the dashboard
shows that message in a warning dialog and the tray as its failure notification. The checks add
a few tens of milliseconds to a probe; a fresh GStreamer registry costs more
than all of them.

| Package | Live H.264, in preference order | Export H.264 | Setup |
| --- | --- | --- | --- |
| DEB (Ubuntu 24.04 / Mint 22.3, Ubuntu 26.04 / Kubuntu, Debian 13) | `vah264lpenc`/`vah264enc` (plugins-bad, or plugins-extra on 26.04), `vaapih264enc` (Recommends `gstreamer1.0-vaapi`), `x264enc` (Recommends `gstreamer1.0-plugins-ugly`), `openh264enc` (plugins-bad, a Depends) | `h264_nvenc`, `h264_vaapi`, `libx264`, `libopenh264` | none: plugins-bad always brings `openh264enc` and `voaacenc` |
| RPM (Fedora 44 and 45) | `vah264*enc` (stock Intel and AMD drivers have no H.264 encode; RPM Fusion's do), `openh264enc` (Recommends `gstreamer1-plugin-openh264`) | `h264_nvenc`, `h264_vaapi`, `libopenh264` | Fedora Workstation ships the `noopenh264` placeholder. The RPM's Recommends do not replace an installed conflicting placeholder, even with weak dependencies and `fedora-cisco-openh264` enabled. Run `sudo dnf install openh264 gstreamer1-plugin-openh264` explicitly. The one-line installer runs this codec step for you when the repository is enabled and the placeholder remains or either codec package is missing. If the repository is disabled, it prints the commands: first `sudo dnf config-manager setopt fedora-cisco-openh264.enabled=1`, then the codec install. It does not change repository settings silently |
| AppImage | `vah264lpenc`/`vah264enc`, `vaapih264enc` through the host's VA-API driver, then the bundled `x264enc` | `h264_nvenc`, `h264_vaapi` through the host's driver, then the bundled `libx264`; H.265 `hevc_nvenc`, `hevc_vaapi`, then the bundled `libx265` | none: the bundled x264/x265 cover hosts whose driver cannot encode |

On Fedora with the placeholder the probe reports `h264_encoder_unusable`
(`openh264enc`: "Could not initialize supporting library"), FFmpeg's
`libopenh264` decoder and encoder fail to open, and the hint names the
command above. Debian 13 has its own release DEB
(`putgd-VERSION-debian13-x86_64.deb`), built against Debian's FFmpeg 7.1: the
Ubuntu DEBs link FFmpeg 6.1 (24.04) or 8.0 (26.04), so neither installs there
now that the helper is packaged.

The microphone needs `pulsesrc` (plugins-good) and, for the mix with system
audio, `audiomixer` (plugins-base); both packages are already dependencies of
the DEB, RPM and Arch packages, and the AppImage bundles both plugins. Where
either is missing the probe reports `audio.microphone: false` with
`microphone_elements_missing`.

### AppImage runtime

The AppImage builds `putgd-record` against a pinned upstream FFmpeg
(`packaging/appimage/tools.lock.json`) configured with `--enable-gpl` and
exactly the demuxer, decoders, encoders, muxers and filters the export uses
(`packaging/appimage/media.json`): the native H.264 and AAC decoders,
`h264_nvenc`, `h264_vaapi`, `libx264`, `hevc_nvenc`, `hevc_vaapi`, `libx265`,
`libvpx-vp9`, `gif`, `libwebp_anim`, `aac` and `libopus`. The encoder order
puts the machine first: the host's GPU encoders (NVENC, VA-API) are tried by
actually opening or starting them, and the bundled x264 (live and MP4) and
x265 (H.265 export) are used only when none of them works; the encoder used
is reported as for the other packages. Live H.265 does not exist, so only
FFmpeg's `libx265` is bundled, not GStreamer's `x265enc`. With x264, x265 and a
GPL FFmpeg inside, the image is a GPL-2.0-or-later combined work:
`usr/share/doc/putgd/APPIMAGE-GPL-NOTICE.txt` says so and names the bundled
versions, the licence texts are under `usr/share/doc/putgd/bundled-licenses`,
and every release publishes the corresponding source as
`putgd-VERSION-appimage-complete-source.tar.gz` (see [releases](../../release/release.md)).
GStreamer comes from the Ubuntu 24.04 builder: the core libraries, the plugins
the live pipeline uses (`coreelements`, `videotestsrc` for the probe,
`videoconvertscale`, `videorate`, `videocrop`, `audioconvert`, `audioresample`,
`audiomixer` (the microphone's mix with system audio, #1364), `ximagesrc`, `pipewire`, `pulseaudio`, `matroska`, `audioparsers`,
`videoparsersbad`, `va`, `vaapi`, `x264`, `voaacenc`) and `gst-plugin-scanner`
under `usr/libexec/gstreamer-1.0/`. VA-API (`libva*`), PipeWire's client
library, the NVIDIA libraries and any OpenH264 stay the host's: libva must
match the host's VA drivers and libpipewire its daemon and modules. On a host
without libva, AppRun adds the bundled copy in `usr/lib/putgd-host-fallback/`
so the helper still starts. (A host OpenH264 cannot be used: the image's
GStreamer and FFmpeg were built against Ubuntu 24.04's `libopenh264.so.7`,
while Fedora 44 ships `.so.8`.)

The image's `pipewire` plugin therefore comes from PipeWire 1.0.5 and talks to
whatever daemon the host runs. Its `pipewiresrc` asks for at least 8 buffers by
default, while KWin's screen casts (Plasma 6.3 to 6.6, portal and
`kde-screencast` alike) offer 2-4. PipeWire up to 1.4 linked the two anyway;
1.5.81 and later refuse ranges that don't overlap, and every start failed with
`error alloc buffers: Invalid argument` on Plasma 6.6 with PipeWire 1.6 (#137).
The recorder sets `min-buffers=1` on the source, which newer plugins use by default,
so the compositor picks the count (3 or 4 with KWin, 16 with Mutter). That also
covers the distro packages whose own plugin still asks for 8: Debian 13's comes
from PipeWire 1.4.2. Fedora 44's and Ubuntu 26.04's plugins already default to 1.
`tests/desktop/pipewire/run_buffers.sh` reproduces the
negotiation with a private PipeWire daemon, a KWin- or Mutter-like source and
any plugin directory, no desktop session needed.

When `putgd-record` runs from the mounted image (`APPDIR` contains the
executable), it confines its own GStreamer before initialising it
(`src/record/BundledRuntime.cpp`): `GST_PLUGIN_SYSTEM_PATH` is the image's
plugin directory, `GST_PLUGIN_PATH` is emptied, `GST_PLUGIN_SCANNER` is the
image's scanner and `GST_REGISTRY` is
`$XDG_CACHE_HOME/putgd/gstreamer-1.0-appimage-registry.bin`, rebuilt once per
mount. Only the helper sets these, so host programs started by put.gd (the
browser behind a notification) keep the host's GStreamer. On Plasma, AppRun
writes a second temporary private entry next to the ScreenShot2 one,
`putgd-record-appimage-XXXXXXXX.desktop` with `Exec` naming the mounted
`putgd-record` and `X-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1,org_kde_kwin_fake_input`,
so KWin offers its screencast protocol, and through Plasma 6.7 the fake input protocol a
[scrolling capture](../scroll-capture.md) turns the wheel with, to the helper; both entries are removed
on exit. The assembly audit runs the relocated helper's probe and fails unless
every required element loads from the image's plugin directory only, the
H.264 decoder opens, all four formats export, the bundled `x264enc`,
`libx264` and `libx265` work (the builder has no GPU, so they are what must)
and no host-matched library was bundled; the receipt records the result under
`postDeploymentAudit.recording` and the bundled GPL packages under
`recordingRuntime.gplComponents`.
