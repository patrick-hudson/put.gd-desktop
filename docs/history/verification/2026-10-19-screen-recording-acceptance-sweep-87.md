# Verification record

> **Historical record.** This is a dated run record, moved unchanged from [implementation evidence](../../testing/verification.md) on 2026-11-05 (#1222). It describes what that run found for the build it names, and its open or pending statements describe that time. "Above" and "below" refer to its neighbours on the old page, in the order of the [history index](../README.md#verification-run-records). For today, see the [current state](../../testing/verification.md#current-state). Do not use it as a work list.

## Screen recording acceptance sweep (#87) — 2026-10-19

The [#87 acceptance receipt](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-19-recording-acceptance-87.md)
records the lab-reachable checks from issue #87, on nightly `5ac6bbc` (AppImage
and Ubuntu 26.04 DEB, CI run 39325350860) and `8e568fd` (Fedora 44 RPM, Ubuntu
26.04 DEB, CI run 39336656763). Every run used a moving pattern with a scored
static control, saved frames and recorded identities.

- **AppImage A/V** (`--av-marker`/`--av-sync`, built-in speakers): exported
  MP4/WebM within −24 to +27 ms on k72 X11, d69 `kde-screencast` and portal, and
  f44 Mutter and portal (window); the RPM on f44 read +10.0/−2.5 ms.
- **#81 AppImage on Wayland:** d69 Plasma 6.3 and f44 GNOME 50 recorded regions
  and monitors in all four formats as native Wayland clients (no X11 socket),
  motion 14–37 against controls of 0.02–0.06.
- **Packaged GNOME extension** (f44, RPM `8e568fd`, re-login): version 2
  `ACTIVE`; dragged and selected regions plus monitor, four formats, motion
  22–38. RPM removed and the session restored afterwards.
- **MCP end to end:** 20/20 checks on k72, d69 and f44.
- **Ubuntu 26.04 GNOME 50.1 and Kubuntu 26.04 Plasma 6.6.6 guests:** Mutter,
  KWin and the GNOME portal; all four formats recorded, saved and uploaded.
- **Multi-monitor Wayland** (`lab.py --heads 2`, both guests): each monitor's
  canvas matched its output, regions on either head and across both had the
  right size and content, and the ring and bar stayed outside on the right head.
- **Share URLs** (bucket `put.gd`, prefix `acceptance/87-20261019/`, a scoped
  key Patrick entered in Settings): MP4, WebM, GIF and WebP played in Firefox and
  Chromium on f44, then were deleted through put.gd's history (S3 origin 403;
  CloudFront kept cached copies). #121 had landed, so WebM ran with its fix.
- **Pause/resume A/V:** sync holds within each segment, but a resume steps it by
  −27 to +30 ms, and the export moves the post-resume part ~10 ms earlier.
- Follow-ups: #129 (Stop hangs with a null audio sink, seen in VM guests), #130
  (pause/resume A/V step), #131 (stale-extension message after an upgrade), #137
  (AppImage can't record on Plasma 6.6 Wayland).
- Blocked: AMD/NVIDIA encoding, physical Mint Cinnamon, a headphone listening
  test and Bluetooth/other sinks.
- **Physical Plasma 6.6.6 Wayland (k72):** the Ubuntu 26.04 DEB recorded regions
  and monitors in all four formats (motion 25–57), passed MCP 20/20 and read
  −15 to +10 ms A/V; the AppImage can't start a recording there (#137). The k72
  wall tile now has a Wayland VNC server too.
