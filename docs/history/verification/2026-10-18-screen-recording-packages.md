# Verification record

> **Historical record.** This is a dated run record, moved unchanged from [implementation evidence](../../testing/verification.md) on 2026-11-05 (#1222). It describes what that run found for the build it names, and its open or pending statements describe that time. "Above" and "below" refer to its neighbours on the old page, in the order of the [history index](../README.md#verification-run-records). For today, see the [current state](../../testing/verification.md#current-state). Do not use it as a work list.

## Screen recording packages — 2026-10-18

The [recording packaging receipt](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-18-recording-packaging.md)
records pull request #75, with final code `7aa1aff`:

- Pull request CI on `7aa1aff`: contracts, Linux suites, the then-supported cross-platform build, three
  native packages, and the AppImage. The AppImage's audit runs the relocated
  recorder and requires its bundled x264/x265 to work.
- On the physical Kubuntu Plasma/X11 desktop, the Ubuntu 26.04 DEB recorded,
  reviewed, exported MP4, WebM, GIF and WebP and uploaded to a local
  destination. So did the Fedora 44 RPM on GNOME/Wayland (Mutter, and the
  portal on an earlier commit). Every clip carries the moving pattern.
- The AppImage did the same on Kubuntu, on Debian Plasma/Wayland (KWin's
  screencast protocol through AppRun's entry) and on Fedora. There it used its
  bundled x264, because the stock driver cannot encode H.264.
- The Fedora `noopenh264` placeholder produced the actionable dialog on the
  dashboard. The earlier container result does not prove that Recommends
  replace an installed placeholder: on Fedora Workstation they do not
  (issue #1091). Use `sudo dnf install openh264 gstreamer1-plugin-openh264`
  explicitly; the one-line installer performs this step with the Cisco repo enabled.
- The AppImage is 13.8 MB larger than nightly 317, and its idle family is
  within 0.6 MiB.

Blocked or not run:
- the Debian-built DEB's recording flow (a pre-existing `Settings::validate`
  crash, fixed in #76; only capabilities and KWin authorization ran);
- the Ubuntu 24.04 DEB on Mint;
- the Ubuntu 26.04 Wayland VMs;
- AMD/NVIDIA GPU encoding from the AppImage;
- A/V sync, MCP end to end and browser playback.
