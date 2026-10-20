# Appearance and themes

put.gd's windows (dashboard, settings, history, editor, about) come in a dark
and a light scheme (#60 phase 1). `preferences.appearance.theme` picks one:

| Value | Effect |
| --- | --- |
| `system` (default, also when `appearance` is absent) | Follows the desktop's light or dark preference, live |
| `dark` | Always the dark scheme |
| `light` | Always the light scheme |

Change it in Settings → General → Appearance (it applies as soon as you pick it), or with `putgd-cli` /
MCP `patch_configuration`:

```json
{"preferences": {"appearance": {"theme": "light"}}}
```

A change applies at once to every open window, without a restart. A theme
picked in Settings applies in the same event, before the resident has accepted
it, and saves straight away (no autosave delay); a refused or discarded save
puts the accepted theme back, and the draft keeps the pick for Retry saving.
CLI and MCP patches apply through the accepted revision, as before (#237).

The switch replaces no stylesheet. Qt re-polishes every widget, several times
over, when the application stylesheet changes: 1.3 to 1.5 s with History and
Settings open. So the application and redesign stylesheets are set once and
hold both schemes' rules, each scoped to the `themeScheme` property that
every widget carries (`VisualStyle::schemeScoped`, `VisualStyle::followScheme`).
A switch sets the palettes and re-polishes only the visible widgets; a widget
hidden at the switch catches up just before it is shown. A pick repaints every
open window in 38 ms on d69 Plasma Wayland (Qt 6.8.2), 39 ms on f44 GNOME
Wayland (Qt 6.11.2) and 64 ms on m74 Cinnamon X11 (Qt 6.4.2; #260), measured
with `putgd-bench-theme-switch`; see
[the evidence](evidence/2026-10-20-ui-polish-233-237-238-239/README.md).
Stylesheet `palette(role)` references are not an alternative: Qt resolves them
when it builds a rule, so they keep the old colours after a palette change.

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
new scheme; the check, radio, chevron and spin box step (#233) images are painted from the
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
video poster's play glyph.

## Recording overlays

`putgd-record`'s windows use the same scheme and tokens (#198). The resident
resolves `preferences.appearance.theme` against the desktop with the same
`Appearance` code as `putgd-ui` (it does not restyle itself) and passes the
scheme in the helper's bootstrap frame; a change while a recording is
prepared, running or under review reaches the open helper as an `appearance`
frame and restyles it live (see [recording](recording.md)).

| Element | Colours |
| --- | --- |
| Control bar | `bg` fill, `text`, `surfaceRaised` controls, `accent` check box with a tick painted from `onAccent`; its outline is `textDisabled`, which meets 3:1 against both black and white in either scheme, so the bar's edge shows over any content |
| Record and Stop buttons, state dot | fixed green and red (white text, 4.5:1 or better), the same in both schemes |
| Region selector hint and size label | the scheme's `bg` label with `text`, outlined like the bar, so it reads over any content |
| Review window | the window tokens; Upload is the primary (`accent`) button |
| Selection ring, dimming, review frame surround, mask outlines | fixed: they sit on the desktop or the recording |

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
session's `XDG_CONFIG_DIRS` and `XDG_DATA_DIRS`. Lab tools that start it over SSH
through `tools/lab/lab.py`'s `guest_session` take both from the desktop shell's
environment (#243). Dark panels get `#9BBCFF` (idle), `#62D69B` (uploading),
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
