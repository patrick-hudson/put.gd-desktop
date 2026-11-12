# Capture monitor, delayed capture and Repeat last region

Three capture additions from [#1363](https://github.com/patrick-hudson/put.gd-desktop/issues/1363).
Each one is native on every desktop adapter; no portal picker stands in for any of them.

| Addition | What it does | How it is started |
| --- | --- | --- |
| Capture monitor | Captures the monitor the pointer is on, at that monitor's native size | The `monitor` action: tray, dashboard, a hotkey (unbound by default), `action.invoke`, MCP `perform_action` |
| Delayed capture | Waits 3, 5 or 10 seconds before a region, window, monitor or desktop capture, a repeat, or Capture region & edit | `delaySeconds` on `action.invoke`; the tray and the dashboard pass `preferences.captureDelaySeconds`, and so does a hotkey when `preferences.captureDelayShortcuts` is on |
| Repeat last region | Captures the rectangle of the last region capture again, with no selector | The `repeat` action: tray, the Capture region button's context menu, a hotkey (unbound by default), `action.invoke`, MCP |

## Capture monitor

`monitor` is a full action in `config.json`, like `desktop`: it has its own output
route, per-action exception, hotkey and history name ("Monitor"). A document
written before it existed gets a copy of the `desktop` route with no hotkey
([configuration](configuration.md)).

The monitor is the one under the pointer **when the capture is prepared**: at the key
press or request for an immediate capture, and when the countdown ends for a delayed
one. Nothing is mapped and pointer focus never moves to find it, so a hover state
under the pointer is still in the picture. Each adapter asks its own desktop:

| Desktop | Where the pointer position comes from |
| --- | --- |
| Cinnamon / X11 | `QueryPointer` on the root window, against the RandR CRTC rectangles |
| GNOME | The Shell extension: `global.display.get_current_monitor()` when the request is prepared |
| Plasma | A one-shot KWin script that reads `workspace.cursorPos` and calls the resident back, as the window list does. ScreenShot2's "active screen" follows a user option, so it is not used |
| Budgie (labwc) | One `ext-image-copy-capture` cursor session per output. The session that reports the cursor's hotspot inside its buffer names the output; a cursor image that straddles two outputs is reported by both, and only one contains the hotspot |
| Hyprland | The `j/cursorpos` IPC request |

The result is that output alone, read the way a desktop capture reads it and never
scaled: a 2560×1440 monitor at 150% gives a 2560×1440 image. Only that output is
read. A pointer that is over no output (a gap in the layout) is refused with
`capture_target`.

## Delayed capture

`delaySeconds` is an option of the existing actions, not a new set of actions.
`action.invoke` accepts `0` (immediate), `3`, `5` or `10` for `region`, `window`,
`monitor`, `desktop`, `repeat` and `edit`, and refuses anything else with
`invalid_request` before an operation exists. An `edit` is delayed as the capture it
starts with is: its `source` must be `region` (the default), `window` or `desktop`.
An edit of the clipboard or a file reads no screen, so a delay on one is refused.

- **The tray and the dashboard** read `preferences.captureDelaySeconds`. The tray's
  **Capture delay** submenu and the same submenu in the context menu of the
  dashboard's Capture region, window, monitor and desktop and Edit & upload buttons
  set it, as one radio group: None, 3, 5 or 10 seconds. The submenu's own label names
  the current choice ("Capture delay: 3 seconds"). **Settings → Capture → Capture
  delay** has the same choice as **Wait before capturing**.
- **A hotkey capture is immediate by default**, whatever that delay is.
  `preferences.captureDelayShortcuts` (Settings → Capture → Capture delay → **Also
  delay captures started by a hotkey**) makes hotkeys wait for it too. An open
  menu usually holds the keyboard, so a hotkey cannot fire while one shows; with
  this on, press the hotkey first and open the menu during the countdown.
- **Capture region & edit** waits like Capture region, from the tray, the dashboard
  and (with the option above) its hotkey. The tray's Edit image entries (the
  clipboard, a file) never wait.
