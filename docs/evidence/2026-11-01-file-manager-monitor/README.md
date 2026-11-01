# AppImage file manager watch from a private runtime folder (#1112, 2026-11-01)

Both frames come from the Tier 1 `copy_image` cell on the Aurora KDE Wayland guest.

| File | Build | What it shows |
|---|---|---|
| `before-banners-over-history.png` | v26.1101.3644-nightly | Stacked "Cannot configure the file manager integration monitor" banners, 8–9 minutes old, over History (red box) |
| `after-no-banners.png` | CI build of `0c4d75a75` | The same area, clear of banners (green box), after the capture is published |

The frames are at different points in the cell, because the first build failed before reaching the later step. The results for every cell are in the pull request that made the fix.
