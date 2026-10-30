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
