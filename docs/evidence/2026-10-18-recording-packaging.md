# Screen recording in every package (issue #50 packaging) — 2026-10-18

This record certifies what pull request #75 (`claude/recording-packaging` into
`nightly`) built and ran. The pull request:

- adds the AppImage recording runtime;
- applies Patrick's codec decision: x264 and x265 are bundled behind the host's GPU encoders, and their corresponding source is published;
- makes the probe check that H.264 actually works, with an actionable message when it does not;
- makes the RPM recommend Cisco's OpenH264;
- checks recording from installed packages and from the AppImage.

The feature is described in [recording](../recording.md) ("Packages and codecs",
"AppImage runtime"); the release obligations are in [releases](../release.md).

## Candidates

| Commit | Built by | Artifacts (SHA-256) |
| --- | --- | --- |
| `7aa1aff` (CI merge `3da3e91`, `0.1.1-dev.378+g3da3e91610d8`), **the final code** | pull request CI | AppImage `1884456b…` (112,736,760 bytes), its media source `f8e5a4e5…` (18,418,513 bytes), Ubuntu 26.04 DEB `5ed04250…`, Fedora 44 RPM `0a89cb8a…` |
| `8385daf` (CI merge `ba7153d`) | pull request CI | AppImage `b538b095…` (109,390,328 bytes, no bundled x264/x265), Fedora 44 RPM `ce71d1a4…` |
| `8385daf` (`0.1.1~dev.371+g8385dafa0f7f`) | Debian 13 builder `.69`, `tools/releases/native.py` (release preset, no lab fixture) | Debian 13 DEB `a862cf43…` |
| `9462fe7` (CI merge `7237d0e`) | pull request CI | Ubuntu 26.04 DEB `a08a93b3…`, Fedora 44 RPM `f4a24f62…` |

The DEB and RPM recording code is the same in all three commits. Only these changed:
- the AppImage assembly and its codec set;
- the probe's export list order;
- two user-facing sentences.

Rows below name the commit they ran.

## Suites

- Pull request CI on `8385daf` and `7aa1aff`: contracts, Linux 24.04 and
  26.04, the Windows portable build, the three native packages and the AppImage
  all passed. On `7aa1aff` the AppImage assembly audit ran the relocated
  helper's probe on the GPU-less builder and required the bundled
  `x264enc`, `libx264` and `libx265` to work, which they did.
- Workstation (Mint 22.3), `ctest --preset dev` on `399d491`: 74/74 in 719 s.
  That run came before the lab's rule against full suites on the workstation;
  later code commits were checked by CI.
- The release contract group (`tools/ci/contracts.py release`) passes,
  including the new `test_appimage_media_sources.py` and the AppRun and audit
  cases in `test_portable_appimage.py`.

## Probe on real hosts

`putgd-record --capabilities` from scratch builds of this branch:

| Host | Result |
| --- | --- |
| Fedora 44 `.70`, Cisco `openh264` installed | `available`, `mutter-screencast`; live `openh264enc`; export `libopenh264` and `libvpx-vp9`. `h264_vaapi`/`hevc_vaapi` fail with "Function not implemented" (stock iHD 25.4.6); NVENC absent |
| Same host, `noopenh264` 2.6.0 placeholder first on the library path | `h264_encoder_unusable`: `openh264enc` fails with "Could not initialize supporting library", FFmpeg's `libopenh264` decoder and encoder fail to open, and MP4 is dropped from `formats`. The message and the `dnf install openh264 gstreamer1-plugin-openh264` hint are shown |
| Mint 22.3 workstation | `available` (`vaapih264enc`, `x264enc`, `openh264enc`; export `h264_nvenc`, `libx264`, `h264_vaapi`, `hevc_nvenc`, `libx265`). A warm probe takes 0.24-0.27 s before and after the change |

## Installed packages and the AppImage on the physical desktops

The runs are driven through `tools/lab/physical.py` with:
- the production CLI;
- private XDG directories under `~/putgd-lab/rp`, with a local destination and archive there;
- a fullscreen `videotestsrc pattern=ball` window;
- saved screenshots (ready, recording, review, edited review, after upload).

The resident is launched the way a desktop launch would be: no
`QT_QPA_PLATFORM`, and on GNOME the session manager's `DISPLAY`/`XAUTHORITY`.

The first format runs the full flow: prepare, start, pause, resume, stop, trim 0.5-4.5 s plus a timed blur, preview, save copy, upload. The other formats record, save a copy and upload. Every listed upload completed and the file appeared in the archive.

