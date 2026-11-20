# Published tray glyph frames

Test input for `tests/desktop/test_surface_tray_pixels.py` only. The lab's matcher never reads
these: in a run it takes its template from the frames the running resident publishes.

- `*-dark-*.png`: the StatusNotifierItem `IconPixmap` frames a resident of build `c5babe1ad`
  published in an Omarchy guest (dark panel tint `#d6d6d2`), with `PUTGD_TRAY_CALENDAR_NOW` set to
  2026-11-20 (turkey), 2026-12-15 (tree) and 2026-06-15 (mark).
- `*-light-*.png`: the same frames for a light panel. `IconArt::liftTrayAtSize` changes only the
  fill colour with the panel (outline `#182126` either way), so the fill was mixed again with the
  light idle tint `#383834`, as `settings-tray-seasons.png` in that build's UI catalog draws it.
