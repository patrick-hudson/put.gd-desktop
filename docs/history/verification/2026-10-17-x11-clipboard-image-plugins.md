# Verification record

> **Historical record.** This is a dated run record, moved unchanged from [implementation evidence](../../testing/verification.md) on 2026-11-05 (#1222). It describes what that run found for the build it names, and its open or pending statements describe that time. "Above" and "below" refer to its neighbours on the old page, in the order of the [history index](../README.md#verification-run-records). For today, see the [current state](../../testing/verification.md#current-state). Do not use it as a work list.

## X11 clipboard image plugins — 2026-10-17

The [X11 clipboard receipt](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-17-x11-clipboard-plugins.md) records
candidate `d11940f` on `feat/edit-and-upload`:

- Dev suite 64/64 on Mint.
- In parallel: Cinnamon VM, physical Kubuntu Plasma/X11, physical Debian
  Plasma/Wayland and physical Fedora GNOME/Wayland, each with native cases, views,
  restart and two 300-second memory windows (25.9–37.6 MiB peak).
- Plasma/X11 resident 10.2 MiB smaller after its first clipboard import, with no
  image plugin mapped. The one Debian window failure was a lab expectation tied
  to the physical profile name, fixed in the
  [window frame receipt](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-17-kde-window-frame.md).
