# Appearance and themes

put.gd's windows (dashboard, settings, history, editor, about) come in a dark
and a light scheme (#60 phase 1). `preferences.appearance.theme` picks one:

| Value | Effect |
| --- | --- |
| `system` (default, also when `appearance` is absent) | Follows the desktop's light or dark preference, live |
| `dark` | Always the dark scheme |
| `light` | Always the light scheme |

Change it in Settings → General → Appearance, or with `putgd-cli` /
MCP `patch_configuration`:

```json
{"preferences": {"appearance": {"theme": "light"}}}
```

A change applies at once to every open window, without a restart: the
interface process follows each accepted settings revision.

## Following the desktop

For `system`, `putgd-ui` (`src/ui/Appearance.*`) reads the desktop's
preference from, in order:

1. Qt's style hints (`QStyleHints::colorScheme`, Qt 6.5 and later), when they
   know the scheme.
2. The XDG desktop portal's `org.freedesktop.appearance` `color-scheme`
   (1 dark, 2 light). This is the path on Qt 6.4.2 (Mint 22.3), where Qt has no
   style hint. Plasma's portal reports it.
3. The portal's `org.gnome.desktop.interface` settings, which GNOME and
   Cinnamon's XApp portal both serve: `color-scheme` (`prefer-dark`,
   `prefer-light`), then the GTK theme's name (a name containing "dark" is
   dark). GNOME's "default" and Cinnamon's portal (which reports no
   preference) are decided here: on f44 `Adwaita` is light, on m74
   `Mint-Y-Aqua` is light and `Mint-Y-Dark-Aqua` dark.
4. Plasma's `kdeglobals` window colour (first in KDE's configuration cascade),
   when the portal is missing.

When nothing says, put.gd stays dark. The portal is read once at start (at
most 300 ms, so the first window has the right colours) and then through its
`SettingChanged` signal; Qt's `colorSchemeChanged` and changes to
`kdeglobals` are followed too.

## Tokens

One set of named colours (`src/ui/Theme.*`) has a dark and a light value:
`bg`, `surface`, `surfaceRaised`, `selected`, `border`, `borderStrong`,
`text`, `textSecondary`, `textMuted`, `textDisabled`, `accent`,
`accentHover`, `onAccent`, `link`, `success`, `warning`, `danger`,
`dangerHover`, `successSurface`, `warningSurface`, `dangerSurface`, `icon`
and `iconSecondary`. The redesign stylesheet, the application stylesheet and
the editor's additions are templates with `%token%` placeholders; custom
painting (History rows, the editor canvas, drag images) reads the tokens when
it paints; IconArt glyphs set through `VisualStyle::icon(name)` redraw in the
new scheme; the check, radio and chevron indicator images are painted from the
tokens into a private temporary folder. The two former dark sets (the
redesign's and the settings stylesheet's near-copies such as `#111318` for
`#111317`) are now one, the redesign's.

Every text token meets WCAG AA (4.5:1) on `bg`, `surface`, `surfaceRaised`
and `selected` in both schemes, the accent (focus rings) and icon colour meet
3:1 on them, and the status tones meet 4.5:1 on their tinted surfaces;
`gui_theme` checks each pair. Disabled text is exempt, as in WCAG.

Colours that are not theme colours stay fixed: the brand mark, user content
(screenshots, annotation colours, redaction black), the custom-colour
rainbow swatch, the on-screen region selection ring and the scrim behind a
video poster's play glyph. The recording overlays and control bar
(`putgd-record`) keep their own dark styling (#198).

## Tray icon

The tray glyph follows the panel, not the app theme, so a light app on a dark
panel (or the reverse) keeps a visible icon. The resident
(`src/ui/PanelTone.*`) decides the panel tone from:

- GNOME (and Ubuntu's GNOME): the top bar is dark in both schemes.
- Plasma: the Plasma style named in `plasmarc`, read through KDE's whole
  configuration cascade (`~/.config`, then `~/.config/kdedefaults`, where a
  global theme writes its choice, then `XDG_CONFIG_DIRS`, where a distribution
  sets its default). A style that ships its own `colors` file
  (`plasma/desktoptheme/NAME/colors`, such as Breeze Dark or Kubuntu's style)
  decides with its window colour; otherwise a name with `dark` or `light`;
  otherwise the colour scheme's header colour in `kdeglobals` (window colour
  without a header group); otherwise Breeze's light panel. Debian 13's Plasma
  (Breeze Dark from the global theme) and Kubuntu 26.04 (the Kubuntu style)
  both draw dark panels while their colour scheme is light.
- Cinnamon: the desktop theme `org.cinnamon.theme name` (a name containing
  "dark" is dark).
- Anything else: dark, the tint put.gd always used.

It re-reads them when `~/.config` changes on Plasma and on the portal's
`SettingChanged`. The resident's own session environment matters: it needs the
session's `XDG_CONFIG_DIRS` and `XDG_DATA_DIRS`. Dark panels get `#9BBCFF` (idle), `#62D69B` (uploading),
`#F3C66F` (encoding) and `#FFB4AB` (error); light panels `#2759B8`,
`#146C42`, `#8A5300` and `#B4312A`, each at least 3:1 on typical panels. The
menu glyphs use the same tint. The resident's full diagnostics (`ui.snapshot`
`diagnostics`, and diagnostics packages) report the choice as
`trayTint {panel, source, color}`.

## View state

The interface reports the appearance in use in its checkpoint, and
`ui.viewState` (MCP `get_view_state`) returns it as `appearance`:

```json
{"theme": "system", "desktop": "light", "desktopSource": "portal-gtk-theme",
 "scheme": "light", "tokens": {"bg": "#f5f6f8", "...": "..."}}
```

`desktop` is null when nothing reported a preference.

## Phase 2

`appearance.accent` (null) and `appearance.overrides` (`{"dark": {},
"light": {}}`) are reserved in the schema for a custom accent and per-token
overrides, and must keep those values until then (#197).

## Evidence

Screenshots from GNOME, Plasma and Cinnamon are in
[evidence/2026-10-20-appearance-60](evidence/2026-10-20-appearance-60/README.md).
