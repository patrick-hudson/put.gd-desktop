# AppImage recording on Plasma 6.6 Wayland (#137) — 2026-10-20

Issue #137: on physical Kubuntu 26.04 Plasma 6.6.6 Wayland (k72: Intel HD 530,
PipeWire 1.6.2), the AppImage prepared a recording, but every `recording.start`
failed in the source with `putgdpipewiresrc ... stream error: error alloc buffers:
Invalid argument`. That happened with every encoder, on both `kde-screencast` and
the portal. The Ubuntu 26.04 DEB worked in the same session. Branch
`claude/jovial-fermat-02d16c`, pull request #140 into `nightly`. The behaviour is
described in [recording](../recording.md) ("AppImage runtime") and
[the AppImage README](../../packaging/appimage/README.md).

## Cause

The error comes from PipeWire's link code (`impl-link.c`, `do_allocation`), not
from GStreamer. `pw_buffers_negotiate` intersects the two ports' `Buffers` params
and returned `-EINVAL` because they had no common value:

- The AppImage bundles Ubuntu 24.04's GStreamer `pipewire` plugin, built from
  PipeWire 1.0.5. Its `pipewiresrc` defaults to `min-buffers=8` and offers
  `buffers Range(16; 8..INT32_MAX)`.
- KWin's screen cast (`ScreenCastStream::newStreamParams`, Plasma 6.3 to 6.6, for
  both its own protocol and the portal) offers `buffers Range(3; 2..4)`, for MemFd
  and DMA-BUF alike.
- Up to 1.4.x, PipeWire's `spa_pod_filter` accepted a range-to-range intersection
  with no overlap. From 1.5.81 it rejects it ("reject impossible range",
  `spa/pod/filter.h`), so the link fails.

That explains every row of the issue. d69 Plasma 6.3 runs a 1.4.2 daemon, which
still accepted the empty intersection. Mutter on f44 offers `Range(16; 2..16)`,
which overlaps `8..`. The distro packages use their own PipeWire's plugin:
`min-buffers` defaults to 1 there since later 1.4.x releases (1.4.0 and 1.4.2
still used 8; 1.4.9 and 1.5.81 use 1). DMA-BUF modifiers play no part.

## Fix

The recorder's screen-cast source sets `min-buffers=1`
(`pipewireSourceDescription()` in `src/record/WaylandAcquisition.*`, used by
`RecordingPipeline`), so the compositor's range decides the count: KWin 3–4,
Mutter 16. Newer plugins default to the same value, so the property changes
nothing for them. No library or plugin changed, so the AppImage size is the
same as nightly's. libpipewire stays the host's.

**Considered and rejected:** bundling a GStreamer plugin from a newer PipeWire,
or preferring the host's plugin.

- **Newer bundled plugin.** PipeWire 1.6.9's `libgstpipewire.so` imports
  `pw_stream_get_data_loop`, `pw_stream_return_buffer` and `pw_stream_set_rate`,
  which libpipewire 1.0.x doesn't export. It would fail to load on Ubuntu
  24.04-era hosts and disable Wayland recording there. All of its imports exist
  in 1.4.2.
- **The host's plugin.** Fedora 44's plugin needs no GStreamer symbol beyond the
  bundled 1.24 core, but Debian 13's (PipeWire 1.4.2) has the same
  `min-buffers=8` default. Using it would trade a fixed, tested plugin for
  whatever each host ships.
- **SHM/memfd fallback.** Not needed: the link fails before any memory type is
  chosen, and KWin offers the same range for MemFd.

## Reproduction without a desktop

`tests/desktop/pipewire/run_buffers.sh PLUGIN_DIR kwin|mutter [props]` starts a
private PipeWire daemon (protocol, access, client-node, adapter and link-factory
modules only; no session manager, no devices) in a new runtime directory. It
runs a source that announces KWin's or Mutter's `Buffers` param and
`gst-launch-1.0 pipewiresrc ...` with the given plugin, links them with
`pw-link` and reports whether frames arrive. Plugin: the PR AppImage's
`libgstpipewire.so` (`g74c2eafaef17`; byte-identical to nightly's
`5ac6bbc`/`8e568fd` image, sha256 `82bd35dc…`). Host plugin: the distro's.

