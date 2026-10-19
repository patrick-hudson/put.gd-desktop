# Screen recording in every package (issue #50 packaging) — 2026-10-18

This record certifies what pull request #75 (`claude/recording-packaging` into
`nightly`) built and ran. It adds the AppImage recording runtime, has the probe
test that H.264 actually works with an actionable message when it does not,
makes the RPM recommend Cisco's OpenH264, and checks recording from installed
packages and the AppImage. The feature is described in
[recording](../recording.md) ("Packages and codecs", "AppImage runtime").

## Candidates

| Commit | Built by | Artifacts (SHA-256) |
| --- | --- | --- |
| `9462fe7` (CI merge `7237d0e`, `0.1.1-dev.371+g7237d0ec411f`) | pull request CI | Ubuntu 26.04 DEB `a08a93b3…`, Fedora 44 RPM `f4a24f62…` |
| `8385daf` (CI merge `ba7153d`, `0.1.1-dev.372+gba7153d34184`) | pull request CI | AppImage `b538b095…` (109,390,328 bytes), Fedora 44 RPM `ce71d1a4…`, Ubuntu 24.04 DEB `a34f2759…`, Ubuntu 26.04 DEB `6db352d2…` |
| `8385daf` (`0.1.1~dev.371+g8385dafa0f7f`) | Debian 13 builder `.69`, `tools/releases/native.py` (release preset, no lab fixture) | Debian 13 DEB `a862cf43…` |
| `399d491` (CI merge `dd0e3d7`, `0.1.1-dev.373+gdd0e3d797c7d`), the final code | pull request CI | AppImage `b5dca870…` (109,390,328 bytes), Ubuntu 26.04 DEB `cdee69c6…`, Fedora 44 RPM `a84cd380…`, Ubuntu 24.04 DEB `49410533…` |

`9462fe7`, `8385daf` and `399d491` differ only in the AppImage assembly, in
how the helper recognises the image for its hint and in one probe sentence
("not available here" instead of "not installed"); the recording code is the
same. The final candidate's AppImage was rerun on the Debian desktop (row
marked `399d491`), and its probe on Fedora (extracted, `APPDIR` set) gave the
refusal with the new sentence and the AppImage hint. Its Kubuntu reruns are
void (see the GPU hang below), and the Fedora desktop was booked by another
session.

## Suites

- Workstation (Mint 22.3), `ctest --preset dev` on the final code: 74/74
  passed in 719 s (run before the lab's no-full-suites-on-the-workstation rule
  was announced). The recording contracts (`recording_model_contract`,
  `recording_service_integration`, `recording_helper_integration`,
  `record_runtime_contract`, `portal_streams_contract`,
  `wayland_acquisition_contract`) also pass on repeated runs. The release
  contract group (`tools/ci/contracts.py release`, including
  `test_portable_appimage.py` with its new AppRun and audit cases) passes.
