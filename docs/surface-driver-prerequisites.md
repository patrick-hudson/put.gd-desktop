# Surface driver target prerequisites and cleanup

The surface driver runs on the QEMU host and uses the guest's native desktop.
`Target.start()` checks target tools once, before creating a private session or
launching a resident. A missing tool blocks startup with its exact name and #709,
rather than blocking every recipe independently.

For the Debian/Ubuntu/Mint guest in boat's `putgd-lab-cinnamon` snapshot, install
these normal distribution packages **inside a disposable guest**:

```sh
sudo -n apt-get update
sudo -n apt-get install -y --no-install-recommends \
  python3 python3-pyatspi python3-dbus python3-pyqt6 dbus-bin libglib2.0-bin libx11-6 \
  xdotool x11-utils x11-xserver-utils xclip wl-clipboard
```

The executable provisioning step is
`tools/lab/surface_driver/profiles/provision-probes.sh`. It checks the imports and
executables after installation. `xdotool`, `xprop` and `xrandr` provide X11 window,
stacking and head observations; `xclip` reads X11 clipboard data. Wayland profiles
use `wl-paste` and `wl-copy`. `python3-pyatspi` supplies the accessibility probe;
`python3-dbus` supplies the tray observer's D-Bus client. `dbus-bin` supplies `dbus-monitor`, and `libglib2.0-bin` supplies `gdbus`.
`python3-pyqt6` preserves clipboard MIME data in the native Qt clipboard backend.
The X11 hit-test also requires the distro X11 library.

The coordinator should provision a **new** Cinnamon golden and a **new** named
boat snapshot, for example `putgd-lab-cinnamon-surface-probes-20261030`. Rebuild the
guest embedded in `putgd-lab-cinnamon`; installing packages on its boat host alone
does not provision that guest. Apply the same step to future Debian/Ubuntu/Mint
surface-test guest images. Preserve existing named snapshots and immutable golden
disks, including disks referenced by overlays. Boot only fresh run overlays.

The driver host additionally needs `python3-pil` for PNG capture and comparison,
plus the lab host's existing QEMU/OpenSSH requirements. Never install a substitute
Qt or compiler toolchain. Target Qt remains dynamically linked to distro libraries.

Before resident launch the target copies the existing clipboard's MIME formats
into a private helper's memory, with a 32 MiB limit. Clipboard bytes never enter
SSH output, files or receipts. An unreadable/oversized clipboard blocks startup.
Closing the run restores those formats. On X11, the restoring helper serves the
selection until the clipboard manager or the next copy takes ownership, then
exits; exiting immediately would clear an otherwise restored selection. The
helper source contains no clipboard bytes. Its private session root is removed
after the resident exits and cleanup succeeds. Failed cleanup retains the root
and reports an explicit error.

Catalog input rectangles use Qt logical client coordinates. Resizing a window,
even without changing its aspect ratio, does not prove that controls scale with
it. Prefer live named controls; static coordinate fallback requires explicit
per-axis `start`, `center` or `end` anchors for each resized axis. Image comparison
independently normalizes both client crops to their common smaller size, including
device pixel ratio differences. A normalized image score never authorizes input.

Recipe alarms are deferred while input release completes through transports
with their own timeouts. A pending alarm is delivered after release and still
fails the recipe. Physical click/drag cleanup becomes eligible only after the
matching press passes endpoint and lock checks and dispatches successfully;
a refused press sends no release. Alarms are deferred until successful press
dispatch is recorded. Every physical release rechecks endpoint identity and
forces a fresh lock heartbeat before sending input. If authorization is lost
while a button is held, send nothing and raise `RecipeBlocked` naming the host
and button that may still be pressed, so a person can check the desktop; the
runner records the warning in its blocked receipt. Guest presses stay inside
the cleanup scope because a deadline or lost reply may occur after the owned
guest receives the press: always attempt release after the owned-endpoint check.

A run uses one owned SSH probe channel instead of starting a new interpreter and
SSH session for each observation. Every character and drag step still obtains
fresh geometry and foreground witnesses. Accessibility observations run in fresh bounded interpreters on that same
channel, so stale AT-SPI caches or a continuously busy event queue cannot
authorize input. X11 geometry and foreground witnesses remain in the persistent
process. A timed-out, interrupted or invalid channel
blocks further input; a late response cannot satisfy a later request. Closing
the Target closes its probe process before removing the private root.

Authorized interrupted drags release at the last dispatched position. Guest QMP release
sends only button-up, with no absolute-axis event. The RFB fallback reuses its
last dispatched coordinates because RFB pointer packets require coordinates;
it never moves to an unvisited drag endpoint during cleanup.

## Desktop observation, occlusion and Wayland placement