| Host | Daemon | Plugin | `kwin` | `kwin` + `min-buffers=1` | `mutter` | `mutter` + `min-buffers=1` |
| --- | --- | --- | --- | --- | --- | --- |
| k72 | 1.6.2 | AppImage (1.0.5) | **FAIL**: `error alloc buffers: Invalid argument (-22)` | PASS, 3 buffers | PASS, 16 | — |
| f44 | 1.6.9 | AppImage (1.0.5) | **FAIL**, same error | PASS, 3 buffers | PASS, 16 | PASS, 16 |
| f44 | 1.6.9 | Fedora's (1.6.9) | PASS, 3 | PASS, 3 | — | — |
| d69 | 1.4.2 | AppImage (1.0.5) | PASS, 4 | PASS, 4 | PASS, 16 | PASS, 16 |
| d69 | 1.4.2 | Debian's (1.4.2) | PASS, 4 | PASS, 4 | — | — |

PASS means about 148 of the source's 150 frames reached the sink in 5 s.
k72's runs predate its outage (see below). m74's
PipeWire 1.0.5 daemon has the same lenient filter as d69's, but the host
has no libpipewire headers to build the source with, and installing them
would add a PipeWire package to a lab desktop, so it wasn't run there.

## Desktop acceptance (PR #140's AppImage)

Image `putgd-0.1.1-dev.516+g74c2eafaef17-x86_64.AppImage` (CI run
39352270738, the PR's merge commit `74c2eaf`), sha256 `90de1577…9624b`,
113,682,936 bytes. It ran as `IMAGE --background` with private XDG
runtime/config/cache/state directories. For `kde-screencast` the real
`XDG_DATA_HOME` was used, so KWin read AppRun's temporary entry.

**Method:** the image's own `putgd-record --av-marker` clip (black, a 100 ms
flash and a 1 kHz beep each second) played full screen through GStreamer
(`waylandsink`/`xvimagesink`, `pulsesink` to the built-in speakers) while the
production CLI recorded the monitor with system audio at 30 fps. Then
`recording.saveCopy` wrote the export, and the image's own `--av-sync` measured
the working clip and the export. Offsets are audio minus video, positive when
the sound is late (one frame = 33.3 ms). The median excludes nothing: the first
pair is sometimes off while the player starts. Motion is `--motion CLIP 8` on a
scrolling SMPTE pattern with a moving ball, against the same bars standing still
(player started before the recording). Load is the 1-minute load average during
each run.

| Desktop / path | Encoder | A/V export medians (ms) [working clip] | Motion: pattern / still | Load |
| --- | --- | --- | --- | --- |
| k72 Plasma 6.6.6 Wayland `kde-screencast` | `vah264lpenc` | MP4 +20.0 [+20.0], MP4 +28.7 [+29.0], WebM +29.5 [+29.0] | 88.4, 86.5 (WebM) / 0.001 | 0.7–2.5 |
| k72 Plasma 6.6.6 Wayland portal (KDE picker) | `vah264lpenc` | MP4 +34.7 [+35.0] | — | 0.6 |
| d69 Plasma 6.3 Wayland `kde-screencast` | `vah264lpenc` | MP4 +31.3 [+31.0], MP4 +12.7 [+13.3], WebM +9.5 [+9.0] | 77.1 / 0.000 | 4.8–9.5 |
| f44 GNOME 50 Wayland `mutter-screencast` | bundled `x264enc` | MP4 +33.3 [+33.0], MP4 −4.0 [−4.0], WebM +17.5 [+16.7] | 76.5 / 0.001 | 0.2–2.7 |
| m74 Mint 22.3 Cinnamon X11 (`ximagesrc`) | `vah264lpenc` | MP4 +5.3 [+5.0], MP4 +5.3 [+5.0], WebM +12.5 [+12.0] | 78.6 / 0.000 | 0.4–1.8 |