- Pull request CI on `8385daf` and on `399d491`: contracts, Linux 24.04 and 26.04, the Windows
  portable build, the three native packages and the AppImage (whose assembly
  audit now runs the relocated helper's probe) all passed.

## Probe on real hosts

`putgd-record --capabilities` from scratch builds of this branch:

| Host | Result |
| --- | --- |
| Fedora 44 `.70`, Cisco `openh264` installed | `available`, `mutter-screencast`; live `openh264enc`; export `libopenh264` and `libvpx-vp9`; `h264_vaapi`/`hevc_vaapi` "Function not implemented" (stock iHD 25.4.6), NVENC absent |
| Same host, `noopenh264` 2.6.0 placeholder first on the library path | `h264_encoder_unusable`: `openh264enc` "Could not initialize supporting library", FFmpeg's `libopenh264` decoder and encoder fail to open, MP4 dropped from `formats`; message and the `dnf install openh264 gstreamer1-plugin-openh264` hint |
| Mint 22.3 workstation | `available` (`vaapih264enc`, `x264enc`, `openh264enc`; export `h264_nvenc`, `libx264`, `h264_vaapi`, `hevc_nvenc`, `libx265`); a warm probe takes 0.24-0.27 s before and after the change |

The relocated AppImage helper (extracted, `APPDIR` set) loaded GStreamer 1.24.2
plugins only from the image on the workstation, Fedora, Debian and Kubuntu, and
found `vah264lpenc`/`vaapih264enc` through each host's own libva and iHD driver
on the workstation, Debian and Kubuntu; on stock Fedora it reports
`live_encoder_missing` with the AppImage hint.

## Installed packages and the AppImage on the physical desktops

Driven through `tools/lab/physical.py` with the production CLI, private XDG
directories under `~/putgd-lab/rp`, a local destination and archive there, a
fullscreen `videotestsrc pattern=ball` window, and saved screenshots
(ready, recording, review, edited review, after upload). The first format runs
the full flow (prepare, start, pause, resume, stop, trim 0.5-4.5 s plus a
timed blur, preview, save copy, upload); the others record, save a copy and
upload. Every listed upload completed and the file appeared in the archive.
Motion scores (`putgd-record --motion`, Pillow for WebP) against static
controls from the Kubuntu desktop of 0.054 (WebM) and 0.106 (MP4 with the blur
switching off mid-clip):

| Desktop | Package | Acquisition, live encoder | MP4 | WebM | GIF | WebP | Helper peak |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Kubuntu 26.04.1 Plasma 6.6.6 / X11, `.72` | Ubuntu 26.04 DEB `9462fe7` | `x11`, `vah264lpenc` + `avenc_aac` | 0.272 | 0.347 | 0.340 | 0.389 | 142-146 MiB |
| Kubuntu 26.04.1 Plasma 6.6.6 / X11, `.72` | AppImage `8385daf` | `x11`, `vah264lpenc` + `voaacenc` (bundled) | 0.251 | 0.253 | 0.208 | 0.428 | 96-105 MiB |
| Debian 13 Plasma 6.3.6 / Wayland, `.69` | AppImage `8385daf` | `kde-screencast` v5 through AppRun's entry, `vah264lpenc` | 0.235 | 0.203 | 0.244 | 0.269 | 99-102 MiB |
| Debian 13 Plasma 6.3.6 / Wayland, `.69` | AppImage `399d491` | same | 0.283 | 0.280 | 0.259 | 0.291 | about 105 MiB |
| Fedora 44 GNOME 50 / Wayland, `.70` | Fedora 44 RPM `9462fe7` | `mutter-screencast`, `openh264enc` + `avenc_aac` | 0.279 | 0.280 | 0.279 | 0.285 | 148-153 MiB |
| Fedora 44 GNOME 50 / Wayland, `.70` | Fedora 44 RPM `9462fe7` | `portal` (one Share click in GNOME's picker) | 0.281 | | | | 152 MiB |
| Fedora 44 GNOME 50 / Wayland, `.70` | AppImage `8385daf` | refused: `live_encoder_missing`, the AppImage hint | | | | | |

Other checks on the same desktops:

- KWin authorizes the installed helper: with the Debian DEB installed and no
  user entries, `/usr/share/applications/putgd-record.desktop` gave
  `kde-screencast` (protocol v5, `HDMI-A-1`).
- AppRun on Plasma wrote `putgd-record-appimage-XXXXXXXX.desktop`
  (`Exec="/tmp/.mount_putgd-…/usr/bin/putgd-record"`,
  `X-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1`) next to the
  ScreenShot2 entry, and both were gone after the resident quit.
- Fedora placeholder in the real UI: the installed RPM's resident started with
  `noopenh264` first on its library path reported `h264_encoder_unusable`,
  `recording.prepare` returned `recording_unavailable` with the message and
  hint, and **Record screen** on the dashboard showed both in a warning dialog
  (screenshot `f44/rpm-stub/02-after-record-click.png`).
- Stock Fedora 44 (`registry.fedoraproject.org/fedora:44` containers on `.70`,
  RPM `8385daf`): a default `dnf install` pulled `gstreamer1-plugin-openh264`
  and Cisco's `openh264` through the RPM's Recommends and the probe found
  `openh264enc` and `libopenh264` usable; with `install_weak_deps=False` the
  probe reported `live_encoder_missing` with the Fedora hint.
- Debian 13 cannot install either Ubuntu DEB (`apt-get install --simulate`:
  `libavcodec60` / `libavcodec62` unsatisfiable); the Debian rows use the
  Debian-built DEB.

## AppImage size and memory

| | nightly 317 | this change |
| --- | --- | --- |
| AppImage bytes | 98,929,144 | 109,390,328 (+10,461,184) |
| Idle family on Debian Plasma/Wayland, RSS / PSS (median of 3) | 111.0 / 75.4 MiB | 111.5 / 75.9 MiB |
| Resident RSS | 29.0 MiB | 29.3 MiB |

The family is the runtime daemon, AppRun, the resident and `putgd-ui`, 30 s
after `--background`. The bundled helper peaks at 96-108 MiB while recording,
against 142-156 MiB for the distro packages' helper.

## Findings and gates not run

- **Kubuntu GPU hang.** At about 21:32 CDT, during a recording with
  `vah264lpenc` while the host ran at load 9 from other sessions' builds, the
  i915 driver reset Xorg's context ("GPU HANG ... in Xorg"). The display froze.
  `kwin_x11 --replace` restored the picture, but `ximagesrc` kept reading stale
  root-window frames (still clips, or flicker between stale and black frames),
  so every Kubuntu run after 21:32 is void and kept apart as
  `k72/*-invalid-*`. The earlier Kubuntu rows above predate it. The same host
  logged an earlier Xorg GPU hang at uptime 6,125 s, before this campaign.
  Restarting Xorg would end the desktop's running session, so it was left for
  Patrick.
- In the AppImage, `putgd-ui --image-job` (the worker that previews non-PNG
  images) aborted twice per run on Kubuntu/X11 when GIF and WebP uploads were
  previewed. The uploads completed, without a history preview. The DEB did not.
  A separate AppImage `putgd-ui` abort was recorded on Fedora at resident
  start. Both are proposed as their own fix.
- `putgd-cli` stops waiting after 15 s without a reply, while `saveCopy`
  answers only when the export ends. A GIF export slowed by host load reported
  `transport_failed` while it carried on (`k72/deb-final-invalid-gpu-hang`, run
  before the hang was noticed). Proposed as its own fix.

- **Debian 13 installed-DEB recording flow: blocked** by a pre-existing
  resident crash. `settings.save`, or a pre-written configuration loaded at
  start, crashes the Debian-built resident in `Settings::validate` (a
  `QJsonValueRef` kept to a temporary, `src/core/Settings.cpp`); gdb shows
  `QJsonValueConstRef::concreteType` on the settings worker. Without a
  destination `recording.prepare` answers `output_required`, so only the
  capabilities and KWin authorization above ran from the Debian DEB. The fix is
  a separate session (`claude/settings-validate-uaf`).
- The AppImage has no Qt `xdg-shell` integration plugin (a pre-existing image
  limit), so its windows use XWayland on Wayland. A run with the lab profile's
  `QT_QPA_PLATFORM=wayland` aborted the helper's window
  (`d69/appimage-ci-qpa-wayland-forced`); real launches leave it unset and passed.
- An early local AppImage run on Kubuntu overlapped another session's
  recording test on the same desktop (`k72/appimage-local`); it was repeated
  with the CI AppImage.
- Not run: the Ubuntu 24.04 DEB on Mint 22.3 Cinnamon (no Cinnamon VM was
  launched in this campaign, and the workstation's own Mint desktop has no
  authorized system-wide install); the Ubuntu 26.04 GNOME/KDE Wayland VMs; GPU-encoded MP4 on AMD or NVIDIA from the AppImage
  (only Intel iHD was available; NVENC was seen on the workstation's probe);
  speaker/headphone A/V sync; MCP end to end per desktop; browser playback of
  share URLs.
- AppImage encoder policy: the image bundles no software H.264 or H.265
  encoder (x264/x265 are GPL; OpenH264 binaries are only royalty-free when
  fetched from Cisco), so it records where the host GPU driver encodes H.264.
  This choice was put to Patrick with the alternatives (bundled x264 under GPL
  obligations, a consented Cisco OpenH264 download, or no AppImage recording)
  and is pending his decision.

## Private evidence

`work/verification/2026-10-18-recording-packaging/` in the branch worktree:
host configurations, the campaign drivers (`rp_record.py`, `rp_stub.py`,
`mem_family.py`, `webp_motion.py`), per-run `results.json`, `run.log`,
capabilities, screenshots, previews, clips and motion scores, the CI and
Debian package receipts and checksums, the stock-Fedora and Debian
installability outputs, and the AppImage memory samples.
