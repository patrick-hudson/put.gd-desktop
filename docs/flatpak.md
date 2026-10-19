# Flatpak feasibility spike (#14)

Status: research, 2026-10-18. Nothing here changes the DEB, RPM or AppImage, and
nothing here is a release input. The experimental manifest is
[`packaging/flatpak/gd.put.Putgd.yml`](../packaging/flatpak/gd.put.Putgd.yml).
[Issue #14](https://github.com/patrick-hudson/put.gd-desktop/issues/14) plans a
separately installed host "capture companion". This spike asked whether a
Flatpak can ship without any new host helper, recording included, and what each
desktop loses if it does. Every claim below comes from a run (on a physical
desktop, a lab guest or a nested GNOME Shell, as labeled) or from the cited
upstream source; gates that were not run are listed at the end.

## Answer

No new host binary is needed on any desktop. What each desktop keeps:

- **Cinnamon and Plasma on X11**: everything, through the X11 socket, except
  Cinnamon region capture. Grabbed shortcuts, window and desktop captures, the
  X11 clipboard and `ximagesrc` recording work in the sandbox; the manifest only
  adds the `ximagesrc` plugin the runtime lacks. On Cinnamon the region capture
  hands Muffin a file path inside the sandbox and times out; moving that file to
  a path the host and the sandbox share fixes it.
- **GNOME 50**: everything, through the existing `putgd@put.gd` Shell extension,
  which every format already needs. Measured in a nested GNOME Shell: captures,
  Shell shortcuts and the unfocused clipboard work from the sandbox, but put.gd's
  own window is not refused, and the bridge currently needs talk access to all of
  `org.gnome.Shell`. Two extension changes fix both (see
  [GNOME](#gnome-50-wayland)).
- **Plasma Wayland**: no direct screenshots. KWin authorizes ScreenShot2 by the
  caller's executable, and a sandboxed caller always looks like
  `xdg-dbus-proxy` to KWin. Clipboard, shortcuts, the tray (with a name
  permission), the region overlay and recording through the ScreenCast portal
  all work. Screenshots have to come from
  the Screenshot portal (full screen only, a one-time permission dialog on
  Plasma 6.6, and a stray file in `~/Pictures` per capture) or from a single
  frame of a screen cast. Silent screen casts need a desktop entry on the host
  that the Flatpak cannot install for itself on a patched Flatpak; the proposal
  makes the two a choice in Settings → Advanced, with put.gd offering to write
  that entry through a save dialog the user confirms.

So the companion in #14 can be dropped. Plasma Wayland is the one desktop where
the Flatpak behaves differently from the native packages, and the proposal at
the end makes that difference explicit instead of hiding it behind a helper.

## What was built

| Item | Identity |
| --- | --- |
| Source | `claude/flatpak-spike` at `4e26ae0b881c` on top of nightly `e13fee9`; the binaries report `0.1.1-dev.370+g4e26ae0b881c`, `dirty: false` |
| Builder | Fedora 44 (`.70`), flatpak 1.17.6, flatpak-builder 1.4.12 |
| Runtime | `org.kde.Platform//6.11` `fe192771c099…`, SDK `305b8ec8d506…` (Qt 6.11.2, GStreamer 1.26.11, FFmpeg 7.1.3 / libavcodec 61.19.101, libsecret 0.21.7, PipeWire 1.4.9), based on freedesktop 25.08 |
| Extensions pulled in by the runtime | `codecs-extra//25.08-extra` `8d29ce3ba01d…`, `VAAPI.Intel//25.08` `b8647e186bdb…`, `GL.default//25.08` `fe659a591fe8…` |
| Modules built | official AWS SDK 1.11.907 (commit and 15 submodules checked by `tools/verify_dependencies.py` inside the build), LayerShellQt v6.6.7, QtKeychain 0.15.0, the `ximagesrc` plugin from gst-plugins-good 1.26.11 |
| App | `gd.put.Putgd` commit `b4f7fe642355…`, 22.7 MB installed; bundle `c0014af0804a…`, 5.1 MB |

Qt stays dynamically linked from the runtime. The KDE 6.11 runtime has no
LayerShellQt, QtKeychain or `ximagesrc`, so the manifest builds them. Proton is
not built (`PUTGD_BUNDLE_PROTON=OFF`); bundling its runtime is a separate task.

`flatpak-builder` copies the checkout (`type: dir`) and configures outside it,
because a build directory inside the copy made the Git status dirty. A plain
`type: git` source defaults to `master`, which this repository doesn't have.

The measurements used the build above. The committed manifest differs in two
places, both from the results: it grants the tray name
`org.kde.StatusNotifierItem.*` (measured as an override), and its exported entry
no longer carries the `X-KDE-*` keys (see [Screen casts](#screen-casts-and-recording)).
That manifest (`b67bfae`) was built again on Kubuntu `.72` with flatpak-builder
1.4.8: build succeeded, exported entry without `X-KDE-*` keys, metadata with the
tray name, bundle `5e2af00eb542…`. It was not installed or re-measured. Later
commits on the branch change only comments in the manifest.

## Where it ran

| Desktop | Host | Session | Portals and Flatpak |
| --- | --- | --- | --- |
| Plasma 6.3.6 Wayland | Debian 13 `.69`, physical | KWin 6.3.6, Qt 6.8.2, PipeWire 1.4.2 | flatpak 1.16.6-1~deb13u3, xdg-desktop-portal 1.20.3, portal-kde 6.3.5 |
| Plasma 6.6.6 Wayland | Kubuntu 26.04.1 guest `fpk-kw-0b0a5599` on `.69` (golden `kde-provisioned-20261016`, `49fcacff…`), KVM | KWin 6.6.6 | flatpak 1.16.6-1, xdg-desktop-portal 1.21.1, portal-kde 6.6.6 |
| Plasma 6.6 X11 | Kubuntu 26.04.1 `.72`, physical | kwin_x11 6.6.4, Qt 6.10.2 | flatpak 1.16.6-1, xdg-desktop-portal 1.21.1, portal-kde 6.6.6 |
| GNOME 50 Wayland | Fedora 44 `.70`, physical session; bridge in a nested `gnome-shell --devkit` on the same desktop | gnome-shell / Mutter 50.0, PipeWire 1.6.9; nested: gnome-shell 50.0 with Mutter 50.6 (pulled in by `mutter-devkit`, reverted afterwards) | flatpak 1.17.6, xdg-desktop-portal 1.21.1, portal-gnome 50.0 |
| Cinnamon 6.6.4 X11 | Mint 22.3 guest `fpk-cin-4309db74` on `.69` (golden `cinnamon-x11`, `4a21566d…`), KVM | Muffin, Qt from the runtime | flatpak 1.14.6, xdg-desktop-portal 1.20.0, portal-xapp 1.1.3, gnome-keyring 46.1 |

The resident ran as `flatpak run --user gd.put.Putgd --background` in each
session, with its own data under `~/.var/app/gd.put.Putgd`. Every CLI call in
the evidence came from the host through `flatpak run --command=putgd-cli`.
Screenshots, clips, captures and logs are kept privately under
`work/verification/2026-10-18-flatpak-spike/` on the workstation.

## Per-desktop result

`works`: the same behavior as the native package. `degrades`: works with a
visible difference. `missing`: not available in the sandbox without a change.

| Check | Cinnamon X11 | Plasma X11 | GNOME 50 | Plasma 6.3 Wayland | Plasma 6.6 Wayland |
| --- | --- | --- | --- | --- | --- |
| Region capture | missing: Muffin path times out | works (500×300 exact) | not run | missing | missing |
| Window capture | works (900×592 with frame) | works (1400×828 with frame) | works (nested) | missing | missing |
| Desktop capture | works (1280×800) | works (1920×1080) | works (nested, 1280×800) | missing | missing |
| Own-window refusal | works | works | missing: captured put.gd's own window | n/a | n/a |
| Shortcuts | works (XGrabKey) | works (XGrabKey) | works (Shell binding, nested) | works (KGlobalAccel) | works (KGlobalAccel) |
| Clipboard import, unfocused | works | works | works (nested) | works (wlr-data-control v2) | works (ext-data-control v1) |
| Copy result, unfocused | works (GTK reader) | works (owner verified) | works (`copied`; no independent paste) | works (native paste) | works (Klipper read it) |
| Tray | degrades: needs a name permission | works | degrades: needs a name permission | degrades: needs a name permission | degrades: same |
| Notifications | works | not checked | not checked | works | not checked |
| Recording, dialog-free | works (`ximagesrc`, x264, motion 1.213) | works (`ximagesrc`, VA H.264, motion 0.372) | works (Mutter, VA H.264, motion 0.264) | needs host entry (motion 0.290) | needs host entry on patched Flatpak (motion 0.094) |
| Recording, portal | n/a | n/a | works, one Share click, then restore token | works, picker once, then restore token | not run |
| Screenshot portal, `interactive=false` | n/a | n/a | refused while unfocused (not needed) | silent | one-time dialog, then silent |
| Credentials (Secret portal) | not run (guest has no login keyring) | not run | reaches gnome-keyring (unlock prompt, cancelled) | works, no prompt | not run |
| Start at login | missing (Background portal works) | missing (portal not run here) | missing (Background portal works) | missing (Background portal works) | missing (Background portal works) |
| Local copies in Documents/put.gd | works | works | works | works | works |
| CLI from the host | works | works | works | works | works |
| S3 request from the sandbox | not run | not run | not run | works (403 for a fake key) | not run |

## Plasma Wayland

### Screenshots

KWin checks ScreenShot2 callers in `checkPermissions()`
([`screenshotdbusinterface2.cpp`](https://invent.kde.org/plasma/kwin/-/blob/Plasma/6.6/src/plugins/screenshot/screenshotdbusinterface2.cpp)):
it asks the bus for the caller's PID, reads `/proc/PID/exe`, and looks for an
application entry whose `Exec` has the same canonical path
(`fetchProcessServiceField` in `src/utils/serviceutils.h`, identical in 6.3 and 6.6).
Measured from a sandboxed client on Fedora: `GetConnectionUnixProcessID`
returns the PID of `xdg-dbus-proxy` (`/usr/bin/xdg-dbus-proxy`, root
containing `.flatpak-info` with `name=gd.put.Putgd`), and the sandboxed
process's own `/proc/PID/exe` reads as its in-sandbox path (`/app/bin/…`),
which doesn't exist on the host. No desktop entry can match either: an entry
naming `xdg-dbus-proxy` would authorize every Flatpak. With the
`org.kde.KWin.ScreenShot2` name temporarily allowed, desktop and region captures
fail with `capture_permission` (KWin's `NoAuthorized`) on 6.3.6 and on 6.6.6,
even where the exported entry still carries `X-KDE-DBUS-Restricted-Interfaces`.
Without that name, the call never leaves the sandbox (`KWin ScreenShot2 service
is unavailable`). Window capture also reads window details from `org.kde.KWin`,
which a sandbox should not be allowed to call (it exposes KWin scripting).

`KWinCapture::installedEntryMatches()` reports a match inside the sandbox
because it finds `/app/share/applications/putgd.desktop` next to
`/app/bin/putgd`; that diagnostic needs a sandbox case.

Two silent or nearly silent alternatives exist:

1. **Screenshot portal with `interactive: false`.** On Plasma 6.3 (portal-kde
   6.3.5) it returns a full 1920×1080 PNG with no dialog. On Plasma 6.6.6 the
   first request shows "Allow put.gd to Take Screenshots?" (the frontend asks
   whenever the backend's Screenshot interface is version 2 or later,
   [`screenshot.c`](https://github.com/flatpak/xdg-desktop-portal/blob/1.20.3/src/screenshot.c));
   after Allow, later requests are silent. It always captures the whole screen,
   and portal-kde saves every capture as `~/Pictures/Screenshot_*.png` and hands
   it over through the document portal. The sandbox gets read access only, so it
   cannot delete that file. Region capture would be put.gd's own selector over
   the returned frame; window capture would need the window's geometry, which
   the sandbox cannot get without `org.kde.KWin` or the restricted
   `org_kde_plasma_window_management` protocol.
2. **One frame of a KWin screen cast.** `zkde_screencast_unstable_v1` streams an
   output, a rectangle or a window without a dialog, and the recording helper
   already uses it. KWin is a restricted global; for a client in a Flatpak
   security context KWin looks up the entry named after the app ID and reads
   `X-KDE-Wayland-Interfaces` from it (`fetchRequestedInterfacesForDesktopId`
   in `src/wayland_server.cpp`, present since KWin 6.1). See the next section for
   who may write that entry.

### Screen casts and recording

What KWin offered a sandboxed client, listed with a dependency-free registry
dump inside and outside the sandbox:

| | Plasma 6.3.6 (Debian, flatpak 1.16.6-1~deb13u3) | Plasma 6.6.6 (Kubuntu, flatpak 1.16.6-1) |
| --- | --- | --- |
| Security context | yes (`wp_security_context_manager_v1` hidden from the sandbox) | yes |
| Data control | `zwlr_data_control_manager_v1` v2 | `ext_data_control_manager_v1` v1 |
| Layer shell | `zwlr_layer_shell_v1` v5 | (not checked) |
| `zkde_screencast_unstable_v1` from the app's own exported entry | no: Flatpak strips the key | yes, v5 |
| `zkde_screencast_unstable_v1` with a host entry `~/.local/share/applications/gd.put.Putgd.desktop` | yes, v5 | (not needed) |

The difference is Flatpak, not KWin. Flatpak 1.18.4 fixed
[GHSA-v64f-hrwr-j4vh](https://github.com/flatpak/flatpak/security/advisories/GHSA-v64f-hrwr-j4vh)
(CVE-2026-97027) by exporting only allowlisted desktop-entry keys; the list in
`common/flatpak-dir.c` names `X-KDE-DBus-Restricted-Interfaces` as deliberately
dropped and doesn't include `X-KDE-Wayland-Interfaces`. Debian backported it in
`1.16.6-1~deb13u3` (2026-09-28). Kubuntu 26.04 still had the unfiltered
`1.16.6-1` after `apt-get update` on 2026-10-18, and Fedora's 1.17.6 still
exported both keys. The loophole closes when those distributions update, so the
design must not rely on it.

A host entry works. With a regular file named `gd.put.Putgd.desktop` in
`~/.local/share/applications` containing
`X-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1`, KWin 6.3.6 offered the
protocol to the sandbox and the helper chose `kde-screencast`. Only the user (or
an administrator) can create it: the Flatpak cannot write outside its sandbox.
It is a file, not a helper, and it grants exactly one protocol to exactly one
app ID.

Measured recordings from the sandbox (monitor target, moving test pattern,
`putgd-record --motion` on the saved copy; a static region scores about 0.01,
and the static Kubuntu X11 control below scored 0.000):

| Desktop | Path | Encoder | Clip | Motion |
| --- | --- | --- | --- | --- |
| Plasma 6.3.6 | `kde-screencast` with the host entry | `vah264lpenc` | 1920×1080, 8.57 s | 0.290 |
| Plasma 6.3.6 | portal, picker (one click on the monitor) | `vah264lpenc` | 1920×1080, 8.57 s | 0.269 |
| Plasma 6.3.6 | portal, restore token, no picker, ready in 2 s | `vah264lpenc` | 1920×1080, 8.53 s | 0.209 |
| Plasma 6.6.6 guest | `kde-screencast` from the app's own entry | `x264enc` (no VA in the guest) | 1280×800, 8.73 s | 0.094 (small pattern window) |

So a sandboxed client can consume KWin's PipeWire node with the manifest's
`--filesystem=xdg-run/pipewire-0`. The restore token lives in the app's own data
(`~/.var/app/gd.put.Putgd/data/putgd/putgd-record/`).

Defect found on the way: `putgd-record` segfaulted in `libwayland-client` when a
`kde-screencast` recording was shut down (Debian 20:24:13, kernel log; Kubuntu
guest 20:41:33, DrKonqi), after the clip was saved; portal runs did not crash.
The recording session traced it to a use-after-free in `KdeScreencast::close()`
(the stream's `close` request is its destructor, and the proxy was then
destroyed again), present in native builds since PR #68, and fixed it in
PR #77 (`daf0792`), verified in the Kubuntu guest.

### Other Plasma Wayland facts

- **Clipboard.** Import and result copy worked without focus on both versions.
  On Debian a host `wl-copy` source was imported, saved to
  `~/Documents/put.gd/text/` on the host, and pasted back into GNOME Text Editor
  (native) while put.gd owned the clipboard. On 6.6 Klipper set the source and
  read the result.
- **Shortcuts.** KGlobalAccel accepts any caller, so `--talk-name=org.kde.kglobalaccel`
  is enough: Ctrl+Shift+2 pressed over VNC reached the sandboxed resident.
- **Region selector.** The layer-shell overlay appears inside the sandbox; the
  capture after it then fails at ScreenShot2.
- **Tray.** The custom tray item registers
  `org.kde.StatusNotifierItem.putgd_<uuid>`, which the D-Bus proxy refuses, so
  no icon appears. With `--own-name=org.kde.StatusNotifierItem.*` the icon shows
  on 6.3 and 6.6. The Qt tray path used on X11 registers under its unique bus
  name and needs nothing.
- **Credentials.** QtKeychain uses libsecret, which inside a sandbox stores an
  encrypted keyring in `~/.var/app/gd.put.Putgd/data/keyrings/default.keyring`
  and keeps its key with the Secret portal (KWallet on Plasma). Storing a
  visibly fake key in Destinations worked without a prompt (the wallet was open);
  **Test connection** then reached AWS and got 403, which also proves the S3
  module loads from `/app/lib/putgd` and HTTPS works.
- **Notifications** through `org.freedesktop.Notifications` appear.

## GNOME 50 Wayland

Fedora's GNOME session had no put.gd extension installed when this ran (the
session's baseline, after the other recording sessions had removed theirs), and
GNOME 50 loads a locally installed extension only at login
(`ReloadExtension` "is deprecated and does not work"; only extensions.gnome.org
installs load at runtime). Instead of logging Patrick's session out twice, the
bridge was tested in a nested `gnome-shell --devkit` (a "Mutter Development
Kit" window on the same desktop) with private XDG config, data and runtime
directories and its own session bus, loading `putgd@put.gd` from the spike
checkout. The physical session itself was used for everything that doesn't need
the extension.

In the physical session, without the extension:

- The resident owns `org.putgd.App.Gnome50` (the bus lists `xdg-dbus-proxy` as
  the owner process) and reports every capture as `helper_unavailable`, as a
  native install without the extension does.
- The tray icon appears through the AppIndicator extension with the
  `org.kde.StatusNotifierItem.*` name allowed.
- **Recording through Mutter's screen cast** (`--talk-name=org.gnome.Mutter.ScreenCast`)
  works without a dialog: 1920×1080, 8.57 s, `vah264lpenc`, motion 0.264. The
  Flatpak's Intel VA extension encodes H.264 on this Skylake GPU; Fedora's own
  free driver cannot, so the native RPM falls back to OpenH264.
- With Mutter's name hidden by an override, the **ScreenCast portal** shows
  GNOME's "Share Screen" dialog with the monitor preselected and "Remember this
  selection" ticked; one Share click recorded (motion 0.292), and the next
  recording was ready in 1 s without a dialog (motion 0.284).
- The **Screenshot portal** with `interactive: false` failed (response 2): the
  frontend tried to show its one-time permission dialog and GNOME refused with
  "Only the focused app is allowed to show a system access dialog". A background
  resident cannot obtain that permission; GNOME capture goes through the
  extension anyway.
- The **Background portal** wrote and removed the autostart entry without a
  dialog. Fedora's permission store already had `background: yes` for the app,
  written by the portal's background-app monitor while windowless probes ran.
- **Credentials**: storing a fake key made GNOME show "Authentication required:
  the login keyring did not get unlocked when you logged into your computer".
  The sandbox's request reached gnome-keyring through the Secret portal; the
  keyring is locked because this session logs in automatically. The prompt was
  cancelled without typing anything.

In the nested Shell, with the extension loaded:

- The resident found the extension through `org.gnome.Shell`: protocol 1,
  Shell 50, all three captures available, clipboard through Mutter's selection,
  the three shortcuts bound by Shell.
- Desktop capture from the CLI and from Ctrl+Shift+3 pressed over VNC both
  completed (1280×800, the nested output).
- Clipboard: a text source set inside the nested session was imported without
  focus, saved, and copied back (`copied`; read back from the saved file, not by
  an independent paste).
- **Own-window refusal is broken.** With put.gd's own window focused, window
  capture completed and returned put.gd's window (1280×768) instead of
  `capture_target`. This is change 2 below: the extension compares Mutter's host
  PID with the proxy's PID and with a namespace PID from `SetUiProcess`.
- Running the bridge needed `--talk-name=org.gnome.Shell`, which is change 1.

## Cinnamon and Plasma on X11

On Kubuntu's Plasma X11 session (the tray-name override was set but is not used
by the Qt tray there):

- The X11 adapter is chosen. Grabbed shortcuts fired: window (1400×828 including
  the frame), region (exactly the dragged 500×300) and desktop (1920×1080).
- Window capture with put.gd's own window focused was refused
  (`capture_target`). Inside the sandbox `_NET_WM_PID` holds the namespace PID
  (14), the same PID space as the UI process, so the check still matches. The
  edge case: another Flatpak's window with the same namespace PID would also be
  refused.
- Clipboard import of a Klipper-set text worked; a native Tk client read the
  result from CLIPBOARD, and the owner window changed from put.gd's to Klipper's
  the moment the resident quit.
- Recording through `ximagesrc` (the plugin the manifest builds) with
  `vah264lpenc`: a region over the moving pattern scored 0.372; the same region
  over a static window scored 0.000.
- The tray uses Qt's own StatusNotifierItem, registered under the unique name.

On Mint 22.3 Cinnamon (guest), with the `org.kde.StatusNotifierItem.*` name
allowed for the tray:

- The Cinnamon core route is chosen (`cinnamon-core-x11`, capture helper,
  native X11 clipboard ownership); the three shortcuts are grabbed. Window
  (900×592 with frame) and desktop (1280×800) captures work, and window capture
  with put.gd's window focused is refused.
- **Region capture fails.** The capture helper runs `X11SourceAdapter`, which on
  Muffin asks `org.Cinnamon.ScreenshotArea` to write the post-selection pixels
  into a `QTemporaryDir` in its own `/tmp` (`CinnamonRegionCapture`). Cinnamon
  runs on the host and cannot write into the sandbox's `/tmp`, so the helper
  waits out its 10 s deadline: `capture_timeout`, notification "The capture
  helper could not complete source acquisition".
- Clipboard import from a host GTK source worked; a GTK reader got the result
  back while put.gd owned CLIPBOARD.
- Recording through `ximagesrc` with `x264enc` (no VA in the guest): 880×540,
  motion 1.213.
- The tray item shows in the Cinnamon panel through `xapp-sn-watcher`; without
  the name permission nothing registers.
- portal-xapp provides Background: autostart was written and removed without a
  dialog, with the same `Exec` as on Plasma.
- Credentials: not established. This guest has no login keyring (only the
  `session` collection), and storing a fake key failed with "Unlock the OS
  secret store explicitly or select session credentials". A Mint profile with an
  unlocked login keyring is needed to check the gnome-keyring Secret portal.

TigerVNC's `x0vncserver` releases Shift to type the `2` keysym, so Ctrl+Shift+2
over VNC arrives as Ctrl+2. Sending the shifted keysyms (`!`, `@`, `#`) fires
the grabs. This is a test-input detail, not an app behavior.

## Every desktop

- **Start at login.** `LoginStartup` writes `putgd.desktop` with
  `Exec="/app/bin/putgd-launcher"` into the sandbox's own
  `~/.var/app/gd.put.Putgd/config/autostart/`, which the host session never
  reads, so the setting silently does nothing. The Background portal
  (`RequestBackground`, `autostart: true`,
  `commandline: ["putgd-launcher", "--background"]`) wrote a correct host entry,
  `Exec=flatpak run --command=putgd-launcher gd.put.Putgd --background`, without
  a dialog on Plasma 6.3, Plasma 6.6, GNOME 50 and Cinnamon, and
  `autostart: false` removed it. No login was done to see it start.
- **CLI and MCP.** The resident's socket stays at
  `$XDG_RUNTIME_DIR/putgd/session.sock`; `--filesystem=xdg-run/putgd:create`
  makes it the same path inside and outside. The server does no peer-credential
  checks, so a CLI in another sandbox instance or on the host connects. The MCP
  helper resolves `putgd-cli` on `PATH` or takes `--cli PATH`; with a Flatpak
  it needs a one-line wrapper that runs `flatpak run --command=putgd-cli gd.put.Putgd "$@"`.
- **Local copies.** `--filesystem=xdg-documents/put.gd:create` lets the default
  archive land in the host's `~/Documents/put.gd`. Without it the host's
  `~/Documents` is not visible in the sandbox; what the archive does then was
  not tested. A user
  who picks another folder needs the FileChooser portal path, not done here.
  The `:create` flag creates the folder on every launch, even if local copies
  are off.
- **Identity.** The UI and helper windows use their own desktop file names
  (`putgd-ui`, `putgd-record`), which Flatpak doesn't export, so their title
  bars show a generic icon. They should use the app ID when sandboxed.
- **Codecs.** In the sandbox the helper found `vah264lpenc`/`vah264enc` (the
  Intel VA driver extension) and `x264enc`, plus FFmpeg `libx264`, `libx265`,
  `h264_vaapi` and `libvpx-vp9` from codecs-extra. On Fedora that is more than
  the native RPM gets (stock Fedora has only OpenH264). It is also the order
  Patrick chose for the AppImage on 2026-10-19 (host VA-API/NVENC first, then
  x264/x265), with one difference: the Flatpak bundle contains no codec. x264,
  x265 and the full FFmpeg come from the separately distributed
  `org.freedesktop.Platform.codecs-extra` extension and VA drivers from the GL
  and VAAPI extensions, which Flatpak installed automatically with the runtime.
  NVENC would need Flatpak's NVIDIA GL extension matching the host driver; no
  builder has an NVIDIA GPU, so it was not tested.
- **Host faults.** `flatpak run` failed with "bwrap: Can't find source path
  /run/user/1000/doc/by-app/gd.put.Putgd" on Kubuntu `.72` (the document
  portal's FUSE mount had been gone since boot) and on Fedora `.70` (gone after a
  GDM re-login, while the service still showed as running). Restarting the user
  service `xdg-document-portal` fixed both. A Flatpak release note should say how
  to recover.
- **Leftover on Debian.** The Secret portal keeps a per-app key in KWallet's
  `xdg-desktop-portal` folder (entry `gd.put.Putgd`). Removing it needs a new
  KWallet access grant, so it was left; it is unused now that the app and its
  keyring file are uninstalled.
- **Session restart.** After installing Flatpak on a running Plasma session, its
  `XDG_DATA_DIRS` lacks Flatpak's export directory until the next login, so KWin
  and the portals don't see the app's entry yet.

## Permissions in the manifest

| Permission | Needed for | Notes |
| --- | --- | --- |
| `--socket=wayland`, `--socket=fallback-x11`, `--share=ipc` | display | X11 only where there is no Wayland socket |
| `--device=dri` | VA-API encode, GL | |
| `--share=network` | uploads | |
| `--socket=pulseaudio` | system audio in recordings | |
| `--filesystem=xdg-run/pipewire-0` | KWin and Mutter screen casts without the portal | the portal path passes its own descriptor |
| `--filesystem=xdg-run/putgd:create` | host CLI/MCP | |
| `--filesystem=xdg-documents/put.gd:create` | default local archive | |
| `--talk-name=org.kde.StatusNotifierWatcher` | tray | plus the name below |
| `--own-name=org.kde.StatusNotifierItem.*` | the custom tray item | measured as an override; drop after change 3 |
| `--talk-name=org.kde.kglobalaccel`, `org.kde.keyboard` | KDE shortcuts | |
| `--own-name=org.putgd.App.Gnome50`, `--talk-name=org.gnome.Shell` | GNOME extension bridge | `org.gnome.Shell` must go, see GNOME |
| `--talk-name=org.gnome.Mutter.ScreenCast` | GNOME recording without the picker | |
| `--talk-name=org.Cinnamon` | Muffin screenshots | see Cinnamon |
| `--talk-name=org.freedesktop.Notifications` | notifications | |

## Changes the code needs (not made here)

None of these are in this branch: two parallel sessions own `src/recording`,
the KDE region helper and the GNOME extension, and this spike only touches
`packaging/flatpak/` and this document. Sandbox detection is `FLATPAK_ID` (or
`/.flatpak-info`).

1. **GNOME extension: its own connection and bus name.** The extension exports
   `org.putgd.Gnome50` on Shell's own connection, and the resident finds it by
   resolving `org.gnome.Shell`. xdg-dbus-proxy applies a talk rule to the
   owner's unique name, so any name owned by Shell's connection lets the sandbox
   call everything Shell exports on it, including `org.gnome.Shell.Extensions`
   (`EnableExtension`, `DisableExtension` and `UninstallExtension` have no sender
   check in gnome-50's `js/ui/shellDBus.js`). The extension should open a
   separate session-bus connection, own `org.putgd.Gnome50` on it and export the
   bridge there; `GnomeAdapter` resolves that name (falling back to
   `org.gnome.Shell` for an older extension), and the manifest replaces
   `--talk-name=org.gnome.Shell` with `--talk-name=org.putgd.Gnome50`.
2. **GNOME extension: recognize the sandboxed client's windows by app ID.**
   For a sandboxed caller, `GetConnectionUnixProcessID` returns the
   `xdg-dbus-proxy` PID (measured), and `SetUiProcess(u)` carries a PID from the
   sandbox's PID namespace; Mutter's `window.get_pid()` is the host PID from the
   Wayland socket. None of the three match, so own-window refusal cannot work by
   PID. Mutter already labels sandboxed windows: `get_sandboxed_app_id()` reads
   `/proc/PID/root/.flatpak-info` (`meta_window_update_flatpak_id` in gnome-50
   `src/core/window.c`). The extension can read the caller's app ID the same way
   from the proxy PID (measured: its root has `.flatpak-info` with
   `name=gd.put.Putgd`) and treat every window with that sandboxed app ID as the
   client's own. No PID translation is needed; native callers keep the PID check.
   The tray tooltip companion compares the client's and the tray item's owner
   PIDs; both are the same proxy PID, so it keeps working.
3. **Tray item name.** `NativeTray` owns `org.kde.StatusNotifierItem.putgd_<uuid>`,
   which a sandbox may not own. Either register by object path only, so the
   watcher uses the unique name (Qt's tray does that and KDE's watcher showed it
   on X11), or own a name under the app ID. Until then the manifest needs
   `--own-name=org.kde.StatusNotifierItem.*`.
4. **Start at login.** In a sandbox `LoginStartup` should call the Background
   portal (`RequestBackground` with `autostart`, `commandline:
   ["putgd-launcher", "--background"]` and a `reason`) instead of writing a file,
   and send `autostart: false` to turn it off. Measured on Plasma 6.3, Plasma 6.6
   and Cinnamon: no dialog, correct host entry.
5. **Cinnamon region capture.** `CinnamonRegionCapture` must create its request
   directory where Muffin and the sandbox see the same path:
   `$XDG_RUNTIME_DIR/app/$FLATPAK_ID/` is bind-mounted at the same path in the
   sandbox. Keep the 0700 directory, pre-created 0600 files and the inotify
   close-write wait. (Falling back to post-selection root capture would also
   work, but it brings back the compositor-settle problem that path exists for.)
6. **Plasma Wayland capture: a choice in Settings → Advanced.** In a sandbox on
   Plasma Wayland, Advanced gets a "Plasma screen capture" setting with two modes;
   it is hidden on every other desktop and outside a sandbox.
   - **Desktop portal** (default, nothing on the host). Region capture keeps the
     live-desktop selector, then makes a Screenshot portal call
     (`interactive: false`) after the overlay is gone and crops the frame to the
     selection; desktop capture uses the same call uncropped. Costs: a one-time
     "Allow put.gd to take screenshots?" on Plasma 6.6 (none on 6.3), a copy of
     each capture in `~/Pictures` that portal-kde writes and the sandbox cannot
     delete, and active-window capture either through the ScreenCast portal's
     window picker every time or reported unavailable (decide when implementing).
   - **KWin screen cast** (silent). Region (`stream_region`), desktop (each
     output) and active window (`stream_window`) are taken as one frame of a KWin
     screen cast: no dialog and no `~/Pictures` copies. KWin offers
     `zkde_screencast_unstable_v1` to the sandbox only when the user has a desktop
     entry named after the app ID that lists it. Choosing this mode offers to
     write that entry: put.gd shows its exact content and saves it through the
     FileChooser portal as `~/.local/share/applications/gd.put.Putgd.desktop`
     (keeping the exported `Exec`, so the menu entry still launches), then checks
     that KWin now offers the protocol. Active-window capture also needs the
     window's identity, from `org_kde_plasma_window_management` granted by the
     same entry; that is not measured yet, and it lets the app see every window's
     title, which the setting's description must say. If the entry is missing,
     removed or not yet indexed, captures fall back to the portal mode with a
     visible reason instead of failing. Choosing the portal mode again stops
     using the protocol; the sandbox cannot delete the entry, so Settings shows
     its path for the user to remove.

   The setting is an ordinary configuration key (for example
   `preferences.plasmaSandboxCapture`: `portal` or `kwin-screencast`), validated
   by `SettingsService`, reported in `capabilities` together with the active mode
   and whether the entry was detected, and controllable through `settings.patch`,
   `putgd-cli` and the MCP helper like every other setting. The capture
   diagnostics say "sandboxed" instead of claiming the restricted entry matches.
7. **Plasma Wayland recording follows the same setting.** Portal mode records
   through the ScreenCast portal with restore tokens (a picker once per target
   kind); KWin mode records through `kde-screencast`, as measured with the host
   entry. The helper takes the mode from the setting rather than probing for the
   protocol, and falls back to the portal with a reason when the entry is
   missing.
8. **Window identity.** Under Flatpak, `putgd-ui` and `putgd-record` should use
   the app ID as their desktop file name so title bars and task switchers show
   the app icon. `ImageJob` and the capture helper keep using `/tmp`, which is
   fine because they stay inside the same sandbox.
9. **MCP.** `tools/mcp` takes `--cli PATH`; document a wrapper that runs
   `flatpak run --command=putgd-cli gd.put.Putgd "$@"`.
10. **Recording helper teardown.** The `libwayland-client` segfault when a
    `kde-screencast` session ends is fixed in PR #77 (`daf0792`).

## Proposed rewrite of #14

Patrick approved this rewrite on 2026-10-19, and #14 now carries it (title
"feat: publish a Flatpak without a host helper").

**Title:** feat: publish a Flatpak without a host helper

**Decision recorded.** A Flatpak ships no separately installed host binary.
It uses only what every put.gd format already relies on or what the desktop
offers to sandboxed apps: the X11 socket, the existing GNOME Shell extension,
KDE's own services that accept sandboxed callers, and portals. Screen recording
is included. DEB, RPM and AppImage keep their current behavior.

**Design.**

- Build with flatpak-builder from pinned inputs: `org.kde.Platform//6.11` (record
  the runtime, SDK and extension commits), the official AWS SDK at the locked
  commit, LayerShellQt, QtKeychain and the `ximagesrc` plugin as modules, Qt
  dynamically linked from the runtime. The manifest lives in
  `packaging/flatpak/`; Patrick chooses the app ID and whether it goes to
  Flathub or to a put.gd repository and `.flatpak` bundle.
- Cinnamon and other X11 sessions: X11 socket; parity once Cinnamon region
  capture uses a shared path (change 5).
- GNOME 50: the existing `putgd@put.gd` extension, installed on the host as for
  every other format, after changes 1 and 2. The sandbox may talk to
  `org.putgd.Gnome50` only, never to `org.gnome.Shell`.
- Plasma Wayland: shortcuts through KGlobalAccel, clipboard through data
  control, tray and layer-shell region selection. Captures and recordings use one
  of two modes chosen in Settings → Advanced (changes 6 and 7): **Desktop
  portal** by default (Screenshot portal cropped to the selection, ScreenCast
  portal with restore tokens; nothing on the host, but a one-time permission on
  Plasma 6.6, a copy of each capture in `~/Pictures`, and active-window capture
  through a picker or unavailable), or **KWin screen cast** (silent region,
  desktop and window captures and recordings from KWin's screen-cast protocol).
  The KWin mode needs a desktop entry named after the app ID in the user's
  `~/.local/share/applications`; put.gd offers to write it through a FileChooser
  save dialog the user confirms, verifies that KWin then offers the protocol,
  and falls back to the portal mode with a reason when it doesn't. The setting is
  controllable through `settings.patch`, `putgd-cli` and MCP.
- Every desktop: Background portal for start at login (change 4), a tray name
  the sandbox may own (change 3), the private socket shared through
  `xdg-run/putgd`, the default archive through `xdg-documents/put.gd`, the
  FileChooser portal for any other archive folder, the Secret portal for
  credentials, the app ID as window identity (change 8), and a documented MCP
  wrapper (change 9).
- Not in scope: a host helper binary, `--socket=session-bus`, talk access to
  `org.gnome.Shell` or `org.kde.KWin`, relying on exported `X-KDE-*` keys (patched
  Flatpak drops them), and bundling the Proton runtime (separate follow-up).

**Acceptance.**

- Build, install, update (from a repository, not only a bundle) and uninstall
  the actual bundle from pinned inputs; record runtime, SDK, extension and app
  commits plus the bundle's SHA-256. Uninstall leaves nothing on the host except
  what the user created (an optional KDE entry, the archive folder).
- Test on a patched Flatpak (1.18.4 or a distro backport of GHSA-v64f-hrwr-j4vh)
  and record the version on every host.
- Cinnamon (Mint 22.3) first, then GNOME 50 (Fedora 44), Plasma 6.6 Wayland
  (Kubuntu 26.04) and Plasma 6.3 Wayland (Debian 13), each with functional and
  memory gates, visible on the VNC wall, with saved screenshots:
  - the four capture actions and their shortcuts, including own-window refusal,
    with exact output dimensions; on Plasma Wayland in **both** capture modes:
    the portal mode's dialogs and `~/Pictures` copies recorded, and the KWin mode
    switched on from Settings → Advanced (entry written through the save dialog,
    protocol detected, silent captures with no `~/Pictures` copies), switched on
    through `settings.patch`/CLI/MCP, and with the entry removed (visible fallback
    to the portal mode);
  - unfocused clipboard import and result copy, witnessed by a native paste;
  - tray icon, menu and progress; notifications;
  - a recording with moving content whose motion score beats a static control,
    through each available path (X11, Mutter, KWin with and without the host
    entry, portal with picker and with restore token);
  - credentials stored and read back through the Secret portal with an
    unlocked login keyring or wallet, and a locked one;
  - start at login verified by an actual logout and login;
  - CLI and MCP from the host through the documented wrapper;
  - a synthetic live S3 upload with link copy and receipt-guarded deletion.
- The manifest is reviewed for every permission; none grants talk access to
  `org.gnome.Shell` or `org.kde.KWin`, or ownership outside the app ID except the
  tray name until change 3 lands.
- No Flatpak asset is advertised as parity-ready until these pass; the Plasma
  Wayland differences are stated in the release notes.

## Gates not run

- **Live S3.** No synthetic upload, link copy or deletion. Credentials can only
  be entered in the GUI, and only a visibly fake key was used (403 on Debian).
- **Memory.** No family-memory measurement of the Flatpak on any desktop.
- **Lifecycle.** Only bundle installs. No install or update from a repository,
  no update across builds, and no check that uninstall leaves nothing behind
  beyond the manual cleanup below.
- **Start at login** was not confirmed by logging out and in; only the portal's
  host entry was checked.
- **GNOME in the real session.** The bridge (captures, shortcuts, clipboard,
  own-window refusal) ran in a nested Shell, not in the logged-in session,
  because GNOME needs a re-login to load a local extension. GNOME region
  capture and the tray tooltip companion were not run; no independent paste
  witnessed the GNOME clipboard result; notifications were not checked.
- **Plasma 6.6 Wayland on hardware.** Only the Kubuntu guest ran Plasma 6.6
  Wayland (Debian's hardware desktop is 6.3). The 6.6 guest did not run the
  ScreenCast portal path, credentials or an S3 request.
- **Plasma X11.** Background portal, credentials and notifications were not run.
- **Cinnamon.** Only a guest, not physical Mint; credentials could not be tested
  (no login keyring in the guest); the region fix was not tried.
- **Ubuntu GNOME, GNOME on X11.** Not run.
- **Multiple monitors and fractional scaling.** Not run anywhere.
- **MCP.** Only the CLI was driven from the host; the MCP helper with a
  wrapper was not run.
- **Proton** in the Flatpak (its bundled runtime is not built).
- **Flathub tooling** (`flatpak-builder-lint`, AppStream metadata) was not run;
  the manifest has no metainfo file yet.
- **The proposed changes 1 to 10** are described, not implemented, so the
  two Plasma Wayland capture modes (changes 6 and 7) are unmeasured as
  features: only their pieces were measured (the portal screenshot, the host
  entry granting the screen cast, recordings through both). The
  `org_kde_plasma_window_management` grant needed for silent window capture was
  not tried.
- **Patched Flatpak on every host.** Only Debian has the export filter; the
  Fedora and Kubuntu results were on unfiltered Flatpak.

## Cleanup

All three builders: the spike app was uninstalled with its data, its
overrides and portal permissions were reset, and the `~/Documents/put.gd` and
`~/.config/autostart` folders that the test runs created (birth times checked)
were removed. The `flatpak` package (Debian, Kubuntu), `flatpak-builder`
(Fedora, Kubuntu), the per-user KDE 6.11 runtime, SDK (Fedora, Kubuntu) and
extensions, `wl-clipboard` and `libkf6service-dev` (Debian) stay installed for
the later Flatpak work; `flatpak uninstall --user --unused` removes the
runtimes. On Fedora, `mutter-devkit` was installed for the nested Shell and the
transaction was undone (Mutter back to 50.0). The temporary host entry on Debian
was removed and its desktop-entry index rebuilt with Plasma's environment. Both
guests' overlays were retired with completion manifests (`fpk-kw-0b0a5599`,
`fpk-cin-4309db74`); their run records and evidence are kept, and their wall
tiles and tunnels were removed.
