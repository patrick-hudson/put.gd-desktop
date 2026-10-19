# Screen recording acceptance sweep (#87) — 2026-10-19

The lab-reachable checks from issue #87 and its comment, run on current
nightly builds. No product code changed; every defect found is its own issue.
Each run used a moving test pattern with a scored control, saved frames and
recorded identities. Desktop slots were held under `tools/lab/desktop_lock.py`
and released `--verified-clean`.

## Builds

| Build | Source | Assets (sha256) |
| --- | --- | --- |
| nightly `5ac6bbc` (includes #118) | CI run 39325350860 | AppImage `putgd-0.1.1-dev.494+g5ac6bbc594e1-x86_64.AppImage` `ebb9fa50…a6`; Ubuntu 26.04 DEB `0f54bff2…3b` |
| nightly `8e568fd` (includes #121, the #119 WebM Opus fix) | CI run 39336656763 | Fedora 44 RPM `4878d6ad…6b`; Ubuntu 26.04 DEB `afdb0697…b4`; AppImage `45934357…68` |

Desktops: k72 Kubuntu 26.04.1 Plasma 6.6.6 X11 (kernel 7.0.0-38, `.72`), d69
Debian 13 Plasma 6.3 Wayland (6.12.107, `.69`), f44 Fedora 44 GNOME Shell 50.0
Wayland (6.19.10, `.70`). Guests (KVM, `tools/lab/lab.py` at `5ac6bbc`):
Ubuntu 26.04.1 GNOME Shell 50.1 Wayland (golden `563730cc…`, on f44 and d69)
and Kubuntu 26.04.1 Plasma 6.6.6 Wayland (golden `49fcacff…`, on d69).

## Method

- **Resident.** The AppImage ran as `IMAGE --background` with private XDG
  directories under `~/putgd-lab/acc87` (the real data home only for d69's
  `kde-screencast` runs, where KWin must see AppRun's temporary entry); the
  packages ran their installed `putgd-launcher --background` the same way. All
  actions pointed at the local destination, except item 7.
- **A/V.** The image's own `putgd-record --av-marker` clip played full screen
  through GStreamer (`waylandsink`/`xvimagesink`, `pulsesink` to the built-in
  speakers at 40%) while the production CLI recorded the monitor (GNOME portal:
  the player's window) with system audio at 30 fps. The helper kept each working
  clip (`PUTGD_RECORD_KEEP_WORKING`); `recording.saveCopy` wrote the MP4 or
  WebM the way the review does; the image's own `putgd-record --av-sync`
  (`APPDIR` set) measured both. Offsets are audio minus video, positive when the
  sound is late, one frame = 33.3 ms; the first pair is dropped (player start).
  Pause runs paused 3 s halfway.
- **Recording flow.** `acc_rec.py` (from the 2026-10-19 AppImage campaign):
  a scrolling SMPTE pattern with a moving ball, a dragged region (full flow:
  pause, resume, trim, timed blur, preview, Save copy, Upload) and regions
  completed with `recording.select`, plus the whole monitor, in MP4, WebM, GIF
  and WebP. Motion is `putgd-record --motion CLIP 8` (Pillow for WebP) against
  the same bars standing still.
- **MCP.** `mcp_record_e2e.py`: the official MCP SDK client → locked
  `putgd-mcp` (workstation) → the target's production `putgd-cli` over pinned
  SSH → resident. 20 checks: capabilities, `recording_active` refusal, monitor
  MP4 with system audio, pause/resume (pause removed from the clip), stale edit
  refused, trim + blur, `preview_recording` as an MCP image, Save copy (motion
  scored), overwrite refused, upload completed, session ends; a GIF region
  completed through `select_recording_region`, saved, scored, uploaded; cancel
  stores nothing.

## 1. AppImage A/V re-measure (nightly `5ac6bbc`)

Exported file medians (ms), with the working clip in brackets and the host's
1-minute load during the run:

| Desktop / path | MP4 | WebM | Load |
| --- | --- | --- | --- |
| k72 Plasma 6.6 X11 (`ximagesrc`, `vah264lpenc`) | +6.0 [+6.0], +22.7 [+23.0] | +16.5 [+16.0] | 0.9–2.3 |
| d69 Plasma 6.3 Wayland `kde-screencast` (`vah264lpenc`) | +26.0 [+26.0], +20.0 [+20.0] | +26.5 [+26.0] | 2.4–4.0 |
| d69 portal (monitor, one KDE picker click per run, "Allow restoring" off) | +19.3 [+19.0], +9.3 [+9.3] | +21.5 [+21.0] | 1.0–1.2 |
| f44 GNOME 50 `mutter-screencast` (bundled `x264enc`) | −8.0 → +25.3 step, +12.0 [+12.0] | +13.5 [+12.7] | 0.3–1.2 |
| f44 portal (window; GNOME picker, "Remember" off) | −24.0 [−23.7] | +1.5 [+1.3] | 1.4–1.5 |
| f44 RPM `8e568fd`, `mutter-screencast` (`openh264enc`) | +10.0 [+9.7] | −2.5 [−3.0] | 1.1–1.7 |

- Every result is within ±27 ms, inside the usual −125/+45 ms tolerance;
  working clip and export agree within 1 ms on every run without a pause.
- The AppImage reads 10–30 ms later than #118's distro scratch builds on the
  same desktops (d69 `kde-screencast` −8.7/+4.7 there, k72 −12/−18). That is
  within one frame and was not investigated further.
- The GNOME portal window stream alternates by one frame between pairs (as
  #118 saw: damage-driven stream), so its medians swing between runs. One f44
  Mutter run stepped by one frame halfway with no pause.
- Motion of the marker clips is not a pattern score (black between flashes);
  each A/V session also carries a static control: k72 0.000, d69 0.000.

## 2. The AppImage on Plasma and GNOME Wayland with native windows (#81)

- **d69 Plasma 6.3 Wayland, AppImage `5ac6bbc`.** Regions (dragged in KWin's
  layer-shell selector, then `recording.select`) and the monitor in MP4, WebM,
  GIF and WebP, all saved and uploaded. `putgd-record` mapped
  `platforms/libqwayland-generic.so`, `libxdg-shell.so` and LayerShellQt, and its
  only display socket was `wayland-0` (no X11). `overlayPlacement`: region
  `outside`, monitor `handle`. Motion 14.1–35.1 (scroll) against a frozen
  control of 0.051–0.064.
  - The first monitor set ran while two other sessions' builds loaded d69 (load
    9–11); the pattern window froze on screen and the clips scored 0.15–0.20.
    It was redone on the quiet host (26.7–35.1). The region WebP from that
    window has one frame (identical frames collapse); it was not redone on d69,
    and region WebP passed on f44 and both guests.
- **f44 GNOME 50 Wayland, AppImage `5ac6bbc`.** The monitor in all four formats:
  `libqwayland.so`, `libxdg-shell.so`, the review window decorated by
  `libadwaita.so`, no X11 socket; motion 27.4–37.4 against a frozen control
  of 0.019. Rectless regions need the Shell extension, which ran from the
  RPM in item 3.

## 3. The packaged GNOME extension's recording path (f44, RPM `8e568fd`)

The RPM installed in a one-package transaction, followed by a re-login (GDM
autologin restart). `gnome-extensions info putgd@put.gd`: version 2, `ACTIVE`,
path `/usr/share/gnome-shell/extensions/putgd@put.gd`; AppIndicator stayed
`ACTIVE`. The installed resident recorded a region dragged in the Shell
extension's selector (full flow), regions completed with `recording.select`, and
the monitor, in all four formats: every clip saved and uploaded, live encoder
`openh264enc` (Cisco's, through the RPM's Recommends), region `outside`, motion
21.7–38.2. Afterwards the RPM was removed and a second re-login restored the
starting state: `putgd@put.gd` absent, AppIndicator `ACTIVE`, the
enabled-extensions list unchanged.

## 4. MCP end to end

| Desktop | Build | Acquisition | Result |
| --- | --- | --- | --- |
| k72 Plasma 6.6 X11 | AppImage `5ac6bbc` | `x11` | 20/20; monitor copy motion 46.1, GIF region 14.2 |
| d69 Plasma 6.3 Wayland | AppImage `5ac6bbc` | `kde-screencast` | 20/20; 43.5, 14.7 |
| f44 GNOME 50 Wayland | RPM `8e568fd` | `mutter-screencast` (region through the packaged extension) | 20/20; 43.7, 24.9 |

The first k72 attempt failed only the driver's own pause bound (7.9 s clip;
over SSH each call adds about 0.5 s); the check now compares the clip with the
measured wall time minus the pause, and the rerun passed.

## 5. Ubuntu 26.04 GNOME and Plasma Wayland guests

- **Ubuntu 26.04.1 GNOME 50.1 (f44 host), DEB `5ac6bbc`.** `mutter-screencast`,
  live `x264enc`. Monitor and region in all four formats, saved and uploaded:
  motion 29.9–58.9, region `outside`. The portal path (window target, GNOME
  picker in the guest) recorded the pattern window: motion 61.3.
  The golden carries an older put.gd whose Shell extension (version 1) stayed
  loaded after the DEB install, so regions failed `region_unavailable` until a
  reboot loaded version 2 (#131).
- **Kubuntu 26.04.1 Plasma 6.6.6 (d69 host), DEB `8e568fd`.**
  `kde-screencast`, live `x264enc`. Regions (layer-shell selector and
  `recording.select`) and the monitor in all four formats: motion 7.4–16.2,
  region `outside`, monitor `hidden` (bar off the recorded output).
  WebM stops hung in 3 of 5 runs (`recording_helper_timeout`, #129); the
  following recordings worked. No desktop frames were saved for this guest
  (Pillow missing on d69); the clips' preview frames are kept, and the 2-head
  run below saved frames of the same golden and DEB.

## 6. Multi-monitor placement on Wayland (`lab.py --heads 2`)

Heads `1920x1080+0+0` (Virtual-1, primary) and `1280x1024+1920+0` (Virtual-2),
verified by `lab.py layout` (kscreen). The pattern ran full screen on head 0;
head 1 showed only its wallpaper and serves as the static control.

| Plasma 6.6.6 guest, DEB `8e568fd` | Canvas | `overlayPlacement` | Motion |
| --- | --- | --- | --- |
| monitor `Virtual-1` | 1920x1080 | `outside` | 4.088 |
| monitor `Virtual-2` | 1280x1024 | `outside` | 0.013 |
| region `[480,270,960,540]` on head 0 | 960x540 | `outside` | 9.031 |
| region `[2240,256,640,512]` on head 1 | 640x512 | `outside` | 0.020 |
| region `[1720,200,400,300]` across both | 400x300 | `outside` | 4.942 |

The preview frames show each output's own content (pattern on Virtual-1,
head 1's wallpaper on Virtual-2), and the spanning region shows head 0's right
edge beside head 1's wallpaper. The ring and bar were drawn on the head that
holds the area (frames of both heads at ready and while recording).

| Ubuntu 26.04 GNOME 50.1 guest, DEB `8e568fd` (`mutter-screencast`, extension v2 after a reboot) | Canvas | `overlayPlacement` | Motion |
| --- | --- | --- | --- |
| monitor `Virtual-1` | 1920x1080 | `outside` | 27.904 |
| monitor `Virtual-2` | 1280x1024 | `outside` | 0.109 |
| region on head 0 | 960x540 | `outside` | 36.255 |
| region on head 1 | 640x512 | `outside` | 0.184 |
| region across both | 400x300 | `outside` | 21.235 |

On GNOME the ring, dimming and bar followed the area too: the head-1 region's
bar sat under it on head 1, and the spanning region's bar went below it on
head 1. Mutter rejects gaps between heads, so only an adjacent layout was run.
The first run of each guest had no pattern (the Kubuntu golden has no
`gst-launch-1.0`, the Ubuntu one no `ffplay`); those attempts are kept and the
reruns use whichever player the guest has. Pointer input per head isn't mapped
on Wayland guests (see [VM lab](../vm-lab.md#multi-head-guests)), so regions
were completed with `recording.select`, not dragged.

## 7. Browser playback of real share URLs

On Patrick's instruction ("#87 share-link test: use bucket put.gd under prefix
acceptance/87-20261019/, synthetic clips only, deleted afterwards through
put.gd's history deletion"). Patrick typed a dedicated IAM key scoped to that
prefix into put.gd's Settings on f44 himself; the profile used `session`
credentials, so nothing was written to a keyring, and no command, log, frame
or file of this campaign contains a key. The first key he tried failed
Test connection (403 on the object check) and was not used.

- Profile: S3, bucket `put.gd`, `us-east-2`, path addressing, `public-read`,
  link domain `https://put.gd`, folder `acceptance/87-20261019/`.
  `setup.testDestination` passed.
- RPM `8e568fd` recorded the scrolling pattern (5.7 s) and uploaded one clip per
  format: MP4 351,028 B, WebM 296,640 B, GIF 2,170,113 B, WebP 252,986 B.
  Each URL answered 200 with `video/mp4`, `video/webm`, `image/gif` and
  `image/webp` and its exact length, and range requests answered 206.
- Firefox (Fedora 44) and Chromium 155 (installed for the test, removed
  afterwards), kiosk mode, throwaway profiles: all eight loads played. Two frames
  1.5 s apart differ by 32–71 (mean absolute luma).
  - The first Chromium attempt raised GNOME's "login keyring" password prompt
    (Chromium's password store); the rerun used `--password-store=basic`.
- Deletion through put.gd: `history.list` (profile filter) → `delete.prepare`
  (4 targets, "guarded origin removal") → `delete.confirm`. Three objects were
  removed at once; the WebP target came back `unknown` (`s3_transport_unknown`)
  and `delete.retry` removed it. All four history records read `removed`, and
  the S3 origin answers 403 for all four keys. `https://put.gd/…` kept serving
  CloudFront's cached copies (Age ~11 min), as put.gd's deletion notice says;
  no invalidation was made.
- The test profile was removed; the Secret Service search for put.gd items
  returned none.
- With #121 merged (2026-10-19 22:40:58Z), the WebM ran on a build with the
  Opus fix and played in both browsers.

## 8. Pause/resume A/V sync

Before → after the paused gap (ms), working clip [exported file]:

| Desktop / path | MP4 | WebM |
| --- | --- | --- |
| k72 X11 | +20.0 → +17.0 [+20.0 → +6.7] | +2.9 → +16.0 [+3.5 → +6.5] |
| d69 `kde-screencast` | +35.0 → +65.0 [+35.3 → **+55.3**] | +18.0 → +31.0 [+18.5 → +21.5] |
| d69 portal | +21.0 → +5.0 [+21.3 → −5.3] | — |
| f44 Mutter | −4.0 → −11.0 [−4.0 → −20.7] | 0.0 → +3.0 [+0.5 → −6.5] |
| f44 portal (window) | +22.7/−10.7 → +3.3/+36.3 [+22.7/−10.7 → −7.3/+26.0] | — |

Sync holds within each segment, but the resume steps the working clip by −16 to
+30 ms, and the export moves the post-resume part a further ~10 ms earlier
on every desktop and in both formats. One export ended at +55.3 ms. Filed as
#130.

## Also seen

- **Stop hang** (`recording_helper_timeout`, no `timestamps:` line after `command
  stop`) in both guests: GNOME 2 of 5 frozen-screen runs with pause/resume,
  Plasma 3 of 5 WebM runs (moving pattern) and 1 frozen MP4; never on the
  physical desktops (about 60 recordings with system audio). Both guests have
  only PipeWire's `auto_null` sink. #129.
- **Stale extension message** after a package upgrade on GNOME: #131.
- On the GNOME desktop (resident launched over SSH), uploads to the local
  destination reported "Clipboard: failed" in the notification; on Plasma they
  reported "copied". Not investigated (no focus for the Wayland clipboard).

## Not run (blocked)

- AMD and NVIDIA GPU encoding: the lab's hardware H.264 is Intel only.
- The Ubuntu 24.04 DEB on physical Mint Cinnamon: there is no physical Mint
  machine.
- A listening test with real headphones, and Bluetooth or other sinks: all
  measurements used the built-in speakers' sink monitor.
- A physical Plasma 6.6 Wayland session: Patrick approved switching k72 from X11
  for it, but k72's wall tile is an X11-only server (`X0tigervnc` on `:0`), so the
  session would not be visible on the wall. He chose to set up a Wayland VNC
  server there first (TigerVNC `w0vncserver`, as on d69); that setup is pending,
  and the guest rows above stay the Plasma 6.6 Wayland evidence until then.

## Lab changes, undone

- f44: RPM `putgd` and Chromium (`chromium`, `chromium-common`,
  `google-crc32c`, `libXNVCtrl`) installed and removed; two GDM re-logins
  (Patrick's apps checked first); put.gd notifications cleared; the test profile
  removed.
- d69: AppRun's temporary `putgd-record-appimage-*.desktop` entry existed only
  while the image ran. The real-data runs created `~/.local/share/putgd` (only
  this campaign's synthetic artifacts); it was moved to
  `~/putgd-lab/acc87/realdata-local-share-putgd`.
- Guests: every overlay retired through `lab.py cleanup` with a completion
  manifest; the goldens re-hashed to their inventory values (`563730cc…`, `49fcacff…`; see
  [remote golden images](../remote-golden-images.md)).
- Wall tiles for the guests were added and removed.

Visuals: [recording-acceptance-87/](recording-acceptance-87/). Raw driver
output, clips and logs are private under the campaign's `work/acc87`.