Motion scores come from `putgd-record --motion` (Pillow for WebP). The static controls from the Kubuntu desktop score 0.054 (WebM) and 0.106 (MP4 with the blur switching off mid-clip).

| Desktop | Package | Acquisition and live encoder (export H.264 / H.265) | MP4 | WebM | GIF | WebP | Helper peak |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Kubuntu 26.04.1 Plasma 6.6.6 / X11, `.72` | Ubuntu 26.04 DEB `7aa1aff` | `x11`, `vah264lpenc` + `avenc_aac` | 0.251 | 0.293 | 0.224 | 0.301 | 141-146 MiB |
| Kubuntu 26.04.1 Plasma 6.6.6 / X11, `.72` | AppImage `7aa1aff` | `x11`, host `vah264lpenc` first; bundled `x264enc` available (export `h264_vaapi`, then `libx264`) | 0.252 | 0.247 | 0.213 | 0.502 | 111-118 MiB |
| Debian 13 Plasma 6.3.6 / Wayland, `.69` | AppImage `7aa1aff` | `kde-screencast` v5 through AppRun's entry, `vah264lpenc` | 0.280 | 0.279 | 0.278 | 0.274 | 110-114 MiB |
| Fedora 44 GNOME 50 / Wayland, `.70` | Fedora 44 RPM `7aa1aff` | `mutter-screencast`, `openh264enc` + `avenc_aac` | 0.281 | 0.281 | 0.279 | 0.284 | 148-152 MiB |
| Fedora 44 GNOME 50 / Wayland, `.70` | AppImage `7aa1aff` | `mutter-screencast`, **bundled `x264enc`** (stock driver has no H.264 encode; export `libx264` / `libx265`) | 0.282 | 0.244 | 0.253 | 0.286 | 129-131 MiB |
| Fedora 44 GNOME 50 / Wayland, `.70` | Fedora 44 RPM `9462fe7` | `portal` (one Share click in GNOME's picker) | 0.281 | | | | 152 MiB |
| Kubuntu, Debian, Fedora | earlier runs: DEB/RPM `9462fe7`, AppImage `8385daf` | as above; the `8385daf` AppImage refused on Fedora (`live_encoder_missing`) before x264 was bundled | 0.20-0.43 | | | | |

Other checks on the same desktops:

- **KWin authorizes the installed helper.** With the Debian DEB installed and no user entries,
  `/usr/share/applications/putgd-record.desktop` gave `kde-screencast`
  (protocol v5, `HDMI-A-1`).
- **AppRun's KWin entry.** On Plasma, AppRun wrote `putgd-record-appimage-XXXXXXXX.desktop`
  (`Exec="/tmp/.mount_putgd-…/usr/bin/putgd-record"`,
  `X-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1`) next to the
  ScreenShot2 entry. Both were gone after the resident quit.
- **Fedora placeholder in the real UI.** The installed RPM's resident was started with
  `noopenh264` first on its library path. It reported `h264_encoder_unusable`, and
  `recording.prepare` returned `recording_unavailable` with the message and
  hint. **Record screen** on the dashboard showed both in a warning dialog
  (screenshot `f44/rpm-stub/02-after-record-click.png`).
- **Stock Fedora 44** (`registry.fedoraproject.org/fedora:44` containers on `.70`,
  RPM `8385daf`). A default `dnf install` pulled `gstreamer1-plugin-openh264`
  and Cisco's `openh264` through the RPM's Recommends, and the probe found
  `openh264enc` and `libopenh264` usable. With `install_weak_deps=False` the
  probe reported `live_encoder_missing` with the Fedora hint.
- **Debian 13 cannot install either Ubuntu DEB.** `apt-get install --simulate` shows
  `libavcodec60` / `libavcodec62` unsatisfiable. The Debian rows use the
  Debian-built DEB.
- **GPL corresponding source for the `7aa1aff` AppImage.** The media source contains:
  - FFmpeg 7.1.5 at `3a0867c2…` (`git archive`);
  - the Ubuntu source packages `x264_0.164.3108+git31e19f9-1`, `x265_3.5-2build1` and `gst-plugins-ugly1.0_1.24.1-1build1` (`.dsc`, orig and debian tarballs);
  - `SOURCES.json` and `README.txt`.

  The image carries `usr/share/doc/putgd/APPIMAGE-GPL-NOTICE.txt` and the distro copyright files for `libx264-164`, `libx265-199` and `gstreamer1.0-plugins-ugly`.

## AppImage size and memory

| | nightly 317 | without x264/x265 (`8385daf`) | this change (`7aa1aff`) |
| --- | --- | --- | --- |
| AppImage bytes | 98,929,144 | 109,390,328 | 112,736,760 (+13,807,616 over nightly 317) |
| Idle family on Debian Plasma/Wayland, RSS / PSS (median of 3) | 111.0-111.7 / 75.4-76.1 MiB | 111.5 / 75.9 MiB | 112.3 / 76.7 MiB |
| Resident RSS | 29.0 MiB | 29.3 MiB | 29.3 MiB |

The family is the runtime daemon, AppRun, the resident and `putgd-ui`, 30 s
after `--background`. The 111.0 and 111.7 MiB figures for nightly 317 come from two runs of the same image.
While recording, the AppImage helper peaks at 110-131 MiB, against 141-153 MiB for the distro packages' helpers.
The corresponding-source asset adds 18,418,513 bytes to each release.

## Findings and gates not run

- **Kubuntu GPU hang, resolved.** At about 02:32Z the i915 driver reset Xorg's context ("GPU HANG ... in Xorg").
  - It happened during a recording with `vah264lpenc` (Intel's low-power VDENC path) while the host ran at load 9 from other sessions' builds.
  - The same host had logged the same hang on Oct 17, before this campaign.
  - `kwin_x11 --replace` restored the picture, but `ximagesrc` kept reading stale root-window frames.
  - With Patrick's approval in this session, `systemctl restart sddm` at 03:12Z brought back a healthy Plasma X11 session (autologin).
  - The `7aa1aff` Kubuntu rows ran after that. The void runs from the frozen display are kept apart as `k72/*-invalid-*`.
  - No reordering of encoders was made on this one event.
- **Debian 13 installed-DEB recording flow: blocked here.** A pre-existing resident crash in `Settings::validate` (a dangling `QJsonValueRef`) hit every settings save on Debian. So only the capabilities and the KWin authorization ran from the Debian DEB. The fix is pull request #76 (confirmed on the same desktop by its session).
- **Qt platform plugins missing from the AppImage.** The image lacks two of them. Both are pre-existing; #80 and #81 deployed them after these runs, and their own receipts cover them:
  - Qt's `offscreen` platform: `putgd-ui --image-job`, which previews non-PNG images such as the recorded GIF/WebP, aborts. The uploads still complete.
  - Qt's `xdg-shell` integration: its windows, including the recorder's control bar and review, use XWayland on Wayland sessions.

  Consequences seen in this campaign:
  - With the lab profile's `QT_QPA_PLATFORM=wayland` forced, the recorder's window aborted (`d69/appimage-ci-qpa-wayland-forced`).
  - On GNOME without `DISPLAY` in the environment it aborted (`f44/appimage-7aa1aff-no-display-env`).
  - A desktop launch has neither problem and passed.

  The recordings were repeated with nightly `9042776` (#75, #77 and #81), with native
  recorder windows on Plasma and GNOME Wayland:
  [2026-10-19 Wayland recording](2026-10-19-appimage-recording-wayland.md).
- **CLI timeout on long exports.** `putgd-cli` gave up after 15 s while `saveCopy` was still exporting under load. Fixed separately in pull request #79.
- **Overlap with another session.** An early local AppImage run on Kubuntu overlapped another session's test on the same desktop (`k72/appimage-local`). It was repeated with CI AppImages.
- **Not run:**
  - the Ubuntu 24.04 DEB on Mint 22.3 Cinnamon (no Cinnamon VM was launched);
  - the Ubuntu 26.04 GNOME/KDE Wayland VMs;
  - GPU-encoded MP4 on AMD or NVIDIA from the AppImage (only Intel iHD was available);
  - speaker/headphone A/V sync;
  - MCP end to end per desktop;
  - browser playback of share URLs.

## Private evidence

`work/verification/2026-10-18-recording-packaging/` in the branch worktree:
- host configurations;
- the campaign drivers (`rp_record.py`, `rp_stub.py`, `mem_family.py`, `webp_motion.py`);
- per-run `results.json`, `run.log`, capabilities, screenshots, previews, clips and motion scores;
- the CI and Debian package receipts and checksums, including the media-source archive;
- the stock-Fedora and Debian installability outputs;
- the k72 restart record and the AppImage memory samples.
