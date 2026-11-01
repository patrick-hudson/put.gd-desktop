# Window outline removal (#1135, 2026-11-01)

| File | Build | What it shows |
|---|---|---|
| `corner-before-after.png` | Nightly `6f74e2885` and `d15516b34`, `putgd-ui-catalog`, offscreen on f44 | The main window's top-left corner at 8x: the 1 px `#737373` outline before (red arrows), none after |
| `gnome-wayland-dark-after.png` | `d15516b34`, f44, Fedora 44 GNOME Wayland, dark theme | The real window on the desktop after its right edge was dragged 120 px left: no outline, and the resize cursor at the new edge. The arrows and label are added to the frame |

`d15516b34` is the only commit in #1135 that changes anything on screen; its later commits change tests and catalog references. The results are in [#1135](https://github.com/patrick-hudson/put.gd-desktop/pull/1135).
