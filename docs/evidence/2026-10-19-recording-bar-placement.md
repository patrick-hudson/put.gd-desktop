# Recording control bar placement and handle (issue #108) — 2026-10-19

This record certifies what pull request #109 (`claude/blissful-lichterman-437a6b`
into `nightly`, issue #108) built and ran. Before the change, the recording
control bar sat inside a full-screen recorded area on X11 (bottom-right corner,
in every frame), widened past the screen edge after Record, and could not be
moved. The same inside fallback put the full bar in KWin 6.3 Wayland and GNOME
clips. The design is in [recording](../recording.md), "Overlay (X11)" and
"Wayland selection, bounds and bar".

Bulk artifacts (clips, working clips, frames, screenshots, per-case JSON, the
drivers) are kept locally in
`work/verification/2026-10-19-recording-bar-placement/` on the workstation.

## Candidates

| Build | Used on | Identity |
| --- | --- | --- |
| Scratch dev build of `fa37dbc` (all code except `7542e8b`) | k72 | `~/work/put.gd-108/build/dev` on k72, nice -j3 |
| CI run 39273616591 on `fe15cd3`, merge `565058fb15f8` (`0.1.1-dev.447+g565058fb15f8`) | d69 first run, Cinnamon guest | Debian 13 DEB 51,917,700 B; Ubuntu 24.04 DEB 52,007,944 B (`4c95e95d…bec7` as installed by `lab.py`) |
| CI run 39277281897 on `7542e8b`, merge `ce5546ac2e2a` (`0.1.1-dev.460+gce5546ac2e2a`) | d69 recheck, f44 | Debian 13 DEB `af42c22a72d9a3b39480a222eba86fb4b18a48a60364e62eb6ce7fc6bb09dfde`, 51,918,326 B; Fedora 44 RPM `0f7a40cbeaa70749809fb0b9b915971c1283cf8588385301f503a2c8f36f56b8`, 39,214,978 B |

## Suites

