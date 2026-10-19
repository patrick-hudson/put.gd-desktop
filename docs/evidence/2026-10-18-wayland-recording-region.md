# Wayland recording regions, bar placement and system audio — 2026-10-18

Follow-up to [issue 50](2026-10-18-issue50-screen-recording.md) on branch
`claude/wayland-recording-region` (pull request into `nightly`). The behaviour
is described in [recording](../recording.md), "Wayland selection, bounds and
bar". Private receipts (driver scripts, logs, clips, frames) are under
`work/verification/2026-10-18-wayland-recording-region/` in the worktree.

## What changed

- A `region` recording without a rectangle is chosen before any stream or
  portal session opens: the helper shows the KDE layer-shell screenshot
  selector on Plasma, and the resident asks the GNOME Shell extension
  (`SelectArea`, recording protocol 1) on GNOME. Escape leaves no stream or
  helper. A rectless region on a Wayland desktop without a selector is
  refused (`region_unavailable`) instead of recording the whole monitor.
- Ready ring, outside dimming and control bar: layer surfaces on KDE, Shell
  actors and a moved bar window on GNOME. `overlayExcluded` and the new
  `overlayPlacement` (`outside`, `inside`, `hidden`, `compositor`) report
  where the bar went; KWin 6.6 and later hide the requesting client's windows
  from its own stream (`hidden`).
- `recording.select` / MCP `select_recording_region` complete an open selector.
- `putgd-record --av-marker` and `--av-sync` measure system-audio A/V sync.
- Two defects from PR #68 found during these runs and fixed:
  export progress was forwarded per frame, so a VA-API export (13x real time
  on the Kubuntu builder) exceeded the resident's 256 control frames a second
  and the helper was failed as `recording_helper_protocol` during Save copy
  (`bdc8fff`); `KdeScreencast::close()` destroyed the stream proxy after the
  protocol's destructor request had already freed it, crashing `putgd-record`
  on Cancel after a KDE recording (`daf0792`).

## Suites

