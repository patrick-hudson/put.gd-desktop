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
Settings open. So the application and window stylesheets are set once and
hold both schemes' rules, each scoped to the `themeScheme` property that
every widget carries (`Ui::schemeScoped`, `Ui::followScheme`).
A switch sets the palettes and re-polishes only the visible widgets; a widget
hidden at the switch catches up just before it is shown. A pick repaints every
open window in 38 ms on d69 Plasma Wayland (Qt 6.8.2), 39 ms on f44 GNOME
Wayland (Qt 6.11.2) and 64 ms on m74 Cinnamon X11 (Qt 6.4.2; #260), measured
with `putgd-bench-theme-switch`; see
[the evidence](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-20-ui-polish-233-237-238-239/README.md).
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
and `iconSecondary`. The window stylesheet, the application stylesheet and
the editor's additions are templates with `%token%` placeholders; custom
painting (History rows, the editor canvas, drag images) reads the tokens when
it paints; IconArt glyphs set through `Ui::icon(name)` redraw in the
new scheme; the check, radio, chevron and spin box step (#233) images are painted from the
tokens into a private temporary folder. A checked, disabled check box keeps the
`selected` fill with a `textMuted` tick, and disabled radio buttons use a `border`
ring with a `textMuted` dot, so a setting that is on never looks off (#300). The two former dark sets (the
window stylesheet's and the settings stylesheet's near-copies such as `#111318` for
`#111317`) are now one, the window stylesheet's.

Every text token meets WCAG AA (4.5:1) on `bg`, `surface`, `surfaceRaised`
and `selected` in both schemes, the accent (focus rings) and icon colour meet
3:1 on them, and the status tones meet 4.5:1 on their tinted surfaces.
Disabled text is exempt, as in WCAG. `gui_theme_contrast` checks text, focus,
scrollbar, slider, live-indicator, locked-readout and input-boundary pairs in
both schemes. It runs on pushes to `nightly`, not on pull requests, so run
`ctest -R gui_theme_contrast` after changing a token.

Colours that are not theme colours stay fixed: the brand mark, user content
(screenshots, annotation colours, redaction black), the custom-colour
rainbow swatch, the on-screen region selection ring and the scrim behind a
video poster's play glyph. Region selectors draw no scrim (#629); their hint pill and
size label use `surfaceRaised` and `text` from the current scheme, with no outline.

## Recording overlays

`putgd-record`'s windows use the same scheme and tokens (#198). The resident
resolves `preferences.appearance.theme` against the desktop with the same
`Appearance` code as `putgd-ui` (it does not restyle itself) and passes the
scheme in the helper's bootstrap frame; a change while a recording is
prepared, running or under review reaches the open helper as an `appearance`
frame and restyles it live (see [recording](recording.md)).

| Element | Colours |
| --- | --- |
| Control bar | `bg` fill, `text`, `surfaceRaised` controls, `accent` check box with a tick painted from `onAccent`; frameless, with no outline |
| Record and Stop buttons, state dot | fixed green and red (white text, 4.5:1 or better), the same in both schemes |
| Region selector hint and size label | an opaque `surfaceRaised` label with `text` and no outline, so it reads over any content |
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
`SettingChanged`. Cinnamon's XApp portal does not serve `org.cinnamon.theme`, so
on Cinnamon the resident also follows dconf's change notice
(`ca.desrt.dconf.Writer` `Notify`) for that schema: a desktop theme picked on its
own (Themes, Advanced settings, Desktop) re-tints the tray too
([#702](https://github.com/patrick-hudson/put.gd-desktop/issues/702),
[evidence](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-29-cinnamon-panel-theme-702/README.md)). The theme
is read with `gsettings`, which reads dconf's database under the resident's own
`XDG_CONFIG_HOME`: a lab run with a private configuration home must share the
session's `~/.config/dconf` for this, as the events recipe does. The resident's own session environment matters: it needs the
session's `XDG_CONFIG_DIRS` and `XDG_DATA_DIRS`. Lab tools that start it over SSH
through `tools/lab/lab.py`'s `guest_session` take both from the desktop shell's
environment (#243). Each stage of the work has its own tint (`Theme::trayTints`),
each at least 3:1 on typical panels of its kind:

| Stage | Dark panels | Light panels | Source |
| --- | --- | --- | --- |
| Idle | `#D6D6D2` | `#383834` | the `icon` token |
| Capturing | `#E8B66D` | `#8F5B20` | the `accent` token |
| Processing | `#8DB4E8` | `#2F5E9E` | the tray's own blue |
| Uploading fill, Complete | `#62D69B` | `#1F7A4A` | the tray's own green |
| Failed | `#E59B93` | `#A44139` | the `danger` token |
| Recording dot | `#F0524F` | `#C8302B` | the `live` token |

The window palette has no blue, and its `success` green is too close to the idle
glyph to read as finished at 16 px, so the tray keeps those two of its own. The
menu glyphs use the idle tint. The resident's full diagnostics (`ui.snapshot`
`diagnostics`, and diagnostics packages) report the choice as
`trayTint {panel, source, color}`.

The item publishes its glyph as `IconPixmap`. Ayatana's and Canonical's
`indicator-application-service` (the tray of Ubuntu Budgie 26.04's default
applet, and of Unity) ignore pixmaps and draw a missing-icon symbol instead. When
the `StatusNotifierWatcher` belongs to a process whose name ends in
`indicator-application-service`, the item also writes the current glyph as a PNG
into `$XDG_RUNTIME_DIR/putgd/tray/` and publishes its absolute path as `IconName`:
GTK re-reads an icon search path only every few seconds, so a host looking up a
just-written name in `IconThemePath` draws the missing-icon symbol. Each change
writes a new file, because these hosts cache icons by name, and the last four are
kept, since a host can still be loading one it was told about.
The PNG is encoded directly, so no Qt image plugin is loaded into the resident.
Every other host keeps the pixmap only
([#1294](https://github.com/patrick-hudson/put.gd-desktop/issues/1294)).

### Seasonal glyphs

The tray icon follows the local calendar, every year: a turkey from November 1
to 30, a Christmas tree from December 1 to 31, and the ordinary mark otherwise.
Both are vector silhouettes in `src/ui/IconArt.cpp`, using the same panel tints
as the mark. The turkey uses a compact bowl and a rounded optical drawing for
16 physical pixels, rasterized directly instead of shrinking a larger bitmap.
The Qt tray icon and the resident's StatusNotifier item offer that frame alongside
24/48/72 px frames; the host selects the size it needs. The larger turkey frames
use their own vector silhouette. December's broad, three-tier fir uses a separate
five-point star and a shallow tray-shaped stand, leaving most of the frame for
the tree. Nothing else about the app icon changes.

| State | What the icon shows | Tooltip title |
| --- | --- | --- |
| Idle | the seasonal glyph, still | `put.gd - Happy Holidays!` (`put.gd` in other months) |
| Working stages | the seasonal glyph in the stage's tint; Uploading's green fill climbs the glyph's own height, head or star included | `Capturing`, `Processing` or `Uploading`, with the real activity rows |
| Complete | the seasonal glyph in green with the check badge, bottom-right (the ordinary mark while a recording or pause badge also shows) | `Upload complete`, or `Saved` |
| Failure, recording or paused | the ordinary mark with its badge | the status as usual |

In the seasons, active work keeps its ordinary title and activity rows, with the
greeting as the last line; only a truly idle tray makes the greeting its title.
The top-right badges (failure, recording, paused) draw on the ordinary mark
because their halo would erase the turkey's head; the seasonal glyph returns
when the badge clears. The check badge sits bottom-right, clear of the turkey's
head and the tree's star, so the seasonal glyph stays when no top-right badge
shows. No glyph animates.

### Activity stages

The icon shows one stage at a time, worked out from the active operations'
own states (`TrayController::stageOf`), and the furthest stage any of them has
reached:

| Stage | When | Icon |
| --- | --- | --- |
| Idle | nothing running | the glyph in the idle tint |
| Capturing | from the hotkey or tray press itself, before the operation is accepted, through the region picker, the capture delay and the capture, until the source hands over its picture | the glyph in the capture tint |
| Processing | importing or encoding the capture, editing, encoding a recording after Upload, saving the local copy (Retry local save included), checking the destination, reading text | the glyph in the processing tint |
| Uploading | a running operation with a destination in its `preparing`, `uploading`, `finalizing`, `sharing` or `reconciling` phase, or with a file uploading; so also while no bytes move (provider setup, finalizing, making a share link, checking an earlier upload) | the processing glyph with the green fill climbing with the real progress, never below a tenth; between transfers it keeps its level, and it is full once at least one file of the operations at this stage has uploaded and none still waits, transfers or checks its name (also while a share link is still being made), or once the upload is known to have completed |
| Complete | the last running operation finished `completed` and uploaded or saved something without an error, for 1.5 s; `Saved` when that attempt only saved (a local-only capture, or a Retry local save of an operation that is `completed`) | the green glyph with the check badge |
| Failed | an operation or action failed, for 8 s | the error glyph with its badge |

An operation never goes back to a stage it has passed, so a copy after the
upload stays green. Each working stage shows for at least 250 ms, and the icon
steps through every stage the run reached from the first one it showed. The
service batches its observations, so a quick operation can end before the tray
saw its processing or upload. Only a successful outcome fills that gap: it says
which stages ran (something uploaded, or only saved), so a short successful
upload from a hotkey reads Capturing, Processing, Uploading, Complete. A failure
waits out the stage on screen, steps through any higher stage the tray did see,
then shows its badge. A recording shows its own badge, not a stage, until
Upload hands it over.

New work always shows: it replaces a failure or completion still on the icon,
and a failure from other work waits until the running work has ended. The
failure badge leaves the icon after 8 seconds, when new work starts, or when
its attention row is opened. The tray menu's attention row (**Failed uploads
(N)** and the others) stays until it is opened, or, for failed uploads, until a
retry has uploaded every failed file, so nothing is lost when the icon resets.

`TrayCalendar` picks the glyph, and a running resident changes it without a
restart: at local midnight, on logind's resume signal, on each tray refresh, and
within five minutes of a clock or time zone change. `ui.snapshot`
`diagnostics.trayIcon` reports `{glyph, shown, localDate, calendarOverride,
activity, history}`: `glyph` is the calendar's choice, `shown` is what a badge
leaves on screen, `activity` the stage the icon shows (`idle`, `capturing`,
`processing`, `uploading`, `complete` or `failed`) and `history` its last
sixteen changes, oldest first, each `{activity, at}` with a UTC timestamp. Lab
runs check the stage sequence with it on trays that expose nothing to
accessibility tools.
`diagnostics.trayMenu` reports what tray hosts asked of the StatusNotifier menu: `shows`
(root `AboutToShow` or `AboutToShowGroup` calls), `opened` and `closed` (the root's events,
which not every host sends), `clicked` (the last eight rows a host reported clicked, each
`{id, name, label}`) and `rows` (the top-level rows served now, in order, separators left out,
each `{id, name, label, visible, enabled, submenu}`); `{"native": false}` for the Qt fallback
tray. Both carry `settle`, the last wait of a screen-reading row chosen in the menu (#2081),
empty until the first such row: `{command, awaitedClose, closeWaitMs, fadeMs, fadeSource,
waitMs}`, plus `rebasedOnClose` when a late close restarted the fade. `waitMs` is the wait put.gd
scheduled from the click, not a measured start time. Such a row first waits for the host's close
when the host reports events (a close seen before, or an `opened` event for this menu), at most
250 ms. It then waits out the rest of the compositor's fade of the closed menu, counted from the
close, or from the click when no close came (a later close restarts it). The fade allowance is
one 34 ms frame plus the visible part of the desktop's animation: 456 ms on KDE by default (`kwin-fadingpopups`, KWin's
fadingpopups time scaled by kdeglobals `AnimationDurationFactor`), 184 ms on GNOME
(`gnome-popup-animation`), 34 ms on Cinnamon (`cinnamon-menu`: on Mint 22.3 its menu is gone within
30 ms of the click, with or without menu effects), 34 ms when the fadingpopups effect or GNOME's
`enable-animations` is off or KWin animations are instant, and 250 ms on desktops not measured. Hotkeys, the main window and session methods never take this wait. It is read-only: the counters only grow over the resident's life, `rows` includes
hidden rows (`visible: false`), and the same object goes into diagnostics packages. It records
the dbusmenu calls and events hosts send, whatever caused them; it never opens the menu. The lab uses it to check the menu
on trays that export nothing to AT-SPI.

For lab previews and tests, `PUTGD_TRAY_CALENDAR_NOW` (a local ISO date, or date
and time) starts the tray's own clock at that moment and lets it run, so a run
started at `2026-11-30T23:59:50` crosses into December ten seconds later. It
moves only the tray glyph and greeting, is logged as a warning, and reports
`calendarOverride: true`. Don't change a builder's system clock to see a season. Set in the
surface driver's environment, it is passed through to the guest resident it starts.

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
[evidence/2026-10-20-appearance-60](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-20-appearance-60/README.md).
