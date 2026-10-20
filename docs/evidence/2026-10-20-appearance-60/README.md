# System, Dark and Light appearance (#60): lab evidence

Taken 2026-10-20 06:40–07:15Z in the physical desktop sessions, from scratch lab builds of
`feat/appearance-theme-60` (with #112 merged in through nightly), run with private config, data,
state and runtime folders and driven by `putgd-cli` (`settings.patch` for
`preferences.appearance.theme`, `ui.show`). The History row is a real recording (see the #112
evidence).

| Host | Desktop | Qt | Desktop preference (`ui.viewState` `appearance.desktopSource`) | Tray (`trayTint`) |
| --- | --- | --- | --- | --- |
| f44 | GNOME 50 Wayland | 6.11.2 | light, `qt` | dark panel, `gnome-shell`, `#9BBCFF` |
| k72 | Plasma 6.6 Wayland, Kubuntu style | 6.10.2 | light, `qt` | dark panel, `plasma-style`, `#9BBCFF` |
| m74 | Cinnamon 6.6 X11, Mint-Y-Dark-Aqua | 6.4.2 | light, `portal-gtk-theme` | dark panel, `cinnamon-theme`, `#9BBCFF` |

m74 is the Qt 6.4.2 case: Qt has no colour-scheme hint there, and the preference came from the
portal's GNOME interface settings (`Mint-Y-Aqua`, light), as designed. All three lab panels are dark
(d69's too: its global theme sets Breeze Dark in `~/.config/kdedefaults/plasmarc`); the light-panel
tints are covered by `gui_theme`.

**Capture**: frames from each host's loopback VNC tile, read and cropped on the host. A before/after
diff isolates put.gd's window; the crop is its client area, kept only when ≥85% of its area (or ≥90%
of its border and ≥55% of its area) is the theme's token colours (kept crops: 90–98%). The tray crop is
the new glyph's tint pixels in the panel strip. Full frames were deleted on the host unopened.
Annotations were added afterwards with Pillow.

On f44 and k72 (Wayland) the Settings view didn't show in the frames when opened, or switched to, in
Dark; the window stayed up and reported the scheme. That is #214. m74 (X11) captured it natively,
including a live switch with the window left open. #214 turned out to be the method, not put.gd: the
private config had no settings revision, so the resident opened Settings at start and the baseline
already held it ([evidence](../2026-10-20-appearance-overlays-198-214/README.md)).

| File | Shows |
| --- | --- |
| `settings-appearance-dark-vs-light.png` | Settings → General → Appearance, Light and Dark (m74) |
| `history-dark-vs-light-f44.png`, `-k72.png`, `-m74.png` | History with the recording's poster, Dark and Light, on GNOME, Plasma and Cinnamon |
| `tray-follows-panel.png` | The tray glyph on each desktop's panel and the rule that chose its tint |