- **`action.invoke` and MCP `perform_action`** use the `delaySeconds` they are given
  and never read the preference.

The operation is accepted and journalled first, so its ID is returned at once and
`operation.cancel` (MCP `cancel_operation`) works for the whole delay.

### Order

| Capture | Order |
| --- | --- |
| Region and Capture region & edit, live selection | The area is chosen first, then the countdown runs, then the area is captured |
| Region, screenshot first | The countdown runs first, then every monitor is frozen, then the area is chosen on the frozen frame |
| Window | The countdown runs, then the window that is active at that moment is frozen and captured |
| Monitor | The countdown runs, then the monitor under the pointer at that moment is captured |
| Desktop, Repeat last region | The countdown runs, then the capture |

So a delayed window or monitor capture is of what is active when the delay ends,
which is what the delay is for: open a menu, hover a button, hold a key.

### The countdown

One pill at the top centre of a monitor: "Capturing in 3 · Click to cancel". It takes
no keyboard focus and covers only itself, so the rest of the screen stays usable
while it counts. A click on it cancels, and so does `operation.cancel`. A cancelled
countdown captures nothing and the operation ends as `cancelled`.

The pill is never in the capture. Each desktop removes it and confirms that before
any pixel is read:

| Desktop | Surface | How it leaves the screen |
| --- | --- | --- |
| Cinnamon / X11 | An override-redirect window of `putgd-capture` (its countdown mode, or the same helper that selected the region) | Hidden, then the helper waits for the compositor's repaints over that area to settle, as it does for the region selector |
| GNOME | A reactive label in the Shell's chrome, drawn by the `putgd@put.gd` extension | Destroyed before the extension reads the stage |
| Plasma, Budgie, Hyprland | An overlay layer surface of `putgd-region` (its countdown mode, or the same helper that selected the region), scope `putgd-capture-countdown` | A fully transparent frame is committed and rendered first, because KWin animates a closing surface from its last frame and a screenshot renders that animation; then the surface is unmapped and a display sync orders the capture after it. Hyprland's `no_anim` layer rule covers the scope |