Before the fix, the same k72 session failed every start: 4 of 4 on
`kde-screencast` and 1 of 1 on the portal (#87, images `8e568fd` and `5ac6bbc`).
Every run above started, stopped, saved and measured.
The d69, f44 and m74 numbers sit in the ranges #87 measured with nightly
(d69 AppImage +20 to +26.5, f44 AppImage −8 to +25.3, Mutter's damage-driven
stream swinging one frame between runs). On d69, other sessions' builds pushed
the load to 8–9 on four cores during the later runs; the pairs there alternate
by one frame.

k72 stopped answering on the network at about 01:56Z, roughly 10 minutes after
this slot was released clean (Plasma Wayland up, wall VNC active, load about 1).
Two other sessions' builds were running there at the time. It was not rebooted
by any session and needs a physical look.

Hosts: k72 Kubuntu 26.04.1, Plasma 6.6.6 Wayland, kernel 7.0.0-38, PipeWire
1.6.2; d69 Debian 13, Plasma 6.3, PipeWire 1.4.2; f44 Fedora 44, GNOME Shell
50, PipeWire 1.6.9; m74 Linux Mint 22.3, Cinnamon 6.6.4 X11, PipeWire 1.0.5.

KDE X11 was not re-run: on 2026-10-20 Patrick made Plasma Wayland k72's
default session, to be switched to X11 only when a KDE/X11 check needs it
(PR #141). The X11 path was checked on m74 instead. This change only touches
the PipeWire source, which X11 recording doesn't use.

## Packages

The CI packages from the same run (`74c2eaf`) were installed for one slot each
and then removed. Each transaction installed only `putgd`. Same method; load
during each run is shown.

| Desktop / package | Path, encoder | A/V export medians (ms) [working clip] | Motion: pattern / still | Load |
| --- | --- | --- | --- | --- |
| d69 Plasma 6.3 Wayland, Debian 13 DEB | `kde-screencast` from the installed `putgd-record.desktop`, `vah264lpenc` | MP4 −10.0 [−10.0], WebM +0.5 [0.0] | 77.2 / 0.001 | 1.1–2.1 |
| f44 GNOME 50 Wayland, Fedora 44 RPM | `mutter-screencast`, `openh264enc` | MP4 −9.3 [−9.0], WebM +12.5 [+12.0] | 82.7 / 0.006 | 0.2–1.0 |

The Debian 13 DEB is the package this change affects. d69's own GStreamer
plugin comes from PipeWire 1.4.2, which still defaults to `min-buffers=8`, so the
recorder now negotiates KWin's count there instead of a clamped 8. On a newer
daemon the DEB would have hit #137 too. Fedora's (1.6.9) and Ubuntu 26.04's
(1.6.2) plugins already default to 1, so the property changes nothing for them.
The Ubuntu 26.04 DEB wasn't re-run, because k72 was offline. The Ubuntu 24.04
DEB records X11 through `ximagesrc` on Mint, which doesn't use the PipeWire
source.

## Visuals

Wall frames taken while the pattern covered the screen (downscaled):
[k72 `kde-screencast`](appimage-plasma66-137/k72-plasma666-wayland-kde-screencast-recording-pr140.png),
[k72 portal picker](appimage-plasma66-137/k72-plasma666-wayland-portal-picker-pr140.png),
[d69 `kde-screencast`](appimage-plasma66-137/d69-plasma63-wayland-kde-screencast-recording-pr140.png),
[f44 Mutter](appimage-plasma66-137/f44-gnome50-wayland-mutter-recording-pr140.png),
[m74 X11](appimage-plasma66-137/m74-cinnamon-x11-recording-pr140.png).
Raw driver reports, clips and logs are private under `work/i137` in the
session's worktree. The clips stay in `~/putgd-lab/i137` on each host.

## Lab changes, undone

- **k72:** SDDM autologin switched from `plasmax11.desktop` to `plasma.desktop`
  and left there (Patrick's decision). My backup of the X11 file was removed;
  `sddm.conf.acc87-backup` was kept. The input agent unit was stopped. KDE's
  permission-store entry from the portal run was removed. The
  `~/.local/share/putgd` the runs created was moved to
  `~/putgd-lab/i137/realdata-local-share-putgd` with Patrick's OK.
- **d69:** AppRun's temporary KWin entry existed only while the image ran, and
  `kbuildsycoca6` was rerun. `~/.local/share/putgd` was moved to
  `~/putgd-lab/i137/realdata-local-share-putgd`.
- **f44:** the RPM was installed for its slot and removed with `dnf remove`.
  Nothing else outside `~/putgd-lab/i137`.
- **d69 (package slot):** the DEB was installed and removed with `apt-get
  remove`, and `kbuildsycoca6` was rerun.
- **m74:** nothing outside `~/putgd-lab/i137`.

Every desktop was released with `desktop_lock.py release --verified-clean`.
