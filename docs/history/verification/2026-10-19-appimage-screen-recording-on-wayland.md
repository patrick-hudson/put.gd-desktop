# Verification record

> **Historical record.** This is a dated run record, moved unchanged from [implementation evidence](../../testing/verification.md) on 2026-11-05 (#1222). It describes what that run found for the build it names, and its open or pending statements describe that time. "Above" and "below" refer to its neighbours on the old page, in the order of the [history index](../README.md#verification-run-records). For today, see the [current state](../../testing/verification.md#current-state). Do not use it as a work list.

## AppImage screen recording on Wayland — 2026-10-19

The [Wayland AppImage recording receipt](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-19-appimage-recording-wayland.md)
records the nightly `9042776` CI AppImage (#75, #77 and #81), sha256
`1be3fbdc…`. It ran from the image with private XDG directories and nothing
installed.

- Desktops: the physical Debian 13 Plasma 6.3.6 Wayland (`kde-screencast`, host
  VA-API) and Fedora 44 GNOME 50 Wayland (`mutter-screencast`, the bundled `x264enc`)
  desktops, with Kubuntu 26.04 Plasma X11 as the control.
- Flow: each desktop recorded a dragged region, a region completed through
  `recording.select`, and the whole monitor, in MP4, WebM, GIF and WebP. Every clip
  was reviewed, saved and uploaded to a local destination: 89 clips in total.
- `overlayPlacement` matched [recording](../../design/recording.md): region `outside` everywhere,
  monitor `inside` on KWin 6.3, Mutter and X11.
- The recorder's bar and review window were native Wayland clients, using
  `xdg-shell` and LayerShellQt, with no X11 connection.
- Against the same test pattern standing still, every clip's motion score was
  hundreds of times higher.
- On GNOME, a rectless region needed the put.gd Shell extension, run as a temporary
  user copy. Without it the region ended `region_unavailable`.

Not run:
- A/V sync;
- the portal path;
- `window` and `desktop` targets;
- KWin 6.6 Wayland on hardware;
- multi-monitor;
- AMD/NVIDIA encoding;
- the window icons and own-window refusal of #88.

The image's own `putgd-record` diagnostics can't read its exports (#92).