Heavy suites ran on the builders from rsynced scratch trees
(`~/work/put.gd-wlrec`, identities in each tree's `COMMIT.txt`):

| Commit | Where | Result |
| --- | --- | --- |
| `b0cd5ee` (final) | Fedora 44 builder `.70` (GCC 16, Qt 6.11) | CTest 74/74, including `gnome_ui_ownership_contract` with the new recording contracts under GNOME 50's GJS |
| `7f09434` | Kubuntu 26.04 builder `.72` (GCC 15, Qt 6.10, LayerShellQt 6.6.4) | CTest 73/73; the layer-shell recording path compiled |
| `ff92cdc` tree | Debian 13 builder `.69` (Qt 6.8, LayerShellQt 6.3.4) | CTest 74/74 |
| `ff92cdc`–`2955cb1` | workstation (Mint 22.3, Qt 6.4, no layer shell; single tests only, the full suites ran on the builders) | `recording_service_integration` 16/16, `recording_helper_integration`, `test_mcp.py` 3 OK |

`b0cd5ee` changes only the extension and its contract after `7f09434`. A
local full run at `ff92cdc` showed `recording_helper_integration` failing
only because `putgd-record` had been rebuilt at a newer commit while the
suite ran (build-identity mismatch); it passed after a full build.

## A/V sync with system audio (physical Kubuntu 26.04 Plasma 6.6.6, X11, `.72`)

`putgd-record --av-marker` wrote a 20 s marker (black 1280x720 frames, a
white flash and a 1 kHz tone burst of 100 ms at the start of each second).
`ffplay -fs` played it on the physical desktop through the built-in card
(`alsa_output.pci-0000_00_1f.3.analog-stereo`, volume 40%); a 960x540 region
recording with `audio.system` (`vah264lpenc` + `avenc_aac`) was saved as MP4
and measured with `putgd-record --av-sync` on the workstation:

| Output port | Retained | Pairs | Offset (audio − video) | Spread |
| --- | --- | --- | --- | --- |
| `analog-output-speaker` | 14.1 s | 14/14 | −4 ms | 0 ms |
| `analog-output-headphones` | 13.9 s | 14/14 | +8 ms | 0 ms |

The headphone port was selected with `pactl set-sink-port` and restored to
the speakers afterwards; no headphones are plugged into the lab machines, so
this proves capture through the headphone route, not a listening test. The
working (Matroska) clips measured +15 ms and +2 ms medians with a 34 ms
spread (one frame at 30 fps); the exported MP4 is re-timed on frame
boundaries. Two earlier attempts on this desktop lost Save copy to the
progress flood above (`k72-avsync/`, `k72-avsync3/`); the run on `bdc8fff`
(`k72-avsync4/`) passed both ports. The packaging session's AppImage run
overlapped this desktop between 01:22Z and 01:25Z; the reported clips are
from 01:28–01:29Z.

## KVM guests (final candidate `b0cd5ee`)

Capture scenarios (region, window, desktop, three clipboard kinds,
region-cancel-escape) ran first as a regression; then `phys_region.py` drove a
resident from the installed package with a moving pattern (SMPTE bars and a
ball, fullscreen): a region dragged in the selector with the QMP pointer,
Escape in the selector, `recording.select`, and a whole-monitor recording.
Motion is the clip's `--motion` score (static control 0.012); a region scores
near the control when the ball does not cross it during the clip.

| Guest (host) | Package | Scenarios | Drag | Escape | Select | Monitor |
| --- | --- | --- | --- | --- | --- | --- |
| Ubuntu 26.04 GNOME 50/Wayland, `gw-wl387cbb1a` from golden `0d2922e34f4b` (`.69`) | `b0cd5ee` DEB | 7/7 | Shell selector, 536x336 canvas = selection, bar `outside` (below the area in the ready frame); motion 0.005, the ball did not cross this area (0.512 in the earlier `2955cb1` pass) | cancelled; no stream, Mutter session or helper left | 640x360, `outside`, motion 0.170 | `inside` (one monitor), 0.201 |
| Kubuntu 26.04 Plasma 6.6.6/Wayland, `kw-wlcc7555f9` from golden `541c1316ab6b` (`.69`) | `b0cd5ee` DEB | 7/7 | layer-shell selector, 536x336 = selection, `outside`, motion 0.094; clip frames show only the bars' slice | cancelled; nothing left | 640x360, `outside` | `hidden`: bar visible on the desktop, absent from the clip ([`kubuntu66-guest-monitor-clip.png`](wayland-recording-region/kubuntu66-guest-monitor-clip.png)), 0.162 |
| Fedora 44 GNOME 50/Wayland, `fgw-wl4bdcdb41` from golden `de0468ff530f` (`.70`) | `b0cd5ee` RPM | 7/7 | Shell selector, 536x336 = selection, `outside` | cancelled; nothing left | 640x360, `outside` | `inside` |

The Fedora GNOME golden was not reprovisioned. Its earlier blocker (no shell
extension) was the session not reloading after the RPM install: the golden's
`lab` account may only run `dnf` through `sudo` and the session ignores the
reboot request, so the run installs the RPM and OpenH264 (Cisco repository)
in its own overlay, syncs, and resets the VM through QMP; GDM auto-logs in and
the Shell loads `putgd@put.gd` (ACTIVE). Its clips cover the put.gd dashboard
rather than the pattern (motion 0.03–0.07): stock Fedora GNOME has no tray
host, so the resident shows its window while recording. The physical Fedora
desktop, which has a tray host, provides the GNOME motion evidence.

Earlier passes found two defects and fixed them before the final run: the
KWin screencast double destroy crashed the helper on Cancel in the Kubuntu
guest (`2955cb1` package), and on the Fedora guest the extension's first move
of the bar was undone by Mutter's initial placement, leaving it inside the
area while reporting `outside` (fixed in `b0cd5ee`, which waits until the
window is shown and re-applies the move). The harness opened the dashboard
by saving settings and then clicked its Record screen button while raising
the pattern; the driver now restarts the resident after saving and does not
click.

All three overlays were retired with the guarded cleanup and explicit
completion manifests after export (02:45Z); their tunnels and wall tiles were
removed.

## Physical desktops

Driven through `tools/lab/physical.py` with the uinput agent and the hosts'
loopback VNC tiles, visible on the wall throughout. Each run started a
scratch-build resident (`~/work/put.gd-wlrec`, isolated XDG directories)
in the real session, a fullscreen moving pattern (SMPTE bars with a ball),
then: a region dragged in the desktop's selector, Escape in the selector,
`recording.select`, a whole-monitor recording, and the A/V marker with system
audio through the built-in card's speaker and headphone ports. Frames:
[`d69-plasma63-desktop.png`](wayland-recording-region/d69-plasma63-desktop.png),
[`f44-gnome50-desktop.png`](wayland-recording-region/f44-gnome50-desktop.png);
clip frames: [`d69-plasma63-region-clip.png`](wayland-recording-region/d69-plasma63-region-clip.png),
[`f44-gnome50-region-clip.png`](wayland-recording-region/f44-gnome50-region-clip.png).

| Desktop | Build | Drag | Escape | Select | Monitor | A/V speakers | A/V headphone port |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Debian 13 Plasma 6.3.6/Wayland (`.69`), `kde-screencast` (protocol v5), `vah264lpenc` | `7f09434` | KDE selector; 806x454 canvas = selection; ring, bar below, outside dimmed; `outside`; motion 0.070 | cancelled, no stream, node or helper left | 640x360, `outside` | `inside` (KWin 6.3 keeps the requester visible; bar in the clip) | 13/13 pairs, −52.7 ms median, 67 ms spread | 13/13, −30 ms, 33 ms |
| Fedora 44 GNOME 50/Wayland (`.70`), `mutter-screencast`, `openh264enc` | `b0cd5ee` (extension) on the `7f09434` binaries | Shell selector; 806x454 = selection; Shell ring, bar moved below, outside dimmed; `outside`; motion 0.077 | cancelled, Mutter session count unchanged (only the wall's own) | 640x360, `outside` | `inside` (one monitor; Mutter has no exclusion) | 13/13, −45.3 ms, 33 ms | not measurable, below |

Static control for motion is 0.012; the region scores are low because the
ball crosses the recorded slice only briefly, and every clip frame shows the
expected slice of the bars and nothing else (no bar, no ring).

On the Wayland desktops the marker played through GStreamer `playbin` with
`waylandsink` and audio led the flashes by 30–53 ms, against −4/+8 ms on the
X11 desktop with `ffplay`; the difference includes the players' own output
latency handling and is within the ITU-R BT.1359 acceptability range
(audio leading by less than 90 ms), but it is above the 45 ms detectability
threshold for the d69 speaker run.

On Fedora, selecting the unplugged headphone port makes WirePlumber move
the default sink to a network (AirPlay) sink, so `@DEFAULT_MONITOR@` recorded
that sink's silence (`av_sync_no_bursts`) while the built-in sink's own monitor
still carried the tone (checked separately, RMS 0.12). No sound was sent to
the network sink: the marker and the check tone address the built-in sink
explicitly, and the default sink and speaker port were restored. Recording
follows the default sink by design ("the sound the computer plays").

Temporary desktop changes, approved by Patrick and undone: on `.69`,
`~/.local/share/applications/putgd-record-wlrec.desktop` naming the scratch
helper for KWin's screencast permission, 02:13:42Z–02:16:27Z, with `kbuildsycoca6`
after both changes. On `.70`, the modified extension as a user copy
(`~/.local/share/gnome-shell/extensions/putgd@put.gd`) from 01:44Z, loaded by
a GDM restart (auto-login) at 02:32Z after notice to the other sessions, and
removed with a second restart at 02:35Z. `gnome-extensions info` showed the
user copy ACTIVE during the run; afterwards it reports that `putgd@put.gd`
does not exist, because no putgd RPM is installed on `.70` (the packaging
session had removed its package at 01:39Z). The input agents ran as
`putgd-lab-input-wlrec` units.

## Not covered

- Real headphones: no headphones are plugged into the lab machines, so the
  headphone port proves capture through that route, not a listening test; on
  Fedora the route could not be measured (WirePlumber moves the default sink).
- Multi-monitor Wayland: the bar's "another monitor" placement is covered by
  the synthetic contract only; every lab desktop and guest has one output.
- A physical Plasma 6.6 Wayland session: `.72` runs Plasma 6.6 on X11, so
  KWin 6.6's requester hiding was verified in the Kubuntu 26.04 guest only.
- The packaged GNOME extension on `.70`: no putgd RPM is installed there now,
  so the physical GNOME run used the extension as a user copy with the
  scratch build.
- Windows and microphone capture (unchanged from issue 50).
