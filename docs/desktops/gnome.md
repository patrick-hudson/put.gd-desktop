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

Native packages install the helper under
`/usr/share/gnome-shell/extensions/putgd@put.gd/`. For a new installation:

1. Install the matching DEB or RPM.
2. Log out of GNOME and log back in, so Shell discovers the new system extension.
3. Run `gnome-extensions enable putgd@put.gd` in the new session. It becomes active
   immediately; another logout is unnecessary.

Before that first re-login, `gnome-extensions enable` can report
`Extension "putgd@put.gd" doesn't exist` even though the package installed it.
Do not also copy the source directory into your user extension directory. A user copy can shadow a newer
system package. Check `gnome-extensions info putgd@put.gd` and retain the loaded
path/version in the acceptance receipt. If a previous manual installation exists,
disable it and remove only that owned user copy before testing the package.

After upgrading an enabled extension, log out of GNOME and log back in before
verifying the new version. Replacing the installed files or checking their hashes
alone does not establish which code the current Shell session has loaded.
The resident compares the `extensionVersion` the loaded extension reports in
`Hello` with the packaged `metadata.json` version (compiled in from
`integrations/gnome/metadata.json`). While an older one is loaded,
`capabilities` reports `helper.outdated: true` with `extensionVersion` and
`packagedExtensionVersion`, screenshots keep working, and a region recording
is refused with `helper_outdated`: "The put.gd GNOME Shell extension was
updated; log out and back in to load the new version" (#131). Bump
`metadata.json`'s `version` whenever the extension's D-Bus interface changes.

For a standalone helper installation, install the release's matching GNOME ZIP
with `gnome-extensions install /absolute/path/to/putgd-gnome50-VERSION.zip`, log out
and back in so Shell discovers it, then run
`gnome-extensions enable putgd@put.gd`. Keep its identity aligned with the
application. The native resident does not initialize Qt Wayland; the on-demand graphical helper uses the session's Wayland platform. Capture and clipboard actions remain available through the production CLI and GUI. Leave the clipboard shortcut unset until the user explicitly assigns one. Binding diagnostics report the actual result for every configured action; existing Shell/system bindings are never overwritten.

Disable with `gnome-extensions disable putgd@put.gd`. Remove a packaged helper
through the package manager; remove only its owned user directory for a manual
installation. Disable removes all helper bindings, signals, pending selectors, transfer buffers, and the exported endpoint. Re-enable recreates them once. Shell/session restart requires the resident application's helper discovery to reconnect and rebind accepted settings.

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
hover behavior. The [current Proton package receipt](../evidence/2026-10-15-proton-p4-p6-acceptance.md) records
visible active/idle companion text, the existing icon, native Activity/paste and
product-family post-use memory at `a5112bc` on a fresh GNOME session. It does not
certify every menu/lock/disable/actor-replacement case or the Shell heap. An
ambiguous or unavailable public actor leaves the companion hidden; no private-field
or replacement-icon fallback is used.

`tests/integration/test_gnome_bridge.cpp` proves bounded transfer decoding and
request metadata validation. It does not run GNOME, prove global shortcuts,
capture real Wayland pixels, or paste into another application. Real GNOME 50
single-display capture, native paste, live upload and post-upload memory evidence
was recorded in [the historical live-S3 campaign](../evidence/live-s3-memory/source114/README.md), with a
later bounded current-main check in
[issue #4](https://github.com/patrick-hudson/put.gd-desktop/issues/4#issuecomment-6152173663).
Those receipts identify the actual tested versions and cases; they do not mark
every scenario in `tests/desktop/gnome.yaml` complete. Full display geometry,
helper/package lifecycle and release acceptance remain separate gates in
[verification](../verification.md).

## Bridge contract

The helper exports `org.putgd.Gnome50` at `/org/gnome/Shell/Extensions/Putgd` on Shell's own session-bus connection. The native adapter resolves and pins the unique owner of `org.gnome.Shell`; it owns `org.putgd.App.Gnome50`, and the extension accepts only that name's current unique owner. D-Bus authenticates the session user. This prevents other session-bus names from submitting capture/clipboard requests and avoids using a caller allow-list workaround. Same-user process compromise is outside this session IPC boundary.

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
[diagnostics](../diagnostics.md)). The browser or file manager is therefore
never classified as put.gd's own window. The
[editor refusal receipt](../evidence/2026-10-19-gnome-editor-refusal.md)
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
disable, a lock or the resident leaving the bus does the same. Mutter offers
no per-window screencast exclusion: a recording of a whole monitor with no
other monitor to hold the bar contains it. The resident then reports
`overlayPlacement: handle` and has the helper shrink the bar to its dot-and-Stop
handle from Record on; the extension follows the bar's size changes and keeps
it in the corner (or, for a bar outside, on its monitor). Details are in `docs/recording.md`, "Wayland
selection, bounds and bar". The synthetic `gnome_ui_ownership_contract` covers
these methods; it is not graphical acceptance.

## UI ownership recovery test

When the final UI process exits, the adapter clears its registered PID. If that
clear fails, window capture remains disabled until the existing three-second
probe successfully repeats the clear. A newer live helper PID supersedes that
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

[The recovery receipt](../evidence/2026-10-08-ui-gnome-recovery.json) preserves the
unchanged-production failure and passing correction, source hashes and replay.
