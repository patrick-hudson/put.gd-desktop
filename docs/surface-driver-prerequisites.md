# Surface driver target prerequisites and cleanup

The surface driver runs on the QEMU host and uses the guest's native desktop.
`Target.start()` checks target tools once, before creating a private session or
launching a resident. A missing tool blocks startup with its exact name and #709,
rather than blocking every recipe independently.

For the Debian/Ubuntu/Mint guest in boat's `putgd-lab-cinnamon-v3` snapshot, install
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

Before resident launch the target inspects and snapshots clipboard MIME data in
private helper memory, with a 32 MiB limit. Payload bytes never enter SSH output,
files or receipts. An unreadable/oversized clipboard blocks startup. Receipts
contain only process identity, format names, review notes and cleanup status.

The keeper is event-driven. One owned SSH input pipe binds it to the runner;
EOF, parent death and termination request restoration. A shutdown-only kernel
deadline bounds a stalled Qt transfer. Run end also stops the exact PID through
a pidfd after validating UID, boot ID, start time and complete argv. Next start
recovers identified orphaned keepers, including old core helpers whose successful
cleanup removed their private root. Recovery happens after the current native
clipboard policy has passed. No process-name or command-pattern kill is used.
Clipboard restoration uses the shared `critical.py` cleanup section.

GNOME needs its existing X11 selection bridge for this unfocused clipboard
helper. The app and compositor witnesses still run natively on Wayland. Mutter
persists one preferred supported format after owner exit, with a 4 MiB text limit:

- Disposable `boat` and `guest` runs restore that supported format, normally the
  primary text type, and record a review note for omitted formats. A platform may
  regenerate equivalent text aliases; that does not promise general multi-MIME
  persistence.
- `native` runs block before ownership changes or resident launch if all offered
  application formats cannot be restored. Native X11 additionally requires a
  `CLIPBOARD_MANAGER`; nonempty native Wayland clipboards outside GNOME remain
  blocked until their persistence handoff is verified. Empty clipboards are safe.