- `bar_placement_contract` (three offscreen monitors: 2560x1440 x2 and
  1920x1080, Patrick's layout) and `bar_placement_single_monitor_contract`
  (1366x768) passed on d69 and k72. They check the pure placement rules over
  500 random regions per layout, the real `ControlBar` after Record and across
  every state, the handle on one monitor, and dragging. `initTestCase` fails if
  the offscreen platform ignored the layout.
- Mutation check on d69: without re-placement on resize, 5 cases fail
  (`barStaysOnItsScreenAfterRecord` reports the recording bar off its screen);
  without the other-monitor candidates, 3 fail.
- `recording_service_integration` (resident reports `handle` and sends the
  helper `overlay {handle: true}` when the Shell leaves the bar inside),
  `recording_helper_integration`, `recording_model_contract`,
  `portal_streams_contract`, `wayland_acquisition_contract` and
  `record_runtime_contract` passed on k72 (8/8).
- `gjs -m tests/desktop/test_gnome_ownership.mjs integrations/gnome/extension.js`
  passed on f44 (gjs 1.88.0), with the new placement cases and the bar
  following its size changes (widening, the handle moving to the corner).
- `tests/desktop/test_lab.py` (23 tests) passed with the new
  `cinnamon-x11-dual` profile.
- Pull request CI on `fe15cd3` passed: contracts, Linux 24.04 and 26.04,
  Windows portable, the four native packages and the AppImage.

## Method

`scripts/rb108.sh` in the artifact folder ran on each desktop as the session
user with a private `XDG_*` profile (local destination, archive copy). It
started a fullscreen `ffplay` pattern it owned (khaki field, a 320x240
`testsrc2` tile moving in the top 60% only), then for each case:
`recording.prepare` (MP4), `inspect` until ready, `start`, `pause`/`resume`,
`stop` (or a click on the handle's Stop), `saveCopy`, `cancel`. On X11 it read
the bar window's geometry in every state with `xwininfo` and took root
screenshots while the pattern covered the screen. `scripts/rb108_frames.py`
decoded every frame of each clip and reported the share of dark pixels (luma
< 70; the bar is `#15171c` on the khaki field) in the ready bar's rectangle
minus the handle's, and in the handle's rectangle, plus
`putgd-record --motion CLIP 8`. A frozen copy of the pattern is the motion
control.

## k72: Kubuntu 26.04, Plasma 6.6.6 X11, one 1920x1080 monitor

Kernel 7.0.0-38-generic, KWin 6.6.6, Qt 6.10.2, GStreamer 1.28.2. Desktop
lock 15:06:00Z–15:17:47Z, released `--verified-clean`. Load 0.04 at the lock,
1.40 at the scored run's start, 3.32 at its end.

| Case | Target | `overlayPlacement` | Bar (ready / recording / paused) | Ready-bar area dark, frame 0 / max | Handle area dark | Motion |
| --- | --- | --- | --- | --- | --- | --- |
| handle | region 0,0,1920,1080 | `handle`, excluded false | 601x43+1309+1027 / 88x35+1822+1035 / 168x35+1742+1035 | 0.000 / 0.110 | 0.518–0.545 | 5.485 |
| monitor | monitor | `handle` | 541x43+1369+1027 / 88x35+1822+1035 / 168x35+1742+1035 | 0.000 / 0.000 | 0.518 | 5.479 |
| below | region 1620,100,300,200 | `outside`, excluded true | 586x43+1334+310 / 640x43+1280+310 / 652x43+1268+310 | not in the 300x200 clip | — | — |
| hidden | as handle, `recording.overlay {visible:false}` before Record | `handle` | hidden | 0.000 / 0.000 | 0.000 | 5.536 |
| stopclick | as handle; Stop clicked on the handle | `handle` | as handle | 0.000 / 0.110 | 0.518–0.545 | 5.991 |
| static | as handle, frozen pattern | `handle` | as handle | 0.000 / 0.000 | 0.518 | 0.020 |

- Every bar rectangle in every state is inside the 1920x1080 screen. In the
  `below` case the bar widens after Record and stays put by its right edge
  (1920), still below the region.
- Frame 0 of every full-screen clip has no ready-bar pixels: the bar shrinks
  to the handle and capture starts 150 ms later. The 0.110 maximum is the
  paused handle (Resume + Stop, 168x35) caught for a few frames around Resume.
- With the bar hidden, no frame has bar pixels anywhere.
- The handle's Stop was clicked with `xdotool` only after
  `getmouselocation` showed the pointer over the bar's own window
  (115343397); the session went to `reviewing`.
- Patrick's Codex session was never on screen: screenshots were taken only
  while the fullscreen pattern covered the display, and the pattern and the
  bar were the only windows touched.

## d69: Debian 13, Plasma 6.3.6 Wayland (KWin 6.3), one 1920x1080 monitor

Kernel 6.12.107+deb13, KWin 6.3.6, Qt 6.8.2, GStreamer 1.26.2. CI Debian 13
DEB installed with `apt-get` (the transaction was the one package),
`kde-screencast` v5, `hidesRequester: false`. Desktop lock
15:18:16Z–15:21:21Z, released `--verified-clean` after `apt-get remove`.

| Case | `overlayPlacement` | Frame 0 bottom-right corner | Motion |
| --- | --- | --- | --- |
| handle (region 0,0,1920,1080) | `handle` | only the dot + Stop handle | 5.511 |
| monitor | `handle` | only the handle | 5.465 |
| below (region 1620,100,300,200) | `outside`, canvas 300x200 | — | — |
| hidden | `handle` | **the full ready bar** (fixed by `7542e8b`, see below) | 5.515 |
| static | `handle` | only the handle | 0.001 |

Plasma's panel stays above an XWayland fullscreen window, so the dark-pixel
shares there measure the panel; the corner crops (`d69-run1/corners.png`)
were compared by eye instead. The hidden case showed that KWin applies a hide
from `recording.overlay` just before Record after the stream's first frame.
`7542e8b` gives any bar placed inside the area the 150 ms settle, hidden or
not.

### d69 recheck on `7542e8b`

Desktop lock 15:40:39Z–15:41:59Z, the CI DEB of `7542e8b` installed and
removed again; load 0.08 at the start, 1.62 at the end. The hidden case ran
twice and the handle case once. The first four frames of the hidden clip
(`d69-run2/fr/hidden-1..4.png`) have no bar and the handle clip's first frame
shows only the handle (`d69-run2/corners.png`); hidden motion 5.466.

## f44: Fedora 44, GNOME Shell 50.0 Wayland, one 1920x1080 monitor

Kernel 6.19.10-300.fc44, mutter 50.0-1.fc44, Qt 6.11.2, GStreamer 1.28.8.
Desktop lock 15:42:19Z–15:48:31Z, released `--verified-clean`. The CI RPM
was installed (one-package transaction); its `extension.js` is byte-identical
to the branch's (`ed366104…a7c5`). A GDM re-login at 15:42:40Z loaded it:
`putgd@put.gd` version 2 ACTIVE, AppIndicator ACTIVE. No apps of Patrick's
were running before either re-login. After the runs the RPM was removed and
a second re-login at 15:48:00Z left AppIndicator ACTIVE and in
`enabled-extensions` (`putgd@put.gd` was already listed there and was left
as found). Another session's guest (`lab-runs/mhcin3-77ea45c1`) ran on the
host throughout; load 1.33 at the start, 3.79 at the end.

`ffplay` crashes on this host and it has no `xrandr`, so the pattern was
`ffmpeg` piped into a fullscreen `waylandsink` and the size came from
`RB_W=1920 RB_H=1080`. A first attempt without those failed before any
recording (`Area is off-screen` for an empty size); its put.gd failure banners
went with the closing re-login.

| Case | `overlayPlacement` | Ready-bar area dark, frame 0 / max | Handle area dark | Motion |
| --- | --- | --- | --- | --- |
| handle (region 0,0,1920,1080) | `handle` (Shell left the bar inside; resident told the helper) | 0.000 / 0.020 | 0.092–0.460 | 5.461 |
| monitor | `handle` | 0.000 / 0.000 | 0.265–0.460 | 5.415 |
| below (region 1620,100,300,200) | `outside` | — | — | — |
| hidden | `handle`, hidden | 0.000 / 0.000 | 0.000 | 5.389 |
| static | `handle` | 0.000 / 0.020 | 0.092–0.460 | 0.063 |

The extension moved the handle to 10 px in from the corner after the bar
shrank (wall frame `f44/wall-handle2.png`). The ready-bar and handle areas
here are the generic corner boxes (the Shell places the bar, so there is no
X11 geometry); the handle-area range includes the move into the corner.

## Cinnamon guest with two monitors (f44)

Linux Mint 22.3, Cinnamon 6.6.4, kernel 6.14.0-37, run `cin108-91ef7e97`
from a fresh overlay of the verified `cinnamon-x11` golden on f44 (QEMU
10.2.2) with the new `cinnamon-x11-dual` profile
(`virtio-vga,max_outputs=2`). The guest showed on the workstation wall as
"Cinnamon VM cin108-91ef7e97" through a tunnel to its VNC; the tile and the
tunnel are removed.

**Not equivalent to two outputs.** The second virtio-gpu scanout stayed
disconnected: QEMU's VNC display never enables it, and the guest user has no
passwordless sudo to force the DRM connector. The guest's one output was set
to 2560x1080 and split into two RandR 1.5 monitors with
`xrandr --setmonitor` (`putgd-left` 1280x1080+0+0, `putgd-right`
1280x1080+1280+0). Qt, and so the helper's placement, sees two screens;
Cinnamon itself still lays out one 2560-wide desktop (its panel and
fullscreen windows span both halves), and put.gd's `monitor` target, which
resolves by RandR output, covers the whole 2560x1080 output.

| Case | Target | `overlayPlacement` | Bar (ready / recording / paused) | Motion |
| --- | --- | --- | --- | --- |
| handle | region 0,0,1280,1080 (the left monitor) | `outside`, excluded true | 602x43+1618+10 / 656x43+1591+10 / 668x43+1585+10 (right monitor, top centre) | 5.956 |
| desktop | desktop | `handle` | 581x43+689+1027 / 88x35+1182+1035 / 168x35+1102+1035 | 4.578 |
| monitor | monitor (the 2560-wide output) | `handle` | as desktop | 4.573 |
| below | region 980,100,300,200 | `outside` | 587x43+693+310 / 641x43+639+310 / 653x43+627+310 | 7.666 |
| hidden | as handle, hidden | `outside` | hidden | 6.504 |
| static | as handle, frozen | `outside` | as handle | 0.008 |

The full-left-monitor clip (1280x1080) has no bar pixels: the bar is on the
other monitor in every state (frames `handle-frame0.png`,
`handle-frame3s.png`). The overlay was stopped with `lab.py reset`; guarded
`cleanup` refused it because the run lives in the scratch tree
(`~/work/put.gd-108/work/lab-runs`) while the golden is under
`~/work/put.gd/work/lab-images`, so the 929 MB `disk.qcow2` is retained with
a hashed completion manifest in `rb108-evidence/`.

## Not covered

- A physical multi-monitor X11 desktop (Cinnamon or Plasma). The lab has none
  besides Patrick's workstation, which these runs don't use. The Cinnamon
  guest's split RandR monitors exercise the same placement code but not two
  real outputs. virtio-gpu connects only head 0 until a UI sends size
  information; the multi-head lab work (issue #111) sends an RFB
  SetDesktopSize per head at launch, which gives two real xrandr outputs, and
  will re-check this case on that guest.
- Multi-monitor GNOME and KDE Wayland placement (all lab Wayland desktops have
  one monitor). The extension and layer-shell rules for another monitor are
  covered by `gnome_ui_ownership_contract` and `bar_placement_contract`.
- Dragging the bar on a real desktop; it is covered offscreen by
  `dragKeepsTheBarOutOfTheClip`.