`ctx.start_notifications()` (the runner calls it after setup) starts one desktop
observer per run. `Target.close` stops it through `monitor_pid`, and the observer
dies with its parent.

- **Notification log.** `notifications.log` holds only the private family's own
  notification traffic: put.gd `Notify` calls whose sender PID belongs to the
  private family, the server's reply to each exact call, and the server's
  `ActionInvoked`/`NotificationClosed` for those IDs. Other applications'
  notifications, and other clients calling themselves "put.gd", never reach the
  disk. A notification row's `id` stays null until the server's reply arrives.
- **Runtime menus and out-of-tree widgets.** Qt lists only non-popup top-level
  windows as application children. A `QMenu::exec()` context or row menu, a
  viewport child widget, and Cinnamon's native region selector are therefore
  missing from an AT-SPI tree walk. The observer listens for
  `object:state-changed:showing` and `:focused`, and records the private family's
  showing popups and focused widget in `$XDG_RUNTIME_DIR/putgd-lab-popups.json`.
  Each `accessible()` call walks them live over the a11y bus as extra roots.

Notification input waits briefly for the banner to be drawn. It hovers the drawn
title, because shell buttons arm only after the pointer enters, and then clicks
the title or the action button with a fresh compositor stage pick. Cinnamon's
default banner lasts 4 s and only fades while hovered. Cinnamon and GNOME show
one banner at a time, so a resident banner left by an earlier run, or the
golden's updater banner, blocks later ones. Close those in a disposable guest
before a notification check.

On X11, pointer guards also ask the shell's AT-SPI stage pick whether a banner,
panel or shell menu is drawn at the point. The X hit-test cannot see these,
because they are not X windows. A just-mapped target may settle (#750): if the
active window is 0, or another window of the same private process, the probe
activates the target once, waits up to 2.5 s and re-observes. It never displaces
a foreign window.

Wayland clients report AT-SPI extents relative to their own window. A profile
`compositor` witness places them on the desktop and answers focus and hit-tests:

- **GNOME** uses the lab-only `putgd-lab-witness@put.gd` Shell extension under
  `tools/lab/surface_driver/compositor/gnome-witness`. Stage it **only in a
  disposable guest**: run `compositor/stage-gnome-witness.sh DIR` inside the
  guest, then `python3 tools/lab/guest_reboot.py RUN`, and enable it if it is not
  `ACTIVE`. It reports Mutter's frame rects, focus, stacking and stage pick. Its
  only action is `Activate`, for an explicit `focus(raise_window=True)`. GNOME
  denies an untokenised `editor.raise`. Never install it on a real desktop.
- **KDE** needs no install. Each observation loads a read-only KWin script over
  `org.kde.KWin /Scripting`; the script reports `workspace.stackingOrder` with
  exact `clientGeometry`.

Without a witness, or when placement is implausible, a row's `rect` is null and
`coordinate_error` gives the reason; window-relative numbers are never used. On
GNOME, client-side decorations are split symmetrically, within bounds.

Fresh accessibility walks are bounded (#805). They have to be: on GNOME 50 the
private Settings window keeps `putgd-ui` at a full core (#840), and some GTK
session services never answer AT-SPI at all.

- Every AT-SPI call (raw D-Bus) has a 3 s timeout. libatspi, where still
  used, gets the same bound instead of its longer default wait for recently
  registered applications, which in a fresh process is all of them.
- Each walk has a 7.5 s budget, counted from the fresh child's start; the
  persistent channel kills the child at 12 s.
- An unanswered call, or a walk over budget, gives RecipeBlocked naming the
  application, its pid and its time per node. It never returns a partial
  result, because a partial walk cannot prove absence or uniqueness.
- Wayland `geometry()` and `windows()` read only application children (window
  level). Control lookups skip text, descriptions and key bindings.
- Queries scoped by pid or `owned` never contact other applications. Unscoped
  queries ask every application's name concurrently. A silent application is
  skipped only if its executable cannot match the queried `application`.

Qt 6.10 exports no objectName attribute (#819). `objectName` then comes from
the last segment of the AT-SPI `AccessibleId` path (row field `accessibleId`).
Class names do not count. Neither do view items (list, table and tree items,
headers, page tabs), which repeat their view's id. A control that no catalog
exports, such as a Settings sidebar row, is declared in the profile as
`live_controls: {"settingsSection0": {"name": "General", "role": "list item"}}`.
`click()` then targets the live row with exactly that name and role, behind the
same guards.

Selector readback is per desktop (`selector_accessible` in the profile). On
Cinnamon it is the private family's `Capture region` pane, found through its
focus event. On GNOME it is the put.gd Shell extension's St actor in
gnome-shell's tree. The release's `putgd@put.gd` extension must be enabled in
the GNOME golden overlay.
