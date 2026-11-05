# Screen recording on Linux (issue #50) — 2026-10-18

This record certifies the candidates on `claude/putgd-desktop-issue-50-29bb77`
(pull request #68 into `nightly`): `8252f034d319` for the X11 capture, export,
review and tray work, and `625b7ff38bb0` (the branch head) for the Wayland
acquisition paths. The feature is described in
[recording](../recording.md); the plan is
[2026-10-17-2230-feat-screen-recording-plan](../history/plans/2026-10-17-2230-feat-screen-recording-plan.md).

## Suites

| Candidate | Where | Result |
| --- | --- | --- |
| `8252f034d319` | Kubuntu 26.04 builder `.72` (GCC 15, Qt 6.10) | CTest 70/70; `test_mcp.py` 3 OK and `test_config_schema.py` 5 OK |
| `8252f034d319` | Fedora 44 builder `.70` (GCC 16, Qt 6.11) | CTest 71/71 |
| `8252f034d319` | Debian 13 builder `.69` | CTest 71/71 |
| `52b1969` (portal slice) | Mint 22.3 workstation, dev preset | CTest 71/71 |
| `625b7ff38bb0` | Mint 22.3 workstation, dev preset | CTest 72/72 |
| `625b7ff38bb0` | Fedora 44 builder `.70` | CTest 73/73 |
| `625b7ff38bb0` | Debian 13 builder `.69` | CTest 73/73 |

The builders ran rsynced scratch trees; the Kubuntu builder was not rerun on
the final commit (the pull request's Ubuntu 26.04 CI job covers that
toolchain). Pull request CI passed on the head: contracts, Linux 24.04 and
26.04, the Windows portable build, native packages for Ubuntu 24.04, Ubuntu
26.04 and Fedora 44, and the AppImage.

## X11 capture, review and export (`8252f034d319`)

Lab campaign with the fixture package on each row, capture scenarios first,
then the recording flow (prepare, start, pause, resume, stop, edit, preview,
save copy, upload) through `putgd-cli`, with a `videotestsrc pattern=ball`
window in the captured area and the exported clip scored by
`putgd-record --motion` (static control 0.012):

| Row | Placement | Capture scenarios | Recording |
| --- | --- | --- | --- |
| Mint 22.3 Cinnamon/X11 | KVM guest on `.70` | 7/7 | full flow, motion 0.613, upload completed |
| Mint 22.3 Cinnamon/X11 | the workstation desktop | hands-on | scripted flows for region, cross-monitor GIF, monitor WebP and window WebM; motion 0.631 |
| Kubuntu 26.04 Plasma 6.6/X11 | physical `.72` | 5/5 | full flow and the shortcut toggle, motion 0.476 |
| Ubuntu 26.04 GNOME/Wayland | KVM guest on `.69` | 7/7 | declined (`wayland_not_implemented` at this candidate) |
| Kubuntu 26.04 Plasma/Wayland | KVM guest on `.69` | 7/7 | declined |
| Fedora 44 GNOME 50/Wayland | physical `.70` | 5/5 | declined |
| Debian 13 Plasma 6.3/Wayland | physical `.69` | 5/5 | declined |
| Fedora 44 GNOME/Wayland | KVM guest on `.70` | blocked: the golden has no put.gd shell extension | — |

Resident RSS after the runs: 31–39 MiB on the guests and the Wayland
desktops, 49.6 MiB on the physical Plasma/X11 desktop, which also owns the
X11 capture adapter.

Workstation measurements (i9-9880H, UHD 630, Quadro T2000; a 60 s 2238×1318
clip with AAC audio exported with `putgd-record --export`): `h264_nvenc`
15.6 s (3.86×), `hevc_nvenc` 15.7 s, `libx264` veryfast 28.2 s, VP9 realtime
34.7 s, `h264_vaapi` on the iGPU 23.5 s (2.56×); `libx265` stays CPU bound
(88.7 s, 0.68×), which is why it sits last. On the Kubuntu builder (HD 530,
iHD) `h264_vaapi` exported a 20 s 1080p clip in 6.6 s (3.0×); `hevc_vaapi` is
"Function not implemented" on Skylake and falls through to `libx265`. Live
capture with `vaapih264enc` needs NV12 input and constant QP on the iHD driver
(CBR and VBR refused); the fixed element captured 10 s of 1280×720 for 2.1 s
of CPU against 4.3 s for `x264enc`. A user-found GIF review that stayed in
`reviewing` (animation edge limit refused the export after Upload closed the
window) and Activity Cancel not ending a recording were fixed and re-run
before the campaign (`e1d012b`, `8252f03`).

## Wayland acquisition (`52b1969`, `625b7ff38bb0`)

Driven on the physical Fedora 44 GNOME 50 and Debian 13 Plasma 6.3 desktops
with a scratch-build resident in the session, the lab input agent for the
one picker click, and clips scored on the workstation.

- Portal (`52b1969`): both desktops probe `acquisition: portal`, portal v5.
  One click on GNOME's Share Screen dialog and on KDE's Screen Sharing tile
  made the session `ready`; the full flow then recorded (GNOME `openh264enc`
  7.5 s, KDE `vah264lpenc` 7.9 s), edited, previewed, saved and uploaded.
  Two Fedora findings on the way: `gstreamer1-plugin-openh264` links the
  `noopenh264` stub until `fedora-cisco-openh264` is enabled, and Fedora's
  FFmpeg decodes H.264 only through `libopenh264`, which refuses the P-frames
  a pause left without references; the pause probes moved to the encoders'
  sink pads, verified on both desktops and on the X11 pause flow.
- Restore tokens (`625b7ff38bb0`): with `persist_mode 2` the token is stored
  per desktop and target kind; after one picker click and a cancel, the next
  `monitor` prepare reached `ready` with no input on both desktops.
- Compositor-native streams (`625b7ff38bb0`): fresh data directories, no
  tokens, a fullscreen Wayland pattern window, then a `monitor` and a centred
  600×400 `region` recording, each expected to reach `ready` without input:

| Desktop | `acquisition` | Monitor clip | Region clip |
| --- | --- | --- | --- |
| Debian 13 Plasma 6.3.6 | `kde-screencast` (protocol v5) | 4.93 s, motion 0.319 | 6.93 s, motion 0.056, ball visible in the frames |
| Fedora 44 GNOME 50 | `mutter-screencast` (`RecordMonitor`, `RecordArea`) | 4.87 s, motion 0.320 | 6.80 s, motion 0.043, ball visible in the frames |

KWin advertises its screencast protocol only to an executable whose desktop
entry lists it in `X-KDE-Wayland-Interfaces`; the run used a lab entry naming
the scratch helper, and packaging ships `putgd-record.desktop` for installs
(not yet exercised from an installed package). The native paths need the
session PipeWire socket, which the lab resident's isolated runtime directory
hid at first; the probe now requires it before preferring a native path. The
region clips score lower than the monitor clips because the ball crosses the
centred region only part of the time.

## Not covered

Windows (design only), microphone capture (reserved and refused), a Wayland
drag overlay (regions on Wayland come from a rectangle; the floating bar is
part of the compositor stream), the Wayland KVM guests on the final commit,
the Fedora GNOME guest (no shell extension in the golden), and GPU export on
AMD hardware (the libva path is shared with Intel and was only run there).

## Retirement and private evidence

Lab residents, pattern windows, input agents and tunnels were stopped on the
three builder desktops; the campaign package was removed where it had been
installed; passed guest overlays were retired through the guarded cleanup and
the failed or blocked launches kept for review on the hosts. The workstation
runs the branch head as a per-user install on its existing configuration.

Private receipts (logs, clips, screenshots, drivers, identities):
`work/verification/2026-10-18-recording-full-suite/` (786 files,
`MANIFEST.sha256` `cb1bf7ff786c276424b6b0e6d06488df3af502b0ddd599fcb48b91c00813d763`),
`work/verification/2026-10-18-recording-cinnamon/` (66 files,
`06146b36ed2d8de4b5c3c2897b6094106ca114f948552b45061daf28899d11ad`) and
`work/verification/2026-10-18-recording-local-install/` (46 files,
`396908c0ca533cb79093d7bd3e7f06183affaec8a7b23bf269f0cfc1d694de30`).