No Shell clipboard extension is added. Full native GNOME MIME persistence is
tracked in [#879](https://github.com/patrick-hudson/put.gd-desktop/issues/879).
The helper always exits; it never remains as a clipboard owner after the run.
Successful cleanup removes the private root; a failed cleanup retains it and
reports an explicit error. See the [#860 evidence and limits](evidence/2026-10-30-surface-clipboard-860/README.md).

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
blocks further input; a late response cannot satisfy a later request. The timeout
budget includes channel startup. Refusals retain the outstanding request, elapsed
time, budget and SSH process state, including when a recipe alarm interrupted the
read. Start a new run after such a refusal; an alive SSH process does not prove
that its pending reply can safely be reused. Closing
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
because they are not X windows. A bounded, fresh shell-tree observation also
checks showing notification, alert and popup rectangles when the stage pick
returns only the full-screen stage. A cover refusal names that shell surface.
An unnamed X11 WM frame is resolved to its unique client before reporting it. A just-mapped target may settle (#750): if the
active window is 0, or another window of the same private process, the probe
rechecks the active window immediately before activating the target once, waits
up to 2.5 s and re-observes. A newly foreign active window refuses activation.
The X11 observation and activation are separate protocol requests; the recheck
closes the known settle gap, without claiming a server-side atomic transaction.

Wayland clients report AT-SPI extents relative to their own window. A profile
`compositor` witness places them on the desktop and answers focus and hit-tests:

- **GNOME** uses the lab-only `putgd-lab-witness@put.gd` Shell extension under
  `tools/lab/surface_driver/compositor/gnome-witness`. From the **QEMU host**,
  run `sh tools/lab/surface_driver/compositor/stage-gnome-witness.sh RUN [SOURCE]`,
  where `RUN` is the running disposable `lab.py` overlay directory and optional
  `SOURCE` is the local extension directory. The wrapper calls the same
  disk/process/SSH/QMP ownership checks as `lab.py`, twice, including immediately
  before writing. Real desktops, goldens and non-GNOME overlays are refused.
  Then use `python3 tools/lab/guest_reboot.py RUN` and verify version 4 is `ACTIVE`.
  It reports Mutter's frame rects, focus, stacking, native stage picks and bounded
  grab-begin/end history. Its only action is `Activate`, for an explicit
  `focus(raise_window=True)`. GNOME denies an untokenised `editor.raise`.
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

## Recording fixture: consent, retained media and the GTK sink

**ScreenCast consent (#791).** Each profile declares `recording.consent`:

- **`portal-dialog`.** Used by GNOME and KDE Wayland. The portal backend draws a dialog for each new ScreenCast `Start` until a stored restore token is accepted.
- **`none`.** Used by X11. Capture reads the X server, and any portal dialog request blocks.

Before `recording.prepare`, the fixture starts an owned `dbus-monitor` filter. It writes JSON records only: no options or restore tokens. The fixture clicks the backend's accept button once with the real pointer, and only when all of these hold:

- **One pending request, and it is ours.** Across the whole monitor log, not just this capture's part, exactly one impl ScreenCast/RemoteDesktop `Start` is pending. Its request handle comes from this capture's `putgd_record_*` `Start`, sent by a private-family process.
- **The declared backend, idle beforehand.** The impl call's destination is a declared backend executable. Before prepare, the process owning its bus name (`backend_bus`) showed no window.
- **One new window.** Exactly one window of that process is new since prepare, whatever its title, and it carries the declared title.
- **The prompt names our application.** The dialog's prompt names the application that xdg-desktop-portal resolved for *this* request: the impl `Start`'s `app_id`, through its desktop entry's `Name` (`putgd-record` → "put.gd screen recorder"). An app-less foreign request shows "An app wants to share your screen".
- **A settled button.** The accept button was sensitive, showing and unmoved across two observations at least 0.25 s apart. A newly drawn GTK4 dialog can ignore an early press.
- **The point is the dialog's.** The compositor witness picks that window at the button's centre.
- **Nothing changed.** A fresh read of the bus and the backend's windows, taken after that pick, still shows only this request and this window.

Nothing else ties a GNOME dialog to its request. put.gd passes no parent window, so the dialog has no transient-for, and the GTK4 accessibility tree holds no request handle. The prompt's app name is the only per-request text the backend draws.

The click is marked before dispatch and is never repeated. `drive()` refuses a second click itself. The step also refuses physical desktops: the dialog's pre-ticked "Remember this selection" stores a ScreenCast grant in the desktop's permission store, keyed by app id. That is lab state that only a disposable overlay may keep, and retiring the overlay discards it. The step needs `Response 0` for our handle. It saves `screencast-consent-before` and `-after` frames.

Anything the step cannot prove or drive raises `RecipeBlocked` (#791) with the reason; a ready timeout names the last consent state. The step never pre-grants through the permission store.

**Remaining window.** A foreign request sent after the post-pick re-read can still get its dialog drawn under the point in the milliseconds before the press. Live, a foreign `Start` fired by the guard's own `Pick` call was caught by the re-read. [Adversarial and live evidence](evidence/2026-10-30-recording-consent-817-validation/README.md).

**Monitors on channel loss.** Both run monitors (the portal consent filter and the notification observer) follow the run's persistent probe process, matched by PID and start time. They exit with it when the guest SSH channel drops. The private session root and resident can outlive a dropped channel; overlay retirement removes them with the disk.

On GNOME 50, xdg-desktop-portal-gnome is GTK4. GTK4 reports screen extents as (0,0) and exposes `sensitive` instead of `enabled`. The GNOME profiles therefore query window-relative extents. Those extents leave out the header bar padding (7,6 px measured). The profiles declare an 8 px `placement_tolerance`, and the accept button's centre must stay inside the button under that tolerance. KDE's native zkde_screencast path needs no consent. Its xdg-desktop-portal-kde fallback is declared but has not been observed in the lab yet. Profiles without a compositor witness block if a dialog appears. [Evidence](evidence/2026-10-30-recording-consent-791/README.md).

**Retained media (#796).** Before scope cleanup, the recording fixture copies the regular `.mkv`, `.mp4`, `.webm`, `.gif` and `.webp` files under its owned root to `<run out>/private-media/<recipe>/<scope>/`. These are the working and frozen clips and the exports that recipes verified there. Each copy:

- has mode 0600, and every directory level the fixture creates has mode 0700 (umask-proof);
- is verified against the guest's SHA-256 and listed in `manifest.json`.

Limits:

- **Size caps.** 32 MiB per file and 128 MiB per run. A file over a cap is hashed and listed as not retained.
- **Time budget.** Retention has its own 60 s budget. Files it cannot copy in time are listed as not retained. The later cleanup steps keep the time they had when retention started.
- **Scope.** Nothing outside the fixture root is read.
- **Location.** A run directory inside the checkout must be git-ignored. The media stays private run evidence: never repository content, receipts or coverage. That holds until secret-safe evidence export (#726) handles it.

Export the run directory, including `private-media/`, before deleting a sandbox.

**GTK sink (#784).** The visible pattern plays through GStreamer's `gtksink`. The fixture checks for it first and blocks (#784) when it is missing. In a disposable run overlay only (never a golden or snapshot), stage it with `sh profiles/provision-probes.sh --recording-overlay`. The script installs `gstreamer1.0-gtk3` and fails if the element is still missing.

## Input contracts

Plain `focus()` verifies focus with `raise_window=False`. Request
`focus(raise_window=True)` explicitly when raising is required; an unsupported
KDE raise still blocks. Accessibility rows keep the server's role spelling
(`button` remains `button`); selector matching normalizes synonymous roles such
as `button` and `push button`.

`double_click_accessible(query, button='left')` sends one guarded double-click.
`scroll_accessible(query, clicks)` sends integer wheel pulses, freshly guarding
each pulse. Both use the same foreground, occlusion, ownership and compositor
witness checks as `click_accessible` and call the existing bounded release
path. A refused or unconfirmed release withholds later input. Neither invokes an accessibility action or a menu service to synthesize
input. GNOME Shell actors must match the actual Shell PID, picked actor/subtree,
name/text, bounds and canonical ATK/AT-SPI role name; the two libraries have
different numeric role enums. A window behind a Shell surface cannot pass this
path.

The X11 native region selector can receive keyboard input only when its private
`putgd-capture` process, executable identity, focused native `Capture region`
accessible, override-redirect center hit and absence of a Shell cover jointly
prove the application's keyboard grab. A foreign override-redirect window does
not qualify.

Use `drag(start, end, require_resize=True)` when a GNOME recipe needs proof of
interactive resize. After press and the first motion, the driver requires a new
Mutter resize-begin event for the exact client window and PID, within the same
witness epoch and without lost history, before continuing. A move-begin, old
event or keyboard-resize event cannot prove it. Missing evidence blocks with a
resize-start reason and uses the existing release path. This proves initiation,
not an eventual size change or minimum-size behavior. Other compositors block
this option until they have an equivalent witness.
