# Hyprland adapter (Omarchy)

The `hyprland-wayland` adapter runs put.gd's resident on Hyprland in the Core
route: region, desktop and window capture, hotkeys, clipboard import
and result copy, and screen recording, with no picker, no dialog and no
substitute tool. Tray, notifications, file dialogs and the Nautilus upload menu
use the desktop's own services. Tracking issue
[#1275](https://github.com/patrick-hudson/put.gd-desktop/issues/1275).

## Status

| Target | Session | Status |
| --- | --- | --- |
| Omarchy 4.5.0 (Hyprland 0.56.2, Lua config) | Wayland, SDDM autologin, uwsm | **Accepted** on the `omarchy-hyprland-20261108` VM golden, AppImage and the Arch package ([acceptance matrix](../testing/acceptance-matrix.md#hyprland-omarchy-vm-row)) |
| Other Hyprland 0.56+ sessions with a Lua config (plain Arch, other dotfiles) | Wayland | Same adapter by protocol; **not accepted** |
| Hyprland on the legacy `hyprland.conf` parser | Wayland | Capture, clipboard and recording work; hotkeys report unavailable with that reason |

Omarchy runs in VMs only in the lab (software rendering, `virtio-vga`); no
physical Hyprland machine has been tested. Hyprland is an acceptance-matrix VM
row, not a nightly or release gate.

Versions on the accepted golden: Omarchy 4.5.0-3, kernel 7.2.15-1-omarchy,
Hyprland 0.56.2, quickshell (Omarchy's bar, dock, tray, notifications, lock and
idle), xdg-desktop-portal 1.22.1 with xdg-desktop-portal-hyprland 1.4.1 and
-gtk 1.15.3, Qt 6.12.0, layer-shell-qt 6.8.1, GStreamer 1.28.8, PipeWire 1.6.9,
Nautilus 51.1, Chromium 155.

## Routing

The resident takes the Hyprland Core route when all of these hold:

- `XDG_SESSION_TYPE=wayland` and `WAYLAND_DISPLAY` is set;
- `QT_QPA_PLATFORM` is unset or its first plugin is Wayland (Omarchy sets
  `wayland;xcb`);
- `XDG_CURRENT_DESKTOP` contains a `Hyprland` token and no KDE, GNOME, Cinnamon
  or Budgie token;
- `HYPRLAND_INSTANCE_SIGNATURE` names a request socket,
  `$XDG_RUNTIME_DIR/hypr/<signature>/.socket.sock`, that accepts a connection.

Nothing is chosen by the word Omarchy. `capabilities` reports
`adapter: hyprland-wayland` and diagnostics `resident: hyprland-core`.

## Protocols

| Protocol | Use |
| --- | --- |
| `ext-image-copy-capture-v1` with the output and foreign-toplevel image capture sources | Desktop, region and window pixels (the shared Wayland layer). Screenshots exclude the cursor; recordings paint it |
| `ext-foreign-toplevel-list-v1` | Window identities; the identifier equals Hyprland's `stableId` |
| `hyprland-toplevel-mapping-v1` | Confirms that the identifier a window capture reads is the window IPC reported |
| `hyprland-global-shortcuts-v1` | put.gd's `putgd:<action>` hotkeys |
| `wlr-layer-shell` (layer-shell-qt, and directly for the press catcher) | The region selector (`putgd-region-selector`), the press catcher (`putgd-region-catcher`) and recording overlays (`putgd-record`) on the overlay layer, above Omarchy's bar, dock and notifications |
| `ext-data-control-v1` | Clipboard import and result copy |

put.gd does not use `wlr-screencopy`, `hyprland-toplevel-export`, grim, slurp,
the Screenshot portal, the GlobalShortcuts portal or the ScreenCast portal (its
share picker). Both Hyprland protocol files are vendored unmodified from
hyprwm/hyprland-protocols (commit `cd1aba6`, BSD-3-Clause) and installed with
the package's notices.

Hyprland's IPC is spoken directly on its sockets, never through `hyprctl`, so an
AppImage's library path never reaches a child process: `j/activewindow`,
`j/clients`, `j/binds`, `binds`, `eval <lua>` and `reload` on `.socket.sock`,
and the `configreloaded>>` event on `.socket2.sock`. Right after a reload
Hyprland briefly refuses connections; requests retry within a 3 s deadline.

## Capture

- **Region**: the shared `putgd-region` helper on the overlay layer covers the
  whole output, Omarchy's bar and notifications included. Live selection is the
  default; screenshot-first mode reads every output before the selector maps.
  A layout change during selection ends it with `display_changed`.
- **Dragging straight after the hotkey**: the selector is a separate process, so
  its surfaces appear a moment after the hotkey. A press in that gap would go to
  the window underneath, which then keeps the whole drag. The resident therefore
  keeps one idle Wayland connection and maps a transparent catcher surface the
  moment the hotkey arrives (about 0.05 s on the Omarchy VM), and relays a drag
  that starts on it to the selector, which draws it. The catcher stays under the
  selector until the selection ends. Three Hyprland 0.56 behaviours shape this:
  only a layer surface that accepts the keyboard gets pointer focus without the
  pointer moving, so the catcher takes it on demand; a surface that takes the
  keyboard as it maps, or becomes exclusive, releases every held button and the
  real release is then swallowed, so the selector maps without the keyboard; and
  switching to on-demand afterwards releases nothing but only takes effect on the
  next pointer motion, so until then Esc and keyboard selection reach the selector
  through the catcher. A press Hyprland handles before it has processed the
  catcher (under heavy compositor load, such as just after an output appears on a
  2-vCPU VM) still goes to the window underneath; the selector then works normally
  for the next drag.
- **Desktop**: every output, composed at native density.
- **Window**: the window the hotkey or CLI call finds active, frozen through
  `j/activewindow`. Its `stableId` is the capture identifier and its `pid`
  decides ownership: put.gd's own windows (the resident, Settings, the editor,
  the recorder, the region helper, any process below the resident) are refused
  with `target_unavailable`, with the app ID as a second check. The
  toplevel-mapping protocol must map the identifier to the same window address,
  else `capture_target`. There is no fallback to region selection.

Window capture is client content only: no border, gaps, shadow or the
titlebar Omarchy's hyprbars plugin draws on floating workspaces. It works for a
window covered by another (only its own pixels), on another workspace and in a
hidden special workspace (the scratchpad), all measured on Omarchy 4.5.

Fractional scale (1.25) and rotated outputs capture upright at native density;
outputs of different scale are composed at the highest density. When an output
is set back to normal, Hyprland 0.56 sends an existing client only the
xdg-output size and `done`, with no `wl_output.geometry`; put.gd rebinds an
output whose `done` carries no wl_output change, so the running resident
captures the new geometry without a restart
([#1299](https://github.com/patrick-hudson/put.gd-desktop/issues/1299)).

## Capture monitor, delay and repeat

- **Capture monitor** (#1363): the output that contains the position `j/cursorpos`
  reports, read whole at native density. The request is sent at invocation, or when
  the countdown ends for a delayed capture.
- **Repeat last region**: the remembered rectangle, from the outputs it lies on.
- **Delayed capture**: the countdown is an overlay layer surface of `putgd-region`
  with the scope `putgd-capture-countdown` and no keyboard interactivity. put.gd's
  `no_anim` layer rule names that scope too, so the pill leaves in one step; it
  still commits a transparent frame, unmaps and syncs before anything is read.
  Hyprland's `j/locked` request answers the lock check when the delay ends, with
  logind's `LockedHint` and the freedesktop screensaver service beside it (Omarchy's
  lock leaves the hint off, so `j/locked` is the one that answers there). A countdown
  whose last frame was not presented is refused as well.

See [capture monitor, delay and repeat](../design/capture-delay.md).

## Capture permissions

Omarchy leaves `ecosystem:enforce_permissions` off, so any client may capture.
When a user enables it, Hyprland answers a denied client with a transparent
frame (it still draws the cursor), not an error. put.gd treats a frame that is
at least 99% `0x00000000` as `capture_permission` and names the rule to add for
the binary that is actually running. For the package:

```lua
hl.permission({ binary = [[/usr/bin/putgd]], type = "screencopy", mode = "allow" })
hl.permission({ binary = [[/usr/bin/putgd-record]], type = "screencopy", mode = "allow" })
```

An AppImage runs from a new `/tmp/.mount_<random>` directory on every start, so
its rules are regular expressions:

```lua
hl.permission({ binary = [[/tmp/\.mount_[^/]+/usr/bin/putgd]], type = "screencopy", mode = "allow" })
hl.permission({ binary = [[/tmp/\.mount_[^/]+/usr/bin/putgd-record]], type = "screencopy", mode = "allow" })
```

Hyprland only matches the path, and `/tmp` is writable by everyone: with these
rules, any program that runs from a matching `/tmp/.mount_…` path can capture the
screen without a prompt. Where `enforce_permissions` matters, use the native
package, whose rules name fixed system paths, or leave these rules out and approve
the AppImage per run at Hyprland's permission prompt.

Use Lua long strings (`[[...]]`): a `"\."` escape is a Lua error, and a Lua
error drops Hyprland back to its default configuration. Enforcement changes
need a Hyprland restart. An unlisted client gets Hyprland's "ask" dialog.

![put.gd reports a denied capture with the rule to add (CI build 26.1108.4146-dev+g3af1e6b2b0a5)](https://raw.githubusercontent.com/patrick-hudson/put.gd-evidence/3c57254830be09016030f82bf5b51d561f298bdf/records/2026-11-08-hyprland-omarchy-1275/pictures/omarchy-capture-permission-refused.png)

## Hotkeys

put.gd registers `putgd:region`, `putgd:window` and so on with
`hyprland-global-shortcuts-v1` on its own Wayland connection and binds each key
at runtime:

```lua
hl.bind("CTRL + SHIFT + 1", hl.dsp.global("putgd:region"), { description = "put.gd: Region capture" })
```

Nothing is written to `hyprland.lua`, `bindings.lua` or any other file. A press
arrives as the protocol's `pressed` event. The rules, from measurements on
Omarchy 4.5:

- **Conflicts are refused.** Hyprland fires every bind on a key, so before
  binding put.gd compares modifiers and key with every existing bind (including
  `code:` binds, which `j/binds` lists with an empty key; their key comes from
  the plain-text `binds` reply). A match is `shortcut_conflict` and the key is
  not bound. Omarchy's own `PRINT` (screenshot) and `SUPER + SHIFT + S` (Google
  Maps) are examples; put.gd's defaults `Ctrl+Shift+1/2/3` are free.
- **Removing a put.gd bind normally touches only put.gd's key.** `reload` drops every
  runtime bind, including other programs', so it isn't the normal path. `hl.unbind`
  matches the chord as written (spaces and case ignored, modifier order not), taking
  every bind written that way in every submap (measured on 0.56.2); put.gd writes its
  binds in one fixed order and calls it only for a chord nothing else holds. To change
  or remove its binds, pause for the hotkey recorder, or quit, put.gd unbinds those
  chords. If something else holds the chord, or put.gd's bind can't be read, it runs
  `reload` and re-applies its set: the configuration's binds come back, but other
  programs' runtime binds are lost, so this path is only for that shared-key case.
- **put.gd never calls a bind handle's `:remove()`.** A stale handle crashed
  Hyprland 0.56.2.
- **Binds come back after every reload.** Omarchy reloads Hyprland on theme and
  settings changes; each `configreloaded` event re-applies put.gd's binds and
  layer rule, and reconnecting the event socket does the same, because a reload
  can happen while it is away.
- **`j/binds` is the truth.** Only binds that read back are reported `bound`;
  put.gd's are the ones whose description starts with `put.gd: `. A bind left
  by a resident that crashed is adopted, not doubled; with no resident running
  it points at a hotkey nobody registered and does nothing.
- After a quit, `j/binds` holds exactly the configuration's binds.

On the legacy `hyprland.conf` parser `eval` is unavailable and hotkeys report
`shortcut_unavailable` with that reason; put.gd writes no `keyword` binds.

![Settings shows Ctrl+Shift+1/2/3 bound and Omarchy's Print and Super+Shift+S as conflicts (CI build 26.1108.4146-dev+g3af1e6b2b0a5)](https://raw.githubusercontent.com/patrick-hudson/put.gd-evidence/3c57254830be09016030f82bf5b51d561f298bdf/records/2026-11-08-hyprland-omarchy-1275/pictures/omarchy-settings-shortcuts-conflicts.png)

Settings → Hotkeys shows each binding's state, and `putgd-cli capabilities`
reports it under `source.shortcuts` (`backend: hyprland-ipc`, per-action
`state`, `actual`, `error`, and `hyprland` diagnostics).

## Recording

Recording uses the `wlr-capture` acquisition (`ext-image-copy-capture` frames
into `appsrc`), never the ScreenCast portal, whose `SelectSources` opens
`hyprland-preview-share-picker`. Recordings paint the cursor. A window
recording without an identity records the active window by `stableId`;
`recording.windows` lists external windows from Hyprland's IPC. `desktop` is a
target only with one output; with several, record a monitor. A region
recording crops one output; a region spanning outputs is
`recording_unsupported`. On Omarchy, install `gst-plugin-va` for hardware
encoding (the Arch package suggests it); without it recording uses x264 or
OpenH264. In the software-rendered lab VM the desktop itself draws about 8
frames per second at 1920x1080, and recordings keep that pace; system audio and
A/V sync (median -19 ms) were measured there too.

### Click highlights

Hyprland reports pointer buttons to binds. From Record to Stop the resident
holds three runtime binds on the bare mouse buttons (`mouse:272`, `mouse:273`,
`mouse:274`), written through `eval` like the hotkey binds and never into a
file. They are `non_consuming` and `ignore_mods`, so the click still reaches
the window under the pointer whatever modifiers are held (measured: a terminal
under the pointer received every click). Their Lua callback reads the cursor
position and sends `putgd-click <token> <button> <x> <y>` as a custom event on
the event socket (`custom>>`), which the resident reads; no process starts per
click.

The callback does nothing unless the global `putgd_clicks` holds this
recording's token, so a bind a crash left behind is inert and another program's
custom events are ignored. At Stop the token is cleared and the binds are
removed with `hl.unbind`, by this adapter's rule: only on a chord nothing else
holds, never through a bind handle. Where the user has a bind of their own on a
bare mouse button, put.gd's stays, inert, until Hyprland next reloads its
configuration; the next recording reuses it and binds only the buttons that
have none of put.gd's, so a kept bind is never doubled. The binds carry the description `put.gd: click highlight`.

`recording.capabilities` reports `route: "hyprland-mouse-binds"`, `drawnBy:
"recorder"` for all four targets, or `hyprland_ipc_unavailable` /
`hyprland_lua_unavailable` with the reason. Measured on Hyprland 0.56.2: four
clicks from 50 ms down to 0 ms long, left and right, all arrived with their
positions, and the bind count was the same before and after (251). In
recordings there (26.1110.4459): three presses, three rings, within 0.6 px of
the press, in monitor, region and window recordings; a window recording maps
each press through the window's place at that press, so a press outside the
recorded window draws nothing. The design
is in [Microphone and click highlights](../design/recording/microphone-and-clicks.md).

## Scrolling capture

Scrolling capture ([design](../design/scroll-capture.md)) uses the `wlr-capture` frames of a
recording for the region, with the session created without `paint_cursors`, and a
`zwlr_virtual_pointer_v1` on put.gd's own Wayland connection for the wheel and for pointer placement:
`motion_absolute` and wheel `axis_source`, `axis_discrete` (15.0, step 1) and `frame`. This is a
Wayland protocol, not a `hyprctl` call: it touches no Hyprland configuration file, no bind
handle and no `hl.unbind`, so the guarantees of this adapter hold. It binds with
`ecosystem.enforce_permissions = true`, since Hyprland's permission types contain no virtual-pointer
gate. The `scroll` shortcut is bound like the other actions (a runtime Lua bind through the
adapter's action table), with no default key.

Two Omarchy 4.5 facts shape the capture, both measured in the lab VM:

- **Hyprland 0.56 composites a software cursor into the output**, so every copy of it shows the arrow
  whatever `paint_cursors` says, and `hyprctl keyword cursor:invisible` is refused on the Lua parser.
  The stream cannot be made pointer-free. Instead the loop keeps the pointer out of the rectangle
  wherever a frame may become the image: it rests at a place outside the region for the first frame
  (the bottom right corner if there is none), keeps appended rows at least 48 px below the parked
  pointer, and on the way to the top leaves the rectangle again right behind every wheel step.
- **Omarchy's notification shell keeps a closed card on screen.** quickshell answers
  `CloseNotification` with `NotificationClosed` and leaves a visible card where it is (a critical one
  until it is dismissed, any other for at least 5 to 8 seconds), so put.gd's own notices would stand
  in the next capture. The resident also dismisses them through the shell's IPC by summary: `qs ipc
  --pid <owner of org.freedesktop.Notifications> call notifications dismiss <summary>`, run with that
  process's `XDG_RUNTIME_DIR`. A quickshell configuration without that target refuses the call at no
  cost, and a card of another application with the same summary goes too.

## Copy text

Copy text ([design](../design/text-recognition.md)) selects a region exactly as **Capture region**
does, with Hyprland's early press catcher (`HyprlandSourceAdapter`) armed for `text` as it is for a
region. A text capture never changes the rectangle that **Repeat last region** remembers. The bind
is `putgd:text`, a runtime Lua bind through the adapter's action table like the other actions, with
no default key, and its label is "Copy text from region". The text lands on the clipboard through
the clipboard keeper like any other result ([clipboard keeper](../design/clipboard-keeper.md)). The
adapter never writes the user's Hyprland configuration and uses no `hl.unbind` or bind handle's
`:remove()` for it. Recognition runs in the confined `putgd-ocr` helper on every desktop (see [text recognition](../design/text-recognition.md)), which needs the kernel's Landlock at version 3 or later (Linux 6.2 or newer with Landlock enabled).

## Clipboard, tray, notifications and files

- Clipboard import works while put.gd is unfocused; results copy as
  `image/png` with put.gd's private marker. PRIMARY is never written. Omarchy's
  clipboard history (`wl-paste --watch`) keeps recording in both directions.
- A result still on the clipboard stays there after a normal put.gd quit, although
  Hyprland drops a selection with its owner. Omarchy's clipboard history records
  copies but doesn't put them back on the clipboard. A crash, `kill -9` or a result
  over 64 MiB isn't kept. See [Clipboard after quitting](../design/clipboard-keeper.md).
- quickshell is the tray host: put.gd's item and its menu work.
- quickshell is the notification server. It keeps **critical** notifications
  in `~/.local/state/omarchy/notifications/live.json` until they are dismissed,
  across reboots. put.gd's critical notices (failures and warnings) stay
  critical, and on this server only (`GetServerInformation` name `quickshell`)
  they also carry the spec's `transient` hint, so quickshell shows them until
  dismissed but does not bring them back after a reboot
  ([#1298](https://github.com/patrick-hudson/put.gd-desktop/issues/1298)).
  History keeps each failed operation with its diagnostics; a file-manager upload
  refused before it became an operation (no destination, another installation
  running) is not in History, so that notice is the only record of it.
- File dialogs go through the GTK portal; Nautilus 51 shows **Upload with
  put.gd** (the packaged nautilus extension). Accepted with a real S3
  destination: a hotkey upload and a Nautilus upload, each link opened from
  put.gd in Omarchy's default browser (Chromium, through the portal)
  ([#1300](https://github.com/patrick-hudson/put.gd-desktop/issues/1300)). With
  the Arch package installed, its Nautilus entry refuses while an AppImage
  resident runs, as on every desktop. System, Dark and Light follow the
  portal's `color-scheme` (Omarchy: dark).

## Lab

Golden `omarchy-hyprland-20261108`, profile `omarchy-hyprland`
([Omarchy VM](../lab/omarchy-vm.md)). Animations are off in that VM
(`animations:enabled = false`). To observe the selector's `no_anim` layer rule
([#1301](https://github.com/patrick-hudson/put.gd-desktop/issues/1301)), turn
them on at runtime with a multi-second `layers`/`fadeLayers` animation and give
slurp a later rule with `no_anim = false` (Omarchy ships `no_anim` for its
`selection` layer): slurp then fades in over the animation while put.gd's
selector appears in one step, and a capture contains no selector.
Acceptance evidence: [2026-11-08-hyprland-omarchy-1275](https://github.com/patrick-hudson/put.gd-evidence/tree/82185b9d2ccfa39c976ab2fd36b45eac4ebe5c35/records/2026-11-08-hyprland-omarchy-1275). The Omarchy golden has no pacman sync databases: run
`sudo pacman -Sy` before `pacman -U`, never `-Syu`. Hyprland cannot run headless
in CI (aquamarine needs a DRM device), so CI covers routing, key and bind
normalisation, the bind plan, refusal frames and the hotkey backend against a
fake IPC server; the compositor itself is covered by the golden.
