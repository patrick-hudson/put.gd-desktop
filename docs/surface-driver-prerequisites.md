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

Cleanup that must run (input releases, the default-browser restore, fixture
finalizers, the runner's fixture cleanup) runs in a critical section
(`tools/lab/surface_driver/critical.py`, #815, #816, #762). Blocking signals as
a cleanup's first statement is not enough: CPython runs a pending handler as a
function starts, before its first line. So the operation around the cleanup
(a press-release gesture, a browser transaction, fixture setup, the recipe
loop) is armed: the run's SIGALRM, SIGTERM, SIGHUP and SIGINT handlers are
wrapped, and a handler that falls due in a section or as a registered cleanup
function starts defers its signal. After the section the deferred handlers run
and their exception is raised, unless the cleanup itself failed: that failure
stays the result and the deadline or termination is recorded as a review.
KeyboardInterrupt always propagates, and a termination still stops the run
after the current recipe's receipt. Recipes must not replace those handlers
inside a `default_browser` transaction: that is reviewed and the entry
handler is restored.

Privileged fixtures (tmpfs mounts, nft rules) stage a narrow sudo rule for
`mount`, `umount` and `nft` in the disposable overlay only, through the golden's
`apt-get` grant. Its removal is retried a bounded number of times and proven
with `sudo -l`; a rule left by a run that was killed, or whose removal met a
held apt lock, is removed by the next fixture staging on that overlay (#762).

Sections are short. A release has its own budget (3 s on a guest, 10 s on a
physical desktop) covering the endpoint check, the lock heartbeat and the QMP
connect and reply. A press is recorded before it is sent and cleared only after
a confirmed release, so a press without a confirmed release always withholds
later input for the run. Physical click/drag cleanup becomes eligible only after the
matching press passes endpoint and lock checks and dispatches successfully;
a refused press sends no release. Signals are deferred until successful press
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
process.

Probes read AT-SPI over raw D-Bus (`Accessible`, `Component`, `Text` and
`Action` calls on the a11y bus), never through libatspi objects (#814). libatspi
sends `org.a11y.atspi.Cache.GetItems` to every application it first meets, and
`getDesktop(0)` meets every registered application, so each fresh probe asked
the shell to serialise its whole ATK-bridge cache. On Mint 22.3 (Cinnamon 6.6.4,
at-spi2-core 2.52.0) that call crashed Cinnamon into fallback mode
(`impl_GetItems` → `g_hash_table_foreach` → `g_object_ref` on a stale cache
entry). Owned-control walks select applications by the bus daemon's PID before
any call reaches them, so the shell and unrelated applications are never
queried. Do not set the a11y bus `ScreenReaderEnabled` property in a lab guest:
it starts Orca, which answers slowly or not at all and adds an AT-SPI client the
run does not control. A timed-out, interrupted or invalid channel
blocks further input; a late response cannot satisfy a later request. Closing
the Target closes its probe process before removing the private root.

Authorized interrupted drags release at the last dispatched position. Guest QMP release
sends only button-up, with no absolute-axis event. The RFB fallback reuses its
last dispatched coordinates because RFB pointer packets require coordinates;
it never moves to an unvisited drag endpoint during cleanup.

## Desktop shell crash detection

A profile's `shell_health` names the desktop shell process and its crash-notice
window titles, for example Cinnamon's
`{"process": "cinnamon", "crash_windows": ["^Cinnamon just crashed"]}`. After
setup the runner records that process's PID and start time in the receipt's
`environment.shell_health`. Before every real input the guard checks that the
same process is still running. Between recipes it also looks for crash-notice
windows and for new `coredumpctl` entries of that process. If the shell crashed,
the in-flight recipe and every remaining recipe are `blocked` with issue 814 and
the reason. The remaining recipes get no input, frames or fixtures, and the
in-flight row keeps its own result as context only. A shell that is missing, or
that already shows a crash notice at run start, blocks the run.

When Cinnamon crashes, `cinnamon-launcher` leaves the session in fallback mode:
no `cinnamon` process, `metacity --replace`, and the "Cinnamon just crashed"
dialog. Session commands that find the desktop through the `cinnamon` process
fail afterwards, so resident cleanup cannot finish (#848). To prove the detector
in a disposable overlay, send `SIGSEGV` to `cinnamon` mid-run. Cinnamon absorbs
the first external `SIGSEGV` with a one-shot handler, so repeat it until the
process is gone. Only `mint-cinnamon-x11` declares `shell_health` so far (#849).

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
  missing from an AT-SPI tree walk. The observer registers
  `object:state-changed:showing` and `:focused` with the AT-SPI registry, reads
  those signals off the a11y bus, and records the private family's showing
  popups and focused widget in `$XDG_RUNTIME_DIR/putgd-lab-popups.json`. Each
  `accessible()` call walks them live over the a11y bus as extra roots.

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

Selector readback is per desktop (`selector_accessible` in the profile). On
Cinnamon it is the private family's `Capture region` pane, found through its
focus event. On GNOME it is the put.gd Shell extension's St actor in
gnome-shell's tree. The release's `putgd@put.gd` extension must be enabled in
the GNOME golden overlay.
