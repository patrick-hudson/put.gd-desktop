# Hotkey to crosshair: frames for the selection stack

Frames are full-desktop VNC captures taken at fixed phases of one real drag:
`overlay` about 1 s after the hotkey, `drag` mid-drag with the button held. Red
boxes and labels were added afterwards. The panel clock differs between any two
frames.

| Pictures | Build |
| --- | --- |
| `mint-appimage-nightly-*`, `fedora-rpm-nightly-*`, `debian-deb-nightly-*`, `kubuntu-deb-nightly-*` | Nightly `61c556dbcb4b` |
| `mint-appimage-head-*`, `fedora-rpm-head-*` | Final head `6d67545ca87c` on memory base `6a93abaf888d` |
| `aurora-appimage-*` | Nightly `61c556dbcb4b` and final head `6d67545ca87c` |
| `debian-deb-head-*`, `kubuntu-deb-head-*` | Earlier head `0f90e4f743c4` on the old base `6fe5c21e1439`. Its KDE code is the same as the final head's. |

## What differs

Compared per pixel at each phase, on the same desktop. The pictures here are a
selection; the full frame sets were kept on the workstation.

A **capture hash** is the SHA-256 of the PNG file put.gd saved from the dragged
region, not of the frame, so the panel clock isn't in it.

- **Mint**, builds nightly, `6a93abaf888d` and the final head, AppImage and DEB:
  the panel clock only. Same capture hash.
- **Debian**, nightly through the old-base head, DEB: the panel clock only. Same
  capture hash.
- **Kubuntu**, nightly through the old-base head, DEB and AppImage: the panel
  clock, plus on the DEB overlay frames from the font-worker layer on, the pill's
  pixels differ by up to 14 of 255 per channel. Text and shape match. Most likely
  the pill's fade at a slightly different moment; not proven.
- **Fedora**: not identical. Besides the clock, some frames show GNOME's busy
  cursor instead of the crosshair, or the first-run "right-click menu" banner,
  both following no build. The cause isn't proven; the lab tool starts a fresh
  resident and profile seconds before each drag. Within the dragged region the
  decoded pixels are identical: PNG file hashes differ between runs here, so
  Fedora hashes the decoded RGB of the saved file.
- **Aurora**, nightly and the earlier final head `6d67545ca87c`, AppImage: the panel
  clock on the overlay frame, and nothing at all on the mid-drag frame. Same
  capture hash (`8aefb4d116df`).
