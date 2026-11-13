# Budgie adapter

Budgie 10.10 runs on labwc, a wlroots compositor with no screenshot or
screencast service of its own. put.gd captures through standard Wayland
protocols on its own connections: no ScreenCast portal, no picker, no `grim` or
`slurp`. Hotkeys use Budgie's labwc bridge, or the GlobalShortcuts portal
where a desktop offers it ([Hotkeys](#hotkeys)).
On Budgie 10.10/labwc, screenshots are refused with `session_locked` while the display is blanked or powered off even if the session is unlocked; keep the display awake for unattended captures.

Budgie 10.9 is an X11 desktop and uses the existing X11 adapter. Budgie 11
(Magpie on Mir) is unreleased; see [Budgie 11](#budgie-11).

Tracking issue: [#1275](https://github.com/patrick-hudson/put.gd-desktop/issues/1275).

## Versions

| Budgie | Session | Ships in | put.gd |
|---|---|---|---|
| 10.9.x | X11, Magpie v0 (Mutter fork) | Ubuntu 24.04, 25.10, Debian 13 | X11 adapter, unchanged |
| 10.10.x | Wayland, labwc | Fedora 44/45, Ubuntu 26.04, Debian forky, Arch | `budgie-wayland` adapter |
| 11.x | Wayland, Magpie on Mir | not released | compatible by protocol, not tested on a real Budgie 11 desktop |

## Routing

A Budgie session sets `XDG_CURRENT_DESKTOP=Budgie` (forge test sessions use
`Budgie:GNOME`). The resident takes the Core route with the `budgie-wayland`
adapter when that token is present, the session is Wayland, and
`QT_QPA_PLATFORM` is unset or names Wayland first. Fedora 45's
`startbudgielabwc` sets `QT_QPA_PLATFORM=wayland;xcb`; Fedora 44 and Ubuntu
Budgie 26.04 set `wayland`; this route accepts both. A session that
names Budgie never gets the GNOME or KDE adapter. Bare labwc (`labwc:wlroots`)
and other wlroots compositors keep the unavailable source: nothing here has
accepted them.

`capabilities` reports `adapter: budgie-wayland`, the outputs, each capture
kind with its reason when unavailable, the clipboard protocol, the hotkey
backend with every binding's state, and `imageCopy.protocols`, the version of
every global the compositor offers. Nothing is chosen by compositor name.

## Protocols

| Need | Protocol | Notes |
|---|---|---|
| Screen and region pixels | `ext-image-copy-capture-v1` + `ext-output-image-capture-source-manager-v1` | Required. No `wlr-screencopy` path. No cursor in screenshots. |
| Window pixels | `ext-foreign-toplevel-image-capture-source-manager-v1` | labwc 0.20+, Mir 2.29+. Missing: window capture reports `window_unavailable` with the protocol named. |
| Window list | `ext-foreign-toplevel-list-v1` | The only list whose handles capture accepts. |
| Active window | `zwlr-foreign-toplevel-management-unstable-v1` | The only list with an activated state. |
| Outputs | `wl_output` v4 names, `zxdg_output_manager_v1` | Logical rectangles, density, rotation. |
| Region selector | `zwlr_layer_shell_v1` through LayerShellQt | The `putgd-region` helper, as on Plasma. |
| Clipboard | `ext-data-control-v1` (or `wlr-data-control` v2) | Shared with Plasma. |

Labwc drops a selection when its owner disconnects, and stock Budgie has no
clipboard manager. A helper keeps a copied link available after a normal quit, so it
doesn't disappear with put.gd. See [Clipboard after quitting](../design/clipboard-keeper.md).

The capture, toplevel and data-control code lives in `src/platform/wayland/`
and is compositor-neutral; only the hotkey bridge and the D-Bus trigger are
in `src/platform/budgie/`. Protocol XML is vendored unmodified
(wayland-protocols 1.45, wlr-protocols) and installed beside the notices.

Every capture maps one `wl_shm` buffer and releases the compositor buffer and
pool when the frame is ready. A rotated or flipped output's buffer is turned
upright; its native size must equal the frozen layout or the capture fails
with `display_changed`, as does any output change during selection.

## Window capture

The window hotkey, `action.invoke window` and the CLI freeze the active
window at invocation: the one ext handle whose app ID and title match the one
activated wlr handle. No match, or more than one (two windows with the same
app ID and title), fails with `capture_target`; there is no fallback to region
selection. put.gd's own windows (`putgd`, `putgd-ui`, `putgd-record`,
`putgd-region`) are refused by app ID: Wayland gives no PID for a toplevel, so
this is weaker than the X11 and KDE PID rules.

On labwc 0.20 the result is the client's content only: the server-side title
bar is not included, and a window covered by another still captures its own
pixels (occlusion does not apply, unlike KDE and X11).

Where window capture is unavailable (labwc 0.9 on Fedora 44 and Ubuntu 26.04),
pressing the window hotkey or button fails with `window_unavailable` and a
notification naming the missing protocol (no fallback to region selection),
the Capture window button carries the alert badge and the protocol in its
tooltip, and Settings → Advanced → Desktop support details shows a Window
capture row with the same reason
([#1295](https://github.com/patrick-hudson/put.gd-desktop/issues/1295)).

![Ubuntu Budgie 26.04 on labwc 0.9.3: the Capture window button with its alert badge, its tooltip naming the missing protocol, and the Window capture row in Desktop support details](https://raw.githubusercontent.com/patrick-hudson/put.gd-evidence/9457580e1236faa8ff065950ab7145d13cc88b4a/records/2026-11-08-budgie-acceptance-round2-1275/images/budgie-window-unavailable-ui.png)

`recording.windows` (MCP `list_recording_windows`) lists the capturable
windows with their identity (the ext toplevel identifier), app ID, title and
active state; `rect` is empty because neither protocol gives geometry. Window
titles never appear in `capabilities` or diagnostics.

## Capture monitor, delay and repeat

Capture monitor (#1363) needs the output the pointer is on, and labwc gives
clients no pointer position and has no IPC to ask. put.gd creates one
`ext-image-copy-capture` cursor session per output on a short connection of its
own and takes the output whose session reports the cursor's hotspot inside its
buffer. A cursor image that straddles two outputs is reported by both; only one
contains the hotspot. No surface is mapped for this and pointer focus never moves,
so a hover state under the pointer is still in the capture. A seat without a
pointer, or a compositor without cursor sessions, is refused with `capture_target`
and the reason. `capabilities.source.capture.monitor.pointer` names the method.

Repeat last region reads the outputs its remembered rectangle lies on. A delayed
capture shows its countdown as an overlay layer surface of `putgd-region` (scope
`putgd-capture-countdown`) with no keyboard interactivity; it commits a transparent
frame, unmaps and syncs before anything is read.

labwc has no lock query, and Budgie's locker (gtklock, started through
`org.buddiesofbudgie.BudgieScreenlock`) sets neither logind's `LockedHint` nor a
screensaver service. The countdown is its own witness instead: its last frame must
be presented before it leaves, and labwc presents no client surface under a session
lock, so a delay that ends on a locked or blanked screen is refused with
`session_locked`. Immediate desktop, window, monitor and repeated-region captures
require a fresh transparent layer-surface frame both before and after their output-copy
batch or window copy; if gtklock withholds either frame, put.gd discards the pixels and returns
`session_locked`. This also protects requests made by an agent. See
[capture monitor, delay and repeat](../design/capture-delay.md).

## Hotkeys

The resident owns `org.putgd.App.Budgie` on the session bus. A bound key runs

```
dbus-send --session --type=method_call --dest=org.putgd.App.Budgie /org/putgd/App org.putgd.App.Budgie.Trigger string:<action>
```

so no binary path is written into the desktop's settings (an AppImage moves)
and no process starts per key press. `Trigger` accepts only an action put.gd
has bound; with no resident running the call fails harmlessly.

The backend is chosen when hotkeys are bound:

1. **labwc bridge** (Budgie 10.10): the
   `org.buddiesofbudgie.settings-daemon.plugins.media-keys` schema answers and
   `$XDG_CONFIG_HOME/budgie-desktop/labwc/rc.xml` exists.
2. **GlobalShortcuts portal**, when the desktop's portal offers it (Budgie
   10.10 does not).
3. **None**: each binding reports `shortcut_unavailable` with the reason; the
   tray and CLI still work.

With the bridge, each action is a relocatable custom keybinding at
`.../custom-keybindings/customputgd<action>/`, named `put.gd: Region capture`
and so on. The id contains `custom` because the bridge only removes stale
entries whose id does. The bridge turns it into
`<keybind bridge="customputgd<action>" key="C-S-1">` in `rc.xml`, about 10 s after
login and within ~100 ms afterwards. Before writing, put.gd compares the
key's modifier set and keysym with every `key=` in `rc.xml`; a key Budgie
already uses (for example `Print` for its own screenshot) reports
`shortcut_conflict` naming the owner and is not written, so Budgie's binding
survives. A binding stays `pending` until its `rc.xml` entry appears.
`rc.xml` is what put.gd reports as bound, so a change made in Budgie Control
Center shows in Settings. While Settings records a new hotkey, put.gd's
keys are cleared from `rc.xml` so the key reaches the recorder. Unbinding
clears the binding and command, drops the path from `custom-keybindings` and
resets the keys; quitting put.gd removes all of its entries. `gsettings` runs
with the host environment, never the AppImage's libraries.

![Settings → Shortcuts (now Hotkeys) on Fedora 45 Budgie: Print reported as a conflict with Budgie's own screenshot binding](https://raw.githubusercontent.com/patrick-hudson/put.gd-evidence/6f522d18635d7789dc271bf3be1b5cf6bf752e43/records/2026-11-08-budgie-adapter-1275/images/budgie-shortcut-conflict.png)

## Recording

Recording uses the `wlr-capture` acquisition: `ext-image-copy-capture`
frames of one output or one window go from `putgd-record`'s own Wayland
connection into `appsrc` and the existing cadence lock, pause, budget and
encoder pipeline. The cursor is painted into the frames. The ScreenCast
portal is never used on Budgie: a missing or refused protocol is the error,
with the protocol named, and no chooser ever shows. Details are in
[Wayland recording](../design/recording/wayland.md).

- Monitor: one output. Desktop: only with a single output (record a monitor
  otherwise).
- Region: cropped from the output it lies on; a region spanning outputs is
  `recording_unsupported`.
- Window: an empty `target.identity` means the window active when the
  recording is prepared, resolved by the resident; otherwise an identity from
  `recording.windows`. A window that changes size is scaled, with borders, into
  its starting size; a window that closes ends the recording
  (`target_closed`) and keeps the clip.
- The ready ring, dimming and control bar are layer surfaces outside the
  recorded rectangle. A monitor recording includes the bar, as on KWin 6.3.
- The cursor is painted by the compositor, except in a recording with click
  highlights, where the recorder draws it (below).

### Click highlights

Recordings of a monitor, the desktop or a region can draw a ring at each click
(`recording.capabilities`: `clickHighlights {available: true, route:
"labwc-mouse-binds", drawnBy: "recorder", targets: [monitor, desktop,
region]}`). labwc has no event interface, so a press reaches put.gd in two
halves; the design and the upstream sources are in
[Microphone and click highlights](../design/recording/microphone-and-clicks.md#budgie-two-halves).

- **The press.** From Record to Stop, `rc.xml` carries put.gd's own
  `<context name="Client" putgd="click-highlights">` and `<context
  name="Titlebar" putgd="click-highlights">` with `Press` binds on the three
  buttons (in `Client` also with Shift, Ctrl, Alt, Super and Ctrl+Shift). Each
  keeps the actions of the bind it replaces (the user's own, or labwc's default
  `Focus` and `Raise`) and adds an `Execute` that calls
  `org.putgd.App.Budgie.Click` on the session bus. labwc reads the file again
  on `SIGHUP`, which shows as the pointer turning into the default arrow for
  about 130 ms and nothing else; the binds go in a quarter of a second before
  the capture starts and come out after it has stopped. The press still
  reaches the window.
- **The file is Budgie's.** `labwc-bridge` rewrites `rc.xml` from its own tree
  at the next setting it mirrors. While recording, the resident puts its binds
  back; otherwise it removes any it finds, also at start after a crash. Nothing
  but elements carrying `putgd="click-highlights"` is added or removed, and the
  file after a recording is the file before it. This is the one place the
  adapter writes `rc.xml`: Budgie's settings offer key bindings (see
  **Hotkeys**) and no mouse binding.
- **The position.** The recording is captured without the compositor's painted
  cursor; `putgd-record` reads the pointer from the capture's cursor session
  and draws the cursor into the frames itself, so the clip still shows it.
- **Not highlighted.** A window recording (`prepare` refuses click highlights
  for it: a window capture has no cursor session in wlroots, and labwc reports
  no window geometry); presses on the panel and other layer-shell surfaces
  (only a bind that would swallow the press matches there); presses with a
  modifier combination other than those above; presses while the application
  hides the pointer; a session without a hardware cursor, or a turned output
  (the recording then keeps the compositor's painted cursor and
  `progress.clickHighlightsError` says there is no pointer position; measured
  on labwc 0.20.2 started with `WLR_NO_HARDWARE_CURSORS=1`: the cursor is in
  the clip, no ring is drawn, and `inspect` gives the reason). Settings →
  Advanced → Desktop support details names the window-recording limit in its
  Click highlights row.

`labwc_bridge_contract` checks the edit of `rc.xml`: put.gd's binds come out
byte for byte, a broken file is left alone, and binds a crash left behind are
removed. That a press reaches both put.gd and the window is checked by Budgie
desktop acceptance only.

## Scrolling capture

Scrolling capture ([design](../design/scroll-capture.md)) reads `ext-image-copy-capture` frames of the
output the region lies on, cropped to the region, through the same `wlr-capture` source as a recording
and with its session created without `paint_cursors`. The wheel is a `zwlr_virtual_pointer_v1` on the
helper's own Wayland connection, not a portal and not a bind: pointer motion is `motion_absolute`
mapped to that output (manager version 2 maps a pointer to one of several outputs; version 1 is
refused when there are several), and each notch is a wheel `axis_source`, an `axis_discrete` of 15.0
with step 1 and a `frame`, as wayvnc and lan-mouse send it. labwc offers the manager with no client
filter, so no access is needed. Region selection is the helper's layer-shell selector, with the ready
ring and bar outside the rectangle. Because a frame may show the pointer, the capture keeps
appended rows at least 48 px below the parked pointer and holds it outside the region while a
way to the top is scrolled. The `scroll` shortcut goes through the labwc bridge or the
GlobalShortcuts portal like the other actions, with no default key.

## Copy text

Copy text ([design](../design/text-recognition.md)) selects a region exactly as **Capture region**
does: the same selector, cleanup and presentation-based lock witness. A text capture never changes
the rectangle that **Repeat last region** remembers. The `text` hotkey goes through the labwc bridge
or the GlobalShortcuts portal like the other actions, with no default key, and its label is "Copy
text from region". The text lands on the clipboard through the clipboard keeper like any
other result ([clipboard keeper](../design/clipboard-keeper.md)), so it still pastes after the
resident quits and the keeper exits when another application copies. Recognition runs in the confined `putgd-ocr` helper on every desktop (see [text recognition](../design/text-recognition.md)), which needs the kernel's Landlock at version 3 or later (Linux 6.2 or newer with Landlock enabled).

## Budgie 11

Budgie 11 has no release and no installable session. The adapter works from
advertised protocols, so Magpie built from the forge gets capture, clipboard,
region and window capture where Mir advertises them (Mir 2.29+ for windows).

Magpie keybinds (`magpierc`) are not written. With real keys on a direct-seat
Magpie 58a0d7a, modifier plus printable combos fire (`Ctrl+Alt+b`, `Ctrl+Shift+K`),
but a binding has to name the shifted symbol (`Ctrl+Shift+!` for physical
Ctrl+Shift+1), and Magpie's key translation can't match F keys, Return, Print or
keys with no modifier
([#1289](https://github.com/patrick-hudson/put.gd-desktop/issues/1289)).

Headless Magpie (Mir 2.29 `mir:virtual`) passes capture, window capture,
clipboard, region (virtual pointer, exact pixels) and monitor and window
recording. Window recording failed 2 of 4 runs until #1296 (a prepare right after
a cancel) was fixed, then passed 5/5 fresh sessions. `mir:virtual` has no
compositing, so only a capture consumes a window's buffers, and a capture session
can get a single stale frame
([#1305](https://github.com/patrick-hudson/put.gd-desktop/issues/1305)): judge
Magpie motion on a real output, where captures and recordings are steady.

**No real Budgie 11 desktop has been tested.** Acceptance runs on the first
Budgie 11 alpha ([#1290](https://github.com/patrick-hudson/put.gd-desktop/issues/1290)).

## Tests

- `budgie_labwc_integration` runs the adapter and shared capture against a
  real headless labwc (`WLR_BACKENDS=headless WLR_RENDERER=pixman`): nothing
  behind a lock screen is captured (a locked desktop, a lock during the copy,
  the countdown's lock query), the visibility witness never answers from another
  compositor or session, and no clipboard keeper takes the clipboard back from an
  app that copied later. Pixels, geometry, output transforms, scale, clipboard
  round trips and click highlights are left to Budgie desktop acceptance. The
  Ubuntu 26.04 CI leg installs labwc; the test skips where labwc is absent. Headless results are supporting evidence,
  never desktop acceptance.
- Routing rows in `test_native_session.cpp` cover `Budgie`, `Budgie:GNOME`,
  `labwc:wlroots` and `wayland;xcb`.
- The VM rows are in [the acceptance matrix](../testing/acceptance-matrix.md).
  The 2026-11-08 results (Fedora 44/45, Ubuntu Budgie 26.04/24.04, headless
  labwc and Magpie) are in the
  [evidence record](https://github.com/patrick-hudson/put.gd-evidence/tree/6f522d18635d7789dc271bf3be1b5cf6bf752e43/records/2026-11-08-budgie-adapter-1275);
  link opening in the real browser, A/V sync, fps against KDE, the tray, the
  window-capture gap and Magpie are in
  [round 2](https://github.com/patrick-hudson/put.gd-evidence/tree/9457580e1236faa8ff065950ab7145d13cc88b4a/records/2026-11-08-budgie-acceptance-round2-1275).

## Tray

Ubuntu Budgie 26.04's default tray applet is Ayatana's AppIndicator applet. Its
`indicator-application-service` ignores pixmaps, so put.gd also names its tray
glyph there ([Tray icon](../design/appearance.md#tray-icon), #1294). Fedora's
Budgie tray and Budgie 10.9's X11 tray draw the pixmap. Menu entries without a
shortcut send no `shortcut` property: libdbusmenu rejects an empty one, which
budgie-panel logged as "Unable to parse shortcut, too many keys".

![Tray glyphs after the fix: Ubuntu Budgie 26.04 draws the named glyph, Fedora 44 and 45 the pixmap; before, the Ayatana applet drew a missing-icon symbol](https://raw.githubusercontent.com/patrick-hudson/put.gd-evidence/9457580e1236faa8ff065950ab7145d13cc88b4a/records/2026-11-08-budgie-acceptance-round2-1275/images/budgie-tray-named-icon.png)
