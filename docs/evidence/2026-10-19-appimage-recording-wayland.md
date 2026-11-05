# AppImage screen recording on Wayland with native windows — 2026-10-19

This record covers the nightly AppImage built from `9042776`, which contains:

- pull request #75: the recording runtime, with x264/x265 as fallbacks behind the host encoders;
- #77: Wayland region selection, the ring/dim/bar placement and the KWin version probe;
- #81: Qt's xdg-shell, decoration and EGL client plugins;
- #79: `saveCopy`/`upload` reply as soon as the export starts.

The AppImage recordings in the [packaging receipt](2026-10-18-recording-packaging.md)
ran before #77 and #81, when the image's windows still fell back to XWayland. This
campaign ran the merged image on the physical Debian Plasma Wayland and Fedora GNOME
Wayland desktops, with the Kubuntu Plasma X11 desktop as the control. The feature is
described in [recording](../recording.md) ("Wayland selection, bounds and bar",
"AppImage runtime") and the image in
[packaging/appimage/README.md](../../packaging/appimage/README.md).

## Candidate

| Field | Value |
| --- | --- |
| Source | nightly `9042776eb70ea4f8595de0bd10867411bb38525b` (tree `b01aaf81…`, clean) |
| Built by | nightly push CI run 39201599215, `packages / appimage` job (passed) |
| Artifact | `putgd-0.1.1-dev.419+g9042776eb70e-x86_64.AppImage`, 113,019,384 bytes, SHA-256 `1be3fbdcd8502d7c3fe58c8faaeed95e7aeab4def6c1df6e60dbf07d1c8ebec7` |
| Receipt | `postDeploymentAudit.qtWayland`: weston start check passed with `xdg-shell`; `recording`: GStreamer 1.24.2, bundled `x264enc` usable, export `libx264`/`libx265`/`libvpx-vp9`; `desktopAcceptance: pending` |

In the run's first attempt, `linux (26.04)` failed in `operations_contract`
(`explicitActivityCopyCanOverrideACompletedListMember`, `operation_busy`). The
second attempt passed, and the run is green. The flake is unrelated to recording and
is tracked as issue #91. The AppImage comes from the first attempt, whose
`packages / appimage` job passed.

## Method

The driver `ar_record.py` ran on the workstation through `tools/lab/physical.py`. Input
came from the host's uinput agent, and frames from the host's loopback VNC wall tile.

- **Launch.** The image ran as `IMAGE --background` in the real desktop session, with the
  environment of a desktop launch: no `QT_QPA_PLATFORM`, and on GNOME the session
  manager's `DISPLAY`/`XAUTHORITY`.
- **Private directories.** `XDG_RUNTIME_DIR`, `XDG_CONFIG_HOME`, `XDG_CACHE_HOME` and
  `XDG_STATE_HOME` were private, under `~/putgd-lab/ar`. `XDG_DATA_HOME` was private
  too, except on Plasma Wayland: KWin reads AppRun's temporary entries from the real
  one.
- **Destination.** Settings pointed every action at the local destination, with the
  archive under `~/putgd-lab/ar/archive`. Uploads therefore landed there, and the
  payloads were the test patterns.
- **Nothing installed.** The one exception is f44, below.

Each run did the following:

- **Pattern.** A fullscreen test pattern went up first:
  - Wayland: SMPTE bars and a moving ball (`videotestsrc`, `waylandsink fullscreen=true`);
  - X11: `ffplay -fs` with SMPTE bars and a moving `testsrc2` inset.
- **Region, MP4.** A rectless region was prepared, so the desktop's selector opened, and
  the area was dragged with the pointer (`[556, 302, 806, 454]`). The MP4 clip got the
  full flow: system audio, Record, Pause, Resume, Stop, review, a 0.5–4.5 s trim plus a
  timed blur, preview, Save copy and Upload.
- **Region, other formats.** WebM, GIF and WebP completed the open selector through
  `recording.select` with the same rectangle.
- **Monitor.** The whole monitor was recorded in all four formats; the MP4 again got the
  full flow.
