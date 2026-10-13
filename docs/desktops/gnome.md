# GNOME 50 / native Wayland

This profile uses the resident Qt Wayland process and the `putgd@put.gd` Shell extension. It never calls the restricted `org.gnome.Shell.Screenshot` service as a spoofed caller, enables unsafe mode, or substitutes a portal picker. The extension owns capture, Shell key bindings, and CLIPBOARD MIME transfer; storage, PNG decoding/composition, artifact files, secrets, and networking remain in the native shared pipeline.

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
`/usr/share/gnome-shell/extensions/putgd@put.gd/`. After installing the package,
enable it with `gnome-extensions enable putgd@put.gd`; do not also copy the source
directory into your user extension directory. A user copy can shadow a newer
system package. Check `gnome-extensions info putgd@put.gd` and retain the loaded
path/version in the acceptance receipt. If a previous manual installation exists,
disable it and remove only that owned user copy before testing the package.

For a standalone helper installation, install the release's matching GNOME ZIP
with `gnome-extensions install /absolute/path/to/putgd-gnome50-VERSION.zip`, then
enable it. Keep its identity aligned with the application. A fresh Wayland login
may be needed for Shell to discover a newly installed extension. Run the resident application with `QT_QPA_PLATFORM=wayland`. Capture and clipboard actions remain available through the production CLI and GUI. Leave the clipboard shortcut unset until the user explicitly assigns one. Binding diagnostics report the actual result for every configured action; existing Shell/system bindings are never overwritten.

Disable with `gnome-extensions disable putgd@put.gd`. Remove a packaged helper
through the package manager; remove only its owned user directory for a manual
installation. Disable removes all helper bindings, signals, pending selectors, transfer buffers, and the exported endpoint. Re-enable recreates them once. Shell/session restart requires the resident application's helper discovery to reconnect and rebind accepted settings.

## Acceptance boundary

`tests/integration/test_gnome_bridge.cpp` proves bounded transfer decoding and
request metadata validation. It does not run GNOME, prove global shortcuts,
capture real Wayland pixels, or paste into another application. Real GNOME 50
single-display capture, native paste, live upload and post-upload memory evidence
is recorded in [source114](../evidence/live-s3-memory/source114/README.md), with a
later bounded current-main check in
[issue #4](https://github.com/patrick-hudson/put.gd-desktop/issues/4#issuecomment-6152173663).
Those receipts identify the actual tested versions and cases; they do not mark
every scenario in `tests/desktop/gnome.yaml` complete. Full display geometry,
helper/package lifecycle and release acceptance remain separate gates in
[verification](../verification.md).

## Bridge contract

The helper exports `org.putgd.Gnome50` at `/org/gnome/Shell/Extensions/Putgd` on Shell's own session-bus connection. The native adapter resolves and pins the unique owner of `org.gnome.Shell`; it owns `org.putgd.App.Gnome50`, and the extension accepts only that name's current unique owner. D-Bus authenticates the session user. This prevents other session-bus names from submitting capture/clipboard requests and avoids using a caller allow-list workaround. Same-user process compromise is outside this session IPC boundary.

Shell hotkeys freeze their external window actor/frame or clipboard revision inside the compositor callback, then dispatch a prepared UUID ticket to the adapter. GUI/CLI requests freeze through Prepare before journal work yields. Protocol version 1 uses opaque UUID request IDs, frozen preparation tickets, one-time acquisition, explicit cancellation/release, bounded PNG/MIME bytes, and at most 64 KiB decoded bytes per base64 chunk. Metadata includes exact byte lengths and each capture frame's identity, logical/native rectangles, density, and orientation. The adapter validates metadata before decoding on a worker. Clipboard revisions come from CLIPBOARD owner changes without reading payloads. Result transfer checks a frozen CLIPBOARD revision and copy epoch at ownership commit, so changed owners or newer invocations return `copy_suppressed`. Missing endpoints, incompatible protocol/Shell versions, stale tickets, vanished external targets, changed monitor layouts, unsupported offers, oversized offers, cancellation, and transfer deadlines produce distinct actionable results.

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
