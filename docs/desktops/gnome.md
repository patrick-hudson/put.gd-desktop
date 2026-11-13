# GNOME 50 / native Wayland

This profile uses the resident `QCoreApplication` process with a native session-bus bridge and the `putgd@put.gd` Shell extension. It never calls the restricted `org.gnome.Shell.Screenshot` service as a spoofed caller, enables unsafe mode, or substitutes a portal picker. The extension owns capture, Shell key bindings, and CLIPBOARD MIME transfer; storage, PNG decoding/composition, artifact files, secrets, and networking remain in the native shared pipeline.

## Native API evidence

API inspection on 2026-10-08 used the upstream `gnome-50` branches:

- [Shell screenshot API](https://raw.githubusercontent.com/GNOME/gnome-shell/gnome-50/src/shell-screenshot.h) and [implementation](https://raw.githubusercontent.com/GNOME/gnome-shell/gnome-50/src/shell-screenshot.c): area capture writes PNG to a supplied stream with cursor excluded; capture dimensions follow the compositor's final scale. For region selection, each enabled monitor is captured independently after the transparent selector is removed; target/layout preparation still precedes selection.
- [Shell screenshot UI](https://raw.githubusercontent.com/GNOME/gnome-shell/gnome-50/js/ui/screenshot.js): crosshair/rubberband selection follows native Clutter/St actors; `paint_to_content()` captures a frozen window actor without looking up a new focus target.
- [Mutter selection API](https://raw.githubusercontent.com/GNOME/mutter/gnome-50/src/meta/meta-selection.h), [selection enum](https://raw.githubusercontent.com/GNOME/mutter/gnome-50/src/meta/meta-selection-source.h), and [implementation](https://raw.githubusercontent.com/GNOME/mutter/gnome-50/src/core/meta-selection.c): MIME enumeration, `owner-changed`, bounded transfer size, and cancellable asynchronous reads. The installed GNOME 50 typelib exposes `Meta.SelectionType.SELECTION_CLIPBOARD`; `St.ClipboardType.CLIPBOARD` belongs to a separate enum. `St.Clipboard.get_content()` uses an unlimited transfer internally, so imports use bounded `Meta.Selection.transfer_async()` instead.
- [St clipboard implementation](https://raw.githubusercontent.com/GNOME/gnome-shell/gnome-50/src/st/st-clipboard.c): `set_content(CLIPBOARD, mime, bytes)` offers PNG/file-URI/text results without changing PRIMARY.
- [Mutter display configuration](https://raw.githubusercontent.com/GNOME/mutter/gnome-50/data/dbus-interfaces/org.gnome.Mutter.DisplayConfig.xml): read-only `GetCurrentState` provides stable connector identity, configuration serial, physical modes, fractional scale and transform. No display configuration is modified.
- [Shell modal input API](https://raw.githubusercontent.com/GNOME/gnome-shell/gnome-50/js/ui/main.js), [grab helper](https://raw.githubusercontent.com/GNOME/gnome-shell/gnome-50/js/ui/grabHelper.js), and [Clutter grab API](https://raw.githubusercontent.com/GNOME/mutter/gnome-50/clutter/clutter/clutter-grab.h): the extension owns a St selector, its captured events and its paired modal grab/pop. GNOME 50 exposes `is_revoked()`; the older `get_seat_state()` API is absent. Selector setup failure, cancellation, and completion all release the actor/grab. The selector actor is transparent, so Shell continues painting the desktop during selection. Region screenshot acquisition begins after the actor is removed and the modal grab is released; no background still frame is displayed. This live-background refinement still needs fresh GNOME graphical acceptance, including outline-free final pixels. Drag positions describe half-open rectangle edges, matching the shared Qt selector: a drag by 80×60 logical units selects 80×60, and a zero-area drag cancels.
- [Shell key binding implementation](https://raw.githubusercontent.com/GNOME/gnome-shell/gnome-50/js/ui/windowManager.js): checked binding action IDs and explicit binding removal, together with [Mutter accelerator grabs](https://raw.githubusercontent.com/GNOME/mutter/gnome-50/src/meta/display.h).

These sources establish API intent. They do not establish graphical acceptance.

## Setup

Use normal distro packages: `gnome-shell` 50, its GJS/Mutter GI runtime, the `gnome-extensions` command supplied by Shell, and the standard GLib runtime. The extension preferences GUI is optional. The application additionally links the distro Qt DBus module (`qt6-base-dev`) and needs the distro Wayland platform plugin (`qt6-wayland`). Root packaging installs the extension files; do not install a substitute Shell/GJS runtime.

A GNOME 50 user does one thing after installing put.gd by any route (one-line
installer, DEB, RPM or an AppImage): start it, then **log out and back in once**
when it asks (the state is `login_required`). It doesn't ask when the extension is
already loaded and current, and another state says when a logout won't help. The
resident does the rest, in the session, as the user; a package's post-install
script runs as root and can't set per-user GNOME state.

- Native packages install the helper under
  `/usr/share/gnome-shell/extensions/putgd@put.gd/`. The resident uses that system
  copy.
- An AppImage carries the matching extension. On first start, when no system copy
  exists, the resident installs it to
  `$XDG_DATA_HOME/gnome-shell/extensions/putgd@put.gd/` (by default under
  `~/.local/share`) and writes a
  `.putgd-managed` marker (the files' SHA-256 digests) next to it. It never installs
  a user copy beside a system copy, because a user copy shadows a newer system
  package, and it never overwrites a copy it didn't create: no marker, a file that
  no longer matches, or anything added to the directory reports `foreign_user_copy`
  and leaves it alone. GNOME loads a user copy ahead of a package's, so an older
  manual copy must be removed before the package can load. A
  copy it owns is kept in step with the running AppImage (never downgraded) and is
  removed if a system copy later appears.
- `putgd --remove-gnome-extension` (or `putgd.AppImage --remove-gnome-extension`)
  removes that owned copy and its setting, then asks you to log out to unload it.
  Quit put.gd first. Deleting the directory by hand also works.

**Why one logout is enough.** Before the Shell has discovered an extension,
`gnome-extensions enable` refuses it (`Extension "putgd@put.gd" doesn't exist`), but
the `org.gnome.shell enabled-extensions` setting is read at the next Shell start, so
adding the UUID first makes the extension activate at that login with no second step.
The resident appends to that list through `gsettings` with the host environment
(`host::environment()`), so an AppImage's libraries never reach it, and every
existing entry keeps its exact text. Only GNOME 50 sessions run this; nothing changes
on other desktops, other GNOME versions, or when the extension is already loaded and
current. If the Shell already knows the extension (a package installed before the
last login), appending activates it immediately, with no logout.
A deliberate off switch stays off: an entry in `disabled-extensions`
(`gnome-extensions disable`, the Extensions app) is respected and reported.

**One state.** `capabilities.source.helper.setup`, the `desktop.setup` and
`desktop.setup.recheck` session methods, `putgd-cli` and the MCP helper report the
same value: `ready`, `login_required` ("Log out and back in once to finish setting up
put.gd."), or one of the failures in [errors](../user/errors.md#gnome-50-setup-states).
A notification and the capture status in the app say the same thing. The state is
derived from the running Shell and the loaded extension, so it stays honest after any
change made outside put.gd. A connected but older extension asks for a logout when
that would load the update: `install_failed` is reported even while the old extension
runs, and so is any other state a logout can't fix. `settings_unavailable` is the
exception that stays `login_required`, because the files are already in place and a
loaded extension proves it is enabled.

The resident rechecks at start, when the Shell's bus owner changes, and on
`desktop.setup.recheck`. It reads the session's `XDG_CONFIG_HOME` and `XDG_DATA_HOME`
from the systemd user manager so a resident with a private environment (the lab
harness) cannot read one dconf database and write another.

Check `gnome-extensions info putgd@put.gd` and retain the loaded
path/version in the acceptance receipt.

After upgrading an enabled extension, log out of GNOME and log back in before
verifying the new version. Replacing the installed files or checking their hashes
alone does not establish which code the current Shell session has loaded.
The resident compares the `extensionVersion` the loaded extension reports in
`Hello` with the packaged `metadata.json` version (compiled in from
`integrations/gnome/metadata.json`). While an older one is loaded,
`capabilities` reports `helper.outdated: true` with `extensionVersion` and
`packagedExtensionVersion`, the setup state is `login_required` unless a logout can't
load the update (see **One state** above), window and desktop
screenshots keep working (an extension too old to report `uiProcessVersion` refuses
window capture with `helper_incompatible` while the put.gd UI is open), and a region capture, a window recording, `recording.windows`
or a region recording that needs the Shell's selector is refused with
`helper_outdated`: "The put.gd GNOME Shell extension was updated; log out and back in
to load the new version" (#131), or the setup state's own message when a logout
can't help. A recording request that already carries its
rectangle skips that selector check.
Bump `metadata.json`'s `version` whenever the extension's D-Bus interface changes.

The native resident does not initialize Qt Wayland; the on-demand graphical helper uses the session's Wayland platform. Capture and clipboard actions remain available through the production CLI and GUI. Leave the clipboard hotkey unset until the user explicitly assigns one. Binding diagnostics report the actual result for every configured action; existing Shell/system bindings are never overwritten.

Disable with `gnome-extensions disable putgd@put.gd`. Remove a packaged helper
through the package manager and an AppImage's owned copy with `--remove-gnome-extension`; remove only your own user directory for a manual
installation. Disable removes all helper bindings, signals, pending selectors, transfer buffers, and the exported endpoint. Re-enable recreates them once. Shell/session restart requires the resident application's helper discovery to reconnect and rebind accepted settings.

## Modal dialogs

GNOME's session defaults `org.gnome.mutter attach-modal-dialogs` to `true`. On
Fedora 44 this comes from `[org.gnome.mutter:GNOME]` in
`00_org.gnome.shell.gschema.override`, so `gsettings get` shows that default only
when `XDG_CURRENT_DESKTOP=GNOME` is set. A value the user set wins over it either
way. With the setting on, Mutter attaches each modal dialog to its parent and drags
the parent along with it. Qt 6.8 and later mark modal windows through
`xdg-dialog-v1`, so the editor's Custom color dialog used to drag the whole
editor. On GNOME, `putgd-ui` and `putgd-record` therefore add `xdg_wm_dialog_v1`
to `QT_WAYLAND_DISABLED_INTERFACES` before Qt starts
(`src/ui/DialogAttachment.cpp`). Their dialogs then move on their own whatever the
setting says. Qt still blocks the parent, the dialog stays above it, and programs
put.gd starts get the user's own value back. Plasma does not attach dialogs.
Cinnamon leaves `org.cinnamon.muffin attach-modal-dialogs` off by default. A
user who turns it on gets attached put.gd dialogs there, because the X11 modal
hint is not covered by this setting
([evidence](https://github.com/patrick-hudson/put.gd-evidence/tree/3268fed84fc28f2ecfbb274bb1a9a2ba17816c59/records/2026-11-07-gnome-attached-dialogs)).

## Acceptance boundary

On GNOME 50, Ubuntu AppIndicators renders the native icon/menu but omits SNI
hover text. The put.gd extension supplies a plain-text companion on the existing
public St panel actor. It accepts one registered `Id=putgd` item with the D-Bus
Unix PID of the current `org.putgd.App.Gnome50` owner, rechecks unique owners
after asynchronous reads, and matches only one actor whose `accessible_name`
equals that authenticated item's `Title`. It reads genuine `ToolTip` text,
bounded to 2048 UTF-8 bytes and four description rows. It creates no second icon,
uses no AppIndicators private fields, and does not change its menu or hover
handlers. Button presses propagate to the native host and hide the companion
until the pointer leaves and returns. Once bound, updates use native signals rather than idle bus polling;
actor-missing setup retries are bounded. Lock, owner changes and disable cancel
reads and remove only the companion's own label/signals/timers.

The existing `gjs -m tests/desktop/test_gnome_ownership.mjs
integrations/gnome/extension.js` suite adds synthetic tooltip ownership,
updates, ambiguity, bounds and lifecycle contracts. They prove no graphical
hover behavior. The [current Proton package receipt](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-15-proton-p4-p6-acceptance.md) records
visible active/idle companion text, the existing icon, native Activity/paste and
product-family post-use memory at `a5112bc` on a fresh GNOME session. It does not
certify every menu/lock/disable/actor-replacement case or the Shell heap. An
ambiguous or unavailable public actor leaves the companion hidden; no private-field
or replacement-icon fallback is used.

`tests/integration/test_gnome_bridge.cpp` proves bounded transfer decoding and
request metadata validation. It does not run GNOME, prove hotkeys,
capture real Wayland pixels, or paste into another application. Real GNOME 50
single-display capture, native paste, live upload and post-upload memory evidence
was recorded in [the historical live-S3 campaign](../evidence/live-s3-memory/source114/README.md), with a
later bounded current-main check in
[issue #4](https://github.com/patrick-hudson/put.gd-desktop/issues/4#issuecomment-6152173663).
Those receipts identify the actual tested versions and cases; they do not mark
every scenario in `tests/desktop/gnome.yaml` complete. Full display geometry,
helper/package lifecycle and release acceptance remain separate gates in
[verification](../testing/verification.md).

## Bridge contract

The helper exports `org.putgd.Gnome50` at `/org/gnome/Shell/Extensions/Putgd` on Shell's own session-bus connection. The native adapter resolves and pins the unique owner of `org.gnome.Shell`; it owns `org.putgd.App.Gnome50`, and the extension accepts only that name's current unique owner. D-Bus authenticates the session user. This prevents other session-bus names from submitting capture/clipboard requests and avoids using a caller allow-list workaround. Same-user process compromise is outside this session IPC boundary.

Discovery is event driven (#1187). The adapter sends `Hello` when it starts, when the owner of `org.gnome.Shell` changes, and when the extension emits `CapabilitiesChanged`, which it does after exporting on every enable, including the one after an unlock, and before unexporting on disable. The signal subscription stays in place while the Shell keeps its connection, and it is sent before `Hello`, so an enable cannot fall between the two. While the helper is unavailable, a failed `Hello` still retries as a safety net for a lost signal or a transient failure: after 3 s, then doubling to one probe every 15 minutes. Each timed retry asks the resident to release the code pages it maps again after the usual thirty seconds of quiet, without postponing a release that is already scheduled. An idle resident with the extension disabled or locked no longer wakes every 3 s.

Extension **v18** (PR #1056) captures a window on its head's output pixel grid,
the same grid used for desktop and region capture of that head. When
`paint_to_content()` returns a backing texture at a ceiled scale (for example
2× on a 1.5× head), the extension resamples it with linear filtering onto the
head's grid. Integer-scale captures keep the previous path. A client already
rendering at fractional scale can have slightly softer edges because Mutter
upscales its actor before this downscale. Verify the loaded version after
logging out and back in; installed files alone do not prove v18 is running.

Shell hotkeys freeze their external window actor/frame or clipboard revision inside the compositor callback, then dispatch a prepared UUID ticket to the adapter. GUI/CLI requests freeze through Prepare before journal work yields. Protocol version 1 uses opaque UUID request IDs, frozen preparation tickets, one-time acquisition, explicit cancellation/release, bounded PNG/MIME bytes, and at most 64 KiB decoded bytes per base64 chunk. Metadata includes exact byte lengths and each capture frame's identity, logical/native rectangles, density, and orientation. The adapter validates metadata before decoding on a worker. Clipboard revisions come from CLIPBOARD owner changes without reading payloads. Result transfer checks a frozen CLIPBOARD revision and copy epoch at ownership commit, so changed owners or newer invocations return `copy_suppressed`. Missing endpoints, incompatible protocol/Shell versions, stale tickets, vanished external targets, changed monitor layouts, unsupported offers, oversized offers, cancellation, and transfer deadlines produce distinct actionable results.

Window capture refuses put.gd's own windows with `capture_target` before any
layout work. A focused window is put.gd's when Mutter's client PID is the
authenticated resident's, the views helper's registered through
`SetUiProcess`, or that of any other process whose parent (from
`/proc/PID/stat`) is the resident. The last rule covers the Edit & upload
editor (`putgd-ui --edit-image`) and the recording helper, which never register
(#103). Links open through the portal's `OpenURI` request; folders go
`ShowItems` → `OpenDirectory` → launcher. When a portal request fails for a
reason other than the user cancelling or the portal not answering, the
launcher runs `gio open` (or `xdg-open`) with `QProcess::startDetached`, so the
opener runs in its own session and is not a child of the process that launched
it; a cancelled or unanswered portal request skips the launcher (see
[diagnostics](../design/diagnostics.md)). The browser or file manager is therefore
never classified as put.gd's own window. The
[editor refusal receipt](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-19-gnome-editor-refusal.md)
records the physical Fedora 44 runs from the RPM and the AppImage.

Recording protocol 1 (`recordingVersion: 1` in `Hello`) serves screen
recording, which must not show its own surfaces on GNOME. `SelectArea(id)`
runs the same St selector as a region screenshot and answers only the
logical rectangle (no capture); `Release(id)` cancels it. `ShowRecordingFrame`
takes `{rect, kind, state: ready|recording|paused, pid}` and draws the ready
ring and, while recording or paused, dim strips as non-reactive Shell actors
outside the rectangle, and moves the recording helper's control bar (window
title `put.gd recording controls`) out of the rectangle with
`move_frame`/`make_above`. The bar is accepted only when its process's parent
(from `/proc/PID/stat`) is the authenticated resident, so a request cannot move
another application's window. `HideRecordingFrame` removes the actors; a
disable, a lock or the resident leaving the bus does the same.

Placing the bar takes several waits: up to 2 s for its window to appear, up to
1 s for it to be shown, and 100 ms after each of up to five moves, because
Mutter places a new window when it is first shown and undoes an earlier move.
The bar can go away during any of them (the helper hides it, exits or is
killed), and a `Meta.Window` object outlives its window. The extension
therefore watches the bar from the moment it finds it: on Mutter's
`unmanaging` or `unmanaged` signal the bar is no longer live, the placement
answers `bar: {placed: false, outside: false}`, the size-change follower drops
it, and no method of that window is called again. The lookup also skips a
window whose actor `is_destroyed()`, since the actor stays listed while it
fades out. Hiding the frame (the bridge call, a disable, a lock, the resident
leaving the bus) ends a placement that is still waiting: it answers
`cancelled`, and neither the frame nor a scroll capture's beat is shown again.
Before this, `meta_window_move_frame()` on the unmanaged Wayland window read
through NULL inside Mutter and ended the session: GNOME Shell 50.6 took a
SIGSEGV in `meta_window_update_monitor` when a scroll capture was cancelled
right after it was prepared, and when a recording was cancelled or its helper
killed while its bar was being placed (2026-11-11, #1376). After a Shell crash
gnome-session also turns every user extension off
(`org.gnome.shell disable-user-extensions`). Mutter offers
no per-window screencast exclusion: a recording of a whole monitor with no
other monitor to hold the bar contains it. The resident then reports
`overlayPlacement: handle` and has the helper shrink the bar to its dot-and-Stop
handle from Record on; the extension follows the bar's size changes and keeps
it in the corner (or, for a bar outside, on its monitor). Details are in `docs/design/recording.md`, "Wayland
selection, bounds and bar".

Capture version 2 (`captureVersion: 2` in `Hello`, extension version 20, #1363)
adds Capture monitor, Repeat last region and the capture countdown:

- `Prepare(id, "monitor")` freezes `global.display.get_current_monitor()`, the
  monitor the pointer is on, with the request, and `Acquire` reads that monitor
  alone with `screenshot_area`, at its native size.
- A region's `Acquire` metadata carries its `selection` and a `layoutKey` (each
  monitor's connectors, logical rectangle, scale and rotation). The resident keeps
  both in memory; `Prepare(id, "repeat")` then `Acquire` with that `area` and
  `layoutKey` captures the rectangle again, reading only the monitors it lies on,
  and refuses with `repeat_layout_changed` when the key no longer matches.
- `Countdown(id, {seconds, appearance})` draws the countdown as a reactive label
  in the Shell's chrome (`addTopChrome` with `trackFullscreen` only: Shell 50 throws
  on any chrome parameter it does not know), at the top centre of the pointer's
  monitor and below the top bar. It takes no grab and no focus; a click on it cancels, `Release(id)`
  removes it, and its actor is destroyed before the method answers, so a capture
  that follows cannot contain it. A live region's `Acquire` takes `delaySeconds`
  instead and shows the same countdown after the selection and before the pixels.
- A lock ends a counting request with `session_locked`. The Shell disables its
  extensions when it locks, sometimes before the extension's own session handler
  runs; the extension answers `session_locked` from `disable()` too, and the
  resident asks logind whether the session is locked (for up to two seconds)
  before it lets a helper error from a counting request stand.

While an extension older than the packaged one is loaded, window and desktop
screenshots work as before and a region capture is refused with `helper_outdated`,
as it already was for any older loaded extension. `monitor`, `repeat` and any
delayed capture answer `helper_outdated` too until the next login loads the
packaged one, and Desktop support details shows **Monitor capture** and **Delayed
capture** rows with that reason.
See [capture monitor, delay and repeat](../design/capture-delay.md).

Recording protocol 2 (`recordingVersion: 2`, extension version 19) adds
`RecordingWindows()`: the Alt+Tab list (`get_tab_list(NORMAL_ALL)`) as
`{identity: Mutter window id, title, appId, rect, active, minimized}` with
the focused window `active`, leaving out every window `_residentWindow`
attributes to put.gd (`activeExcluded` says the focused one was). The resident
resolves a window recording from it and the helper passes the id to Mutter's
`RecordWindow` (`window-id`); see `docs/design/recording/wayland.md`, "Window
recordings". The synthetic `gnome_ui_ownership_contract` covers these methods;
it is not graphical acceptance.

Recording protocol 3 (`recordingVersion: 3`, extension version 21) reports
pointer button presses for click highlights. `WatchClicks()` starts and
`UnwatchClicks()` stops the watch; each press of the left, middle or right
button is the signal `Click(json)` with `{x, y, button, window?: {identity,
rect}}` in logical desktop coordinates, sent to the unique bus name of the
resident that asked and to nobody else. A disable, a lock or the resident
leaving the bus ends the watch.

Mutter's own event filter runs before any the extension can add and stops the
events it delivers to an application, so a stage `captured-event` handler and
an extension `Clutter.Event.add_filter` never see a click on a window (measured
on Shell 50.6: 0 of 10 each). What Mutter does on every such press is stamp the
window's user time, synchronously, while the press is still Clutter's current
event. The extension connects `notify::user-time` on every window (and on each
new one) and reads button, position and time from
`Clutter.get_current_event()`; a key press stamps the user time too and is told
apart by the event's type. Presses on the Shell's own actors (panel, overview,
window decorations it draws) arrive at the stage's `captured-event`, and one
press seen by both hooks is reported once. Ten injected clicks from 50 ms down
to 0 ms long were all reported. Nothing is grabbed, consumed or polled.

The resident watches from Record to Stop (`RecordingClicks`,
`src/core/RecordingClicks.h`) and needs `recordingVersion >= 3`: with an older
extension loaded, `recording.capabilities` reports `clickHighlights.available:
false` with `helper_outdated` or `helper_incompatible`, and a recording that
has the remembered preference on starts without rings. The route is
`gnome-shell-extension`, the rings are drawn by the recorder into the clip, and
all four targets have them; see
[Microphone and click highlights](../design/recording/microphone-and-clicks.md).

## Scrolling capture

Scrolling capture ([design](../design/scroll-capture.md)) adds the `scroll` action (extension
version 22). The extension selects the region and draws the ring (`SelectArea`,
`ShowRecordingFrame`, as for a recording), and the resident places the bar through it. The helper
reads frames from Mutter's `RecordArea` with `cursor-mode` 0 inside a ScreenCast session linked to a
RemoteDesktop session, and turns the wheel through the same session
(`NotifyPointerMotionAbsolute` on the area stream, `NotifyPointerAxisDiscrete`). The order is
Mutter's: RemoteDesktop `CreateSession`, its `SessionId`, ScreenCast `CreateSession` with
`remote-desktop-session-id`, `RecordArea`, then `Start` on the RemoteDesktop session only. Mutter
accepts `Start`, `Stop` and every `Notify*` only from the D-Bus connection that created the session, so
one connection makes all the calls. The interface is private to Mutter and promises no
compatibility; no owner for `org.gnome.Mutter.RemoteDesktop`, or a refused `CreateSession`, is
`scroll_unavailable` with reason `injection_unavailable`. The Shell shows its screen-sharing
indicator while the session runs; a click on it closes the session, and the helper reads that as Stop.

`ShowRecordingFrame` takes `beat: true` for a scroll capture's frame (`GnomeAdapter::showFrameImpl`).
While that frame shows, the extension adds one transparent 1×1 `St.Widget` at the rectangle's
corner and queues it for redraw every 40 ms. Mutter's area stream sends a frame only for damage inside
its area, and at 125%, and on some logins at 100%, a page at rest sent none, so the end of the
content could not be proved and the capture stopped `lost_track`. Queueing the same pixel on the stage
with a clip sent nothing; a widget in the area sends about seven frames in 300 ms of a still page.
The actor is the only one inside the area, a new frame replaces it and its timer, and hiding the frame
ends both. `gnome_ui_ownership_contract` covers the timer.

A scrolling capture needs the extension at version 22 or later (`ScrollShell::selectsRegions` also
requires `recordingVersion` 1 or later). An extension loaded before the update (below version 22)
ignores `beat` and has no `scroll` in its `ACTIONS`: the `scroll` shortcut reports `bound: false` with
`helper_outdated` until the next login, and every start (tray, main window, CLI, MCP) gets the region
error (`helper_outdated`, else `region_unavailable`) instead of a capture. Log out and back in once,
as for any extension update ([Setup](#setup)).

## Copy text

Copy text ([design](../design/text-recognition.md)) adds the `text` action to the extension's hotkey
list, so the packaged extension is **version 23**. The extension prepares a native region selection
for `text` as it does for `edit`, and the adapter keeps the prepared hotkey tickets for it by action.
A text capture never changes the rectangle that **Repeat last region** remembers.

As with every extension update, a session that still has version 22 loaded must log out and in
once ([Setup](#setup)). Until then:

- `GnomeAdapter` refuses region preparation for `text` with the existing "log out once" step
  (`helper_outdated`), so **Copy text from region** reports it, as Capture region and region
  recording do, and `capture.text` in capabilities says the same.
- The `text` hotkey reads `helper_outdated`, and a saved `text` hotkey that version 22 does not
  know is not counted as a failed restore.
- `text.recognize` from `window`, `monitor`, `desktop`, `clipboard`, `file` and `history`, the
  editor's text tools and History's actions work, because they need nothing new from the extension.
- Every existing gate keeps its version rule. A scrolling capture is still available from version
  22. Recognition runs in the confined `putgd-ocr` helper on every desktop (see [text recognition](../design/text-recognition.md)), which needs the kernel's Landlock at version 3 or later (Linux 6.2 or newer with Landlock enabled).

TO CONFIRM before merge: this section describes the desktops pull request (#1442), which is not
merged, and GNOME on f44 has not been driven with it. No acceptance of Copy text has run on GNOME.
Version 22 loaded with 23 installed, and the restore of every shortcut after login, are among the
cases item 8 of #1428 covers.

## UI ownership recovery test

When the final UI process exits, the adapter clears its registered PID. If that
clear fails, window capture remains disabled until a retry three seconds after
each failed clear succeeds. A newer live helper PID supersedes that
recovery. Live-helper registration failures remain blocked, and a new Shell
owner receives the latest PID during rediscovery.

The production adapter has a focused transport regression using native Qt Wayland
and a synthetic D-Bus peer on a **private test bus**. In an already-running
Wayland desktop, build with the normal dev preset and run:

```sh
QT_QPA_PLATFORM=wayland dbus-run-session -- \
  build/dev/test_gnome_bridge --adapter-transport
```

It opens no windows and changes neither the installed extension nor the real
Shell service. The three scenarios cover failed clear recovery followed by an
action ticket, preservation of a newer/live PID, and Shell-owner replacement.
The default `gnome_bridge_contract` remains a headless decoder test. This extra
mode fails if native Wayland is unavailable; it is not an offscreen replacement
for graphical capture or tray acceptance.

[The recovery receipt](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-08-ui-gnome-recovery.json) preserves the
unchanged-production failure and passing correction, source hashes and replay.