- **Per clip.** Save copy (#79: polled until `lastExport.status` was `saved`), then Upload
  (polled `operation.inspect` until `completed`, file in the archive).
- **Windows.** At the selector, ready, recording and review steps, the driver recorded:
  - which Qt platform, shell and decoration plugins `putgd-record`, `putgd-ui` and
    `putgd-kde-region` mapped (`/proc/PID/maps`);
  - which display sockets they were connected to (the server ends that name their socket
    inodes as peers);
  - any X11 window they owned (`xwininfo`, or `xprop` on f44).
- **Motion.** Scored afterwards on the builders with `putgd-record --motion CLIP 8`, from
  a distro-FFmpeg build on d69 (scratch build `7f09434`; the `--motion` and preview code is
  unchanged up to `9042776`). WebP was scored with Pillow on k72 by the same measure.
  The image's own `putgd-record` can't read its exports (issue #92). Each pattern run was
  repeated as a static control.

Two pattern pairs were used:

1. **ball / desktop:** the ball pattern against the bare desktop (`static`).
2. **scroll / frozen:** bars that scroll sideways at 6 px per frame plus the ball, against
   the same bars standing still.

The second pair is decisive. On the Wayland desktops the ball often missed the recorded
rectangle, and in the region MP4 the blur switching off at 3 s adds a score of its own.
On f44 the textured wallpaper, re-encoded live by software x264 under load, scored
higher as a "static" desktop than the flat bars did with the ball. The first pair is kept
for completeness; the tables give both.

## Hosts

| Host | Session | Notes |
| --- | --- | --- |
| d69 `.69`, Debian 13.7, kernel 6.12.107 | Plasma 6.3.6, KWin 6.3.6 Wayland, 1920x1080, Intel iHD | load 0.0–1.9 |
| f44 `.70`, Fedora 44, kernel 6.19.10 | GNOME Shell 50.0, mutter 50.0 Wayland, Xwayland 24.1.9, 1920x1080; stock driver, no H.264 encode | load 1–11 from another session's VM work (a `qemu-system` guest and `qemu-img compare`) that was already running when the slot started |
| k72 `.72`, Kubuntu 26.04.1, kernel 7.0.0 | Plasma 6.6.6, KWin 6.6.4 X11, 1920x1080, Intel iHD | load 0.1–2.7 |

## Results

All runs passed every functional step. That's 89 clips: 80 in the ten full sets, 8 in
f44's first set with the dashboard open, and the extra f44 region GIF. Each clip was
prepared, reached ready, recorded, stopped into review, saved a copy and uploaded to the
local archive.

| | d69 Plasma Wayland | f44 GNOME Wayland | k72 Plasma X11 (control) |
| --- | --- | --- | --- |
| Acquisition | `kde-screencast` v5 through AppRun's temporary entry; `kwinVersion 6.3.6`, `hidesRequester: false` | `mutter-screencast` | `x11` |
| Live encoder | host `vah264lpenc` + `voaacenc` | bundled `x264enc` + `voaacenc` | host `vah264lpenc` + `voaacenc` |
| Region selector | the helper's KDE layer-shell selector | the put.gd Shell extension's `SelectArea` | the X11 drag surface |
| Region placement | `outside`, `overlayExcluded: true`; 806x454 canvas = selection | `outside`, `true`; 806x454 | `outside`, `true`; 806x454 |
| Monitor placement | `inside`, `false` (KWin 6.3 keeps the requester's windows in its stream) | `inside`, `false` (Mutter has no per-window exclusion) | `inside`, `false` |
| Helper windows | native Wayland: `libqwayland-generic`, `libxdg-shell`, `libLayerShellQtInterface`; only `wayland-0` sockets; no X11 window | native Wayland: `libqwayland-generic`, `libxdg-shell`, `libLayerShellQtInterface`, and Qt's `adwaita` decoration for the review; only `wayland-0`; no X11 window | `libqxcb`; X11 windows `put.gd recording controls`, `Review recording` |
| Helper peak (VmHWM) | 106–114 MiB | 83–129 MiB | 109–119 MiB |

Every placement matches docs/recording.md for that compositor and KWin version. KWin
6.6's requester hiding (`hidden`) was not reachable: no physical desktop runs Plasma 6.6
on Wayland.

Frames:

- d69: [ready ring and bar](appimage-recording-wayland/d69-region-ready-ring-bar.png),
  [recording, outside dimmed](appimage-recording-wayland/d69-region-recording-dimmed.png),
  [review (KWin's Breeze frame)](appimage-recording-wayland/d69-review-native.png), and
  [two monitor clip frames](appimage-recording-wayland/d69-monitor-clip-frames.png) at
  0.5 s and 2.5 s. The ball moved between them, and the bar and an earlier upload's
  notification are inside the clip.
- f44: [Shell selector while dragging](appimage-recording-wayland/f44-shell-selector-dragging.png),
  [ready ring and bar](appimage-recording-wayland/f44-region-ready-ring-bar.png),
  [recording, outside dimmed by the Shell](appimage-recording-wayland/f44-region-recording-dimmed.png),
  [review (Qt's adwaita frame)](appimage-recording-wayland/f44-review-adwaita.png).
- k72: [X11 recording](appimage-recording-wayland/k72-x11-region-recording.png).

Export copies took 0.7–3.4 s for a region and 1.8–13.4 s for a monitor; the slowest is
the 1920x1080 GIF. Every upload's operation `completed`, and the file appeared in the
archive.

### Motion

Scores are `--motion` with 8 samples: mean absolute grey difference between consecutive
samples at 320 px. Each cell gives pattern / control for the same target and format.

| Desktop, pair | Region MP4 | Region WebM | Region GIF | Region WebP | Monitor MP4 | Monitor WebM | Monitor GIF | Monitor WebP |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| d69, scroll / frozen | 30.991 / 0.051 | 44.284 / 0.001 | 48.685 / 0.000 | 47.295 / 0.000 | 25.959 / 0.078 | 33.135 / 0.056 | 33.898 / 0.051 | 34.673 / 0.081 |
| d69, ball / desktop | 0.122 / 0.021 | 0.106 / 0.002 | 0.072 / 0.000 | 0.608 / 0.000 | 0.158 / 0.118 | 0.193 / 0.108 | 0.148 / 0.099 | 0.194 / 0.116 |
| f44, scroll / frozen | 38.900 / 0.059 | 24.759 / 0.004 | 22.383 / 0.000 | 21.745 / 0.004 | 26.826 / 0.052 | 35.485 / 0.007 | 35.290 / 0.002 | 37.996 / 0.032 |
| f44, ball / desktop | 0.165 / 0.225 | 0.166 / 0.206 | 0.000 / 0.121 | 0.444 / 0.106 | 0.177 / 0.172 | 0.092 / 0.245 | 0.109 / 0.147 | 0.138 / 0.081 |
| k72, moving inset / desktop | 6.653 / 0.066 | 6.840 / 0.010 | 4.132 / 0.002 | 3.531 / 0.005 | 4.857 / 0.095 | 5.444 / 0.072 | 5.023 / 0.065 | 5.505 / 0.077 |

- With the scroll pattern, every clip on both Wayland desktops scores hundreds of times
  its frozen control. The clips captured motion over time, through the KWin and Mutter
  streams and every export format.
- In the f44 ball run, the ball didn't cross the region during the GIF (0.000). A rerun
  with the scroll pattern scored 36.461.
- f44's first ball set (`pattern-dashboard-open`) recorded the dashboard (below), and
  is kept only for its placement and window checks.

## Findings

- **f44 had no tray host after the first re-login.** AppIndicator
  (`appindicatorsupport@rgcjonas.gmail.com`) had been enabled at runtime only. The
  persistent `enabled-extensions` list, last written at 02:56Z before this slot, did not
  contain it, so the re-login dropped it. Without a tray host the resident opens its
  dashboard at start, and in the first f44 set it covered the pattern
  ([frame](appimage-recording-wayland/f44-no-tray-dashboard-over-pattern.png)). That is
  the documented no-tray behaviour, not a defect. Later runs closed that window (the
  exact `putgd-ui` PID), and it did not reopen during recording. With Patrick's OK in this
  session, AppIndicator was added to `enabled-extensions` before the second re-login. It
  was ACTIVE afterwards, with the StatusNotifier host registered.
- **The bar placement worked with native windows on GNOME.** The extension accepts only a
  `put.gd recording controls` window whose process is a direct child of the
  authenticated resident. With the native Wayland bar, Mutter's `get_pid()` matched the
  helper (ppid = resident), and the bar was moved below the area. This is the same PID
  path that issue #88 lists as unchecked for the screenshot own-window refusal; that
  refusal itself was not exercised.
- **The AppImage's `putgd-record` diagnostics can't read exports.** The pinned FFmpeg
  has only the Matroska demuxer and the H.264/AAC decoders. So `--motion`, `--preview`
  and `--av-sync` fail on the saved MP4/GIF/WebM/WebP copies, and `--av-marker` lacks
  its encoders. Product recording, review and export are unaffected. This is issue #92;
  this campaign scored clips with a distro build instead.
- **Completion notifications show in monitor clips.** The local destination's "put.gd —
  completed" notification appears on screen, so a monitor clip started right after an
  upload contains it on KWin 6.3 and GNOME, which do not exclude anything from a monitor
  stream.
- **Driver fixes during the campaign.** Each failed attempt is kept.
  - Attempt 1 on d69 died parsing `putgd-cli --version-json`, which prints multi-line
    JSON. Its resident was quit through its own socket.
  - The first d69 runs ended with an SSH error in teardown, caused by `set -e` on an
    empty `ls`. Cleanup was then checked by hand.

## Desktop changes and cleanup

- **d69.** AppRun's two temporary entries (`putgd-appimage-*`,
  `putgd-record-appimage-*`) existed while the image ran and were gone after it quit.
  Afterwards `~/.local/share/putgd`, created by these runs, was removed and
  `kbuildsycoca6 --noincremental` run. Locks: 05:05–05:15Z and 05:58–06:06Z.
- **f44.**
  - The put.gd Shell extension from `9042776` (`integrations/gnome`, SHA-256 matching) was
    a user copy in `~/.local/share/gnome-shell/extensions/putgd@put.gd` from 05:26Z to
    05:56Z.
  - GDM re-logins (autologin) loaded it at 05:32:03Z and unloaded it at 05:57:24Z.
    Before each, the coordinator had notice and the session held none of Patrick's apps.
  - Afterwards `gnome-extensions info putgd@put.gd` reports that it doesn't exist, as
    before the slot.
  - Lock 05:25–05:58Z.
- **k72.** Nothing was written outside `~/putgd-lab/ar`. Patrick's notification was left
  alone. Lock 05:16–05:25Z.
- **Every host.** The input agent (`putgd-lab-input-ar`) and its forward were stopped.
  Each lock was released with `desktop_lock.py release --verified-clean`; the receipts are
  in `~/putgd-lab/locks/history/` on each builder.

## Not run

- A/V sync of the recorded system audio. MP4/WebM clips carried `voaacenc` audio, but no
  marker was played or measured.
- The portal path, `window` and `desktop` targets, and Escape in the selector with the
  AppImage.
- KWin 6.6 Wayland (`hidden` placement) on physical hardware, and multi-monitor
  placement.
- GPU-encoded export on AMD or NVIDIA; only Intel iHD was available, and f44's stock
  driver has no H.264 encode.
- An integrated AppImage install (host desktop entry); the window icons from issue #88.
- Cinnamon, the Ubuntu 26.04 GNOME/KDE Wayland VMs, MCP end to end, and family memory
  (only helper peaks are recorded).

## Private evidence

`work/verification/2026-10-19-appimage-recording-wayland/` in the branch worktree holds:

- the driver, input, extension, scoring, summary and frame-export scripts;
- the host configurations;
- per run (`d69/`, `f44/`, `k72/`; `pattern`, `static`, `scroll`, `frozen`,
  `pattern-dashboard-open`, `no-extension`, `region-gif-scroll`, `attempt1-version-json`):
  `results.json`, `run.log`, capabilities, wall frames, previews, operations, every clip
  and `motion.json`;
- the console outputs.

The CI artifact and the downloaded failed-job context are in `work/ar/ci-9042776/`.