The residents on these desktops own no window, so the pill belongs to a capture
helper. A countdown that is not followed by a region selection runs the helper in its
countdown mode (`--countdown SECONDS SCHEME BUILD`): it shows the pill and exits `0`
when the delay ran out and the pill is gone, `10` when the pill was clicked, `11`
when the compositor never presented the pill's last frame (see below), `5` when it is
another build than the resident. It reads no pixels and takes no target. Once its
event loop ends the helper leaves with `_Exit`, skipping Qt's teardown: Qt Wayland's
display teardown could wait forever on its event thread (seen with Qt 6.10, #1332),
and the resident then killed the helper at its deadline and reported a cancelled or
finished countdown as `delay_unavailable`.

### A locked screen

A delay that ends on a locked screen captures nothing: the operation fails with
`session_locked`. Each adapter checks when the countdown ends: Cinnamon and
freedesktop screensaver services and logind's `LockedHint` where available,
Hyprland's own `locked` request as well, and on GNOME the extension, which also
drops every request the moment the Shell locks. Budgie uses the visibility
witness described below. A delayed live-region request on GNOME checks
`Main.sessionMode.isLocked` directly at expiry: screensaver/logind services are not
queried on its successful path, so a service reporting locked does not refuse that
request when the Shell itself remains unlocked.

Not every locker reports its state: Budgie's gtklock sets no `LockedHint` and offers
no service to ask, and Omarchy's lock leaves the hint off too. So on Plasma, Budgie
and Hyprland the countdown itself is the second witness. Before it leaves the screen
the pill commits one more frame and waits for the compositor's frame callback. A
compositor that is showing a lock screen, or has the output blanked, does not present
client surfaces and never sends it; after three seconds the helper gives up, the
countdown is reported as not shown, and the capture is refused with `session_locked`
("The countdown was not on screen when the capture delay ended (a locked or blanked
screen), so nothing was captured."). The wait is that long because a busy compositor
answers late: one second was missed once by a software-rendered guest that was
starting a browser.

Budgie also requires fresh transparent layer-surface frames before and after each
output-copy batch or window copy, including immediate agent requests, monitor capture
and repeated regions. If either frame is withheld, the pixels are discarded with `session_locked`;
this conservatively refuses an unlocked display that is blanked or powered off too
([Budgie](../desktops/budgie.md)). labwc offers no client-held lock boundary, so an
entire lock-and-unlock between the two visibility frames cannot be detected.

## Repeat last region

`repeat` is a hotkey-only entry in `config.json`. It always uses the `region`
action's output, and its operation is recorded as a region capture.

The resident remembers the rectangle of the last region that was actually captured
(Capture region or Capture region & edit), with the monitor layout it was chosen on.
It is kept in the resident's memory only: quitting put.gd forgets it, and nothing is
written to the configuration, the journal or the logs.

- With nothing remembered, `repeat` is refused with `repeat_unavailable`: "There is
  no region to repeat yet. Capture a region first."
- When the monitor layout differs from the remembered one (an output added, removed,
  moved, resized, rescaled or rotated), it is refused with `repeat_layout_changed`:
  "The monitor layout changed since the last region capture. Capture a region again."
  Only the monitors are compared (name, native and logical rectangle, scale,
  rotation), so a layout that is put back exactly as it was works again. On X11 the
  comparison leaves out the desktop-settings fingerprint a capture otherwise checks,
  because any settings rewrite changes that.
- Otherwise the same rectangle is captured with no selector, through the path a
  selected region takes, so the result has the same size.

`capabilities.source.capture.repeat.remembered` says whether a region is remembered.

## Capabilities

`capabilities.source.capture` reports `monitor` and `repeat` beside `region`,
`window` and `desktop`, and `capabilities.source.delay` reports
`{available, seconds: [3, 5, 10], indicator}` with the reason when a desktop cannot
show the countdown. Settings → Advanced → Desktop support details shows a **Monitor
capture** or **Delayed capture** row when one is unavailable. On GNOME an extension
loaded before these existed answers `helper_outdated` for them until the next login
loads the packaged one; window and desktop screenshots keep working meanwhile and a
region capture is refused as with any older loaded extension
([GNOME](../desktops/gnome.md)).

## Errors

| Code | When |
| --- | --- |
| `repeat_unavailable` | Repeat last region with no region captured since put.gd started |
| `repeat_layout_changed` | Repeat last region after the monitor layout changed |
| `session_locked` | The screen was locked when a capture delay ended, or the countdown was not on screen at that moment |
| `delay_unavailable` | The desktop cannot show the countdown, or its helper failed |
| `capture_target` | Capture monitor with the pointer over no monitor, or the desktop did not report the pointer |
| `invalid_request` | `delaySeconds` other than 0, 3, 5 or 10, on an action that takes none, or on an edit of the clipboard or a file |

## Tests

- `capture_delay_contract` (`tests/unit/test_capture_delay.cpp`): the operation
  service against a fake adapter: a countdown that ends on a locked screen, or is
  cancelled during the lock query, reads no pixel.
- `budgie_labwc_integration`: on a real headless labwc, the countdown's lock query
  reports a real session lock through the visibility witness.
- `gnome_ui_ownership_contract`: a countdown that ends on a locked screen, or whose
  lock disables the extension, answers `session_locked`.
- Ordering, validation, Repeat last region, Capture monitor, the countdown pill, the
  tray and Settings controls and the MCP mapping are checked by desktop acceptance
  only. Desktop acceptance, with a two-monitor run per desktop, is in the
  [acceptance matrix](../testing/acceptance-matrix.md).
