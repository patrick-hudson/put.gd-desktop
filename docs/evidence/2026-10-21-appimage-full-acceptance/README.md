# AppImage full acceptance: nightly 842 on six desktops

The first pass of the [AppImage full-acceptance profile](../../acceptance-matrix.md#appimage-full-acceptance-profile)
(items A–K), recorded 2026-10-21 by the appimage-full-acceptance session. It ran on
four physical desktops and two VMs, all with the same published AppImage.

| Build | Value |
| --- | --- |
| Release | [`v0.1.1-nightly.842`](https://github.com/patrick-hudson/put.gd-desktop/releases/tag/v0.1.1-nightly.842), published 18:33:50Z, commit `08878c9` (nightly with #318 and #321) |
| File | `putgd-0.1.1-nightly.842-x86_64.AppImage`, 115,018,232 bytes |
| SHA-256 | `36b5eac723e457b6b16de6ac3cdc365bdf6ca91178dfd9c83db6bb3611d6c2b5` (= `SHA256SUMS` on every desktop) |
| GNOME extension | `putgd-gnome50-0.1.1-nightly.842.zip`, `eae27128…`, installed as a user copy on f44 and in the Ubuntu VM |

Nightly 847 (`4a9103b`, docs only) was published during the pass. Every desktop
stayed on 842, so from the Ubuntu VM onwards the update checks correctly
report 847 as available.

## Results

✅ pass · ❌ fail (issue linked)

| Item | f44 Fedora 44 GNOME 50.0 | d69 Debian 13 Plasma 6.3.6 | k72 Kubuntu 26.04 Plasma 6.6.6 | m74 Mint 22.3 Cinnamon 6.6.9 X11 | Ubuntu 26.04 GNOME 50.1 (VM) | Aurora 44 Plasma 6.7.4 (VM) |
| --- | --- | --- | --- | --- | --- | --- |
| A. Identity | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| B. Start, tray, window icon | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| C. Idle memory (< 40 MiB) | ✅ 28.94 MiB | ✅ 29.77 MiB | ✅ 29.74 MiB | ✅ 29.17 MiB | ✅ 29.21 MiB | ✅ 29.44 MiB |
| D. Capture, Edit & upload, Save a copy… | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| E. Recording | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| F. Notification default action | ❌ [#326](https://github.com/patrick-hudson/put.gd-desktop/issues/326) | ❌ #326 | ❌ #326 | ✅ | ❌ #326 | ❌ #326 |
| G. History | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| H. Settings: theme, Updates | ❌ [#329](https://github.com/patrick-hudson/put.gd-desktop/issues/329) (Check now) | ✅ | ✅ | ✅ | ✅ | ✅ |
| I. Start at login | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| J. `appimageupdatetool --check-for-update` | ❌ [#297](https://github.com/patrick-hudson/put.gd-desktop/issues/297) | ✅ | ✅ | ✅ | ✅ | ✅ |
| K. Clean exit and cleanup | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |

Findings filed during the pass:
- **[#326](https://github.com/patrick-hudson/put.gd-desktop/issues/326):** the notification's "open in browser" does nothing from the AppImage wherever the host GLib is newer than the bundled one. The host `gio` inherits the image's `LD_LIBRARY_PATH` and dies (`undefined symbol: g_unix_mount_entry_get_options`).
- **[#329](https://github.com/patrick-hudson/put.gd-desktop/issues/329):** on Fedora 44 the AppImage's bundled OpenSSL finds no CA bundle, so **Check now** and the scheduled check fail TLS. HTTPS uploads are likely affected too; the pass didn't exercise them (loopback fixture only).
- **[#333](https://github.com/patrick-hudson/put.gd-desktop/issues/333):** Plasma regions are one pixel larger than the drag (1121×681 for 1120×680). It doesn't fail D, but it's inconsistent with GNOME and X11.
- **[#297](https://github.com/patrick-hudson/put.gd-desktop/issues/297)** (existing, upstream zsync2): appimageupdatetool segfaults on Fedora 44. The documented `SSL_CERT_FILE=/etc/pki/tls/certs/ca-bundle.crt` workaround no longer exists on F44; `/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem` works (exit 1, the correct answer). [Comment](https://github.com/patrick-hudson/put.gd-desktop/issues/297#issuecomment-6325604189).
- **[#327](https://github.com/patrick-hudson/put.gd-desktop/issues/327)** was filed for a missing window icon on m74 and then closed: it didn't reproduce in a fresh Cinnamon session (see m74 below).

## Method

The driver is [lab/afa.py](lab/afa.py), run on the desktop's host over SSH (or inside the VM guest), in the real
login session's environment. It uses no `QT_QPA_PLATFORM`, as a terminal launch would.

**Profile and uploads**
- **Private profile:** `XDG_CONFIG_HOME`, `XDG_STATE_HOME`, `XDG_CACHE_HOME` and `XDG_RUNTIME_DIR` under the run directory.
- **Data home:** the user's own `~/.local/share`. AppRun registers KWin's capture and screencast permissions and the Wayland icon entries there. `~/.local/share/putgd` didn't exist before the run and was removed after it.
- **Uploads:** to the loopback S3 fixture (`tests/fixtures/s3_server.py`, synthetic keys in session mode), with a local copy kept. No real bucket was used.
- **Clipboard:** "Copy each link" everywhere. The list behaviour was run once, in the Ubuntu VM.

**Screen and input**
- **Cover:** during captures and recordings, a magenta cover with a scrolling 75 % bar pattern filled the screen (`tools/lab/cover_player.py`; on Aurora, which has no `gtksink`, [lab/gtk_pattern.py](lab/gtk_pattern.py)). Every kept capture passed `cover_player.py check-frame`.
- **Input:** real input only. That's the uinput agent (`tools/lab/host_input.py`) on physical Wayland desktops, `xdotool` on X11, and QEMU's VNC in the VMs.

**Pictures**
- Every frame was looked at before it was kept.
- Physical desktops were captured full-screen only after Patrick had sanitized them.
- Frames that showed one of Patrick's windows were deleted, on the host and here (see d69).

**Per item**
- **A:** `identity` and `versions` subcommands. The session type comes from `loginctl` (`Type`, `Service`); the Qt platform from the plugins each process maps.
- **C:** a 180 s idle window right after start, before anything was opened. RSS is from `/proc/PID/statm`, PSS from `smaps_rollup`. The mapped libraries are checked for GTK, a KDE platform theme, `libssl`/`libcrypto`/`libgnutls` and `libQt6Network`.
- **D:**
  - The edit puts a blur (strength 24) and a solid redaction inside the pattern.
  - The uploaded object is decoded: the solid rectangle must be pure black, at least 30 % of the blur's pixels must differ from the source, and over 99 % of the pixels outside both must match.
  - **Save a copy…** opens through `editor.fileDialog`, a plain file name is typed into the chooser, and the saved file is checked.
- **E:**
  - `recording.prepare` (a 1120×680 region straddling the pattern), then `start`, `pause` after 5 s, `resume` 2 s later, and `stop` 5 s later.
  - The Review window gets its own controls (`recording.review`): in at 1 s, out 1 s before the end, and a blur 2–5 s.
  - Exports: MP4 (H.264), WebM (VP9), GIF and WebP. Plus one MP4 through the Review window's **Save copy…** chooser, and the Upload.
  - Motion is the mean frame difference over 8 samples (`putgd-record --motion`; Pillow for animated WebP). The control is the same region with the pattern stopped.
- **F:** notifications are switched on for this step only (left on, GNOME and Plasma queue banners over the full-screen cover). A capture is re-uploaded, the banner is cropped and clicked with real input, and `dbus-monitor` records `ActionInvoked`. A private `x-scheme-handler/http(s)` handler records any URL put.gd opens.
- **H:** `settings.patch` switches the theme with Settings open, and the window's scheme is read back (`ui.viewState`). Then `update.status`, and `update.check` against the live `app.put.gd` feed.
- **I:**
  - Launch at login is turned on; put.gd writes the managed entry (#318's `appimage-start.sh`) in the private config home. The same entry is published in `~/.config/autostart`, because the session only reads that one.
  - The private resident is quit, then there's a real logout and an autologin (see the per-desktop timelines).
  - The login-started instance runs with the default profile. It reported `fromAppImage: true` and was then quit from the tray (K).
- **K:** a real click on the tray icon, then on **Quit**. Then a check that no put.gd process, FUSE mount, `putgd*` desktop entry, autostart entry or profile file remains. Everything the run created was removed and the locks were released `--verified-clean`.

Raw results per desktop are in [results/](results/): `results.json` (every step, including start, memory, captures, notifications, login and quit), `identity.json`, `versions.json`, `edit.json`, `record.json`, `settings.json`, `history.json` and `timeline.txt` (logout, login and quit times). [lab/summarize.py](lab/summarize.py) prints the numbers below from them.

## Versions

| Desktop | OS, kernel | Session (`loginctl`) | Desktop and compositor | Qt platform (putgd-ui) | GPU |
| --- | --- | --- | --- | --- | --- |
| f44 (physical) | Fedora 44, 6.19.10-300.fc44 | wayland, gdm-autologin | GNOME Shell 50.0, mutter 50.0-1.fc44 | `libqwayland-generic.so` | HD 530 |
| d69 (physical) | Debian 13, 6.12.107+deb13 | wayland, sddm-autologin | Plasma 6.3.6, KWin 6.3.6 | `libqwayland-generic.so` | HD 530 |
| k72 (physical) | Kubuntu 26.04.1, 7.0.0-38 | wayland, sddm-autologin | Plasma 6.6.6, KWin 6.6.6 | `libqwayland-generic.so` | HD 530 |
| m74 (physical) | Linux Mint 22.3, 6.14.0-37 | x11, lightdm-autologin | Cinnamon 6.6.9, muffin 6.6.3, Xorg 21.1.12 | `libqxcb.so` | HD 530 |
| Ubuntu VM on d69 | Ubuntu 26.04.1, 7.0.0-38 | wayland, gdm-autologin | GNOME Shell 50.1, libmutter-18 50.1 | `libqwayland-generic.so` | virtio-vga |
| Aurora VM on f44 | Aurora 44.20260908.1 (Kinoite) | wayland, plasmalogin-autologin | Plasma 6.7.4, KWin 6.7.4 | `libqwayland-generic.so` | virtio-vga |

In every case the resident (`putgd`) maps no Qt platform plugin; it's the core resident.

## Memory (C)

| Desktop | RSS (MiB) | PSS (MiB) | Flagged libraries | Load at start → end |
| --- | --- | --- | --- | --- |
| f44 | 28.94 | 22.62 | none | 1.40 → 0.07 (fresh login, settled 60 s first) |
| d69 | 29.77 | 24.08 | none | 0.43 → 0.05 |
| k72 | 29.74 | 23.38 | none | 0.08 → 0.00 |
| m74 | 29.17 | 23.37 | none | 0.00 → 0.00 |
| Ubuntu VM | 29.21 | 22.87 | none | 0.75 → 0.03 |
| Aurora VM | 29.44 | 23.02 | none | 1.20 → 1.29 (guest baseline) |

## Recording (E)

| Desktop | Acquisition | Live encoder | MP4 | WebM | GIF | WebP | Static control (MP4/GIF) | Save copy… chooser |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| f44 | `mutter-screencast` | bundled `x264enc` (no host H.264) | 20.32 | 20.33 | 20.23 | 24.11 | 0.0 / 0.0 | portal (GNOME) |
| d69 | `kde-screencast` (KWin 6.3.6) | VA-API `vah264lpenc` | 20.26 | 20.30 | 20.30 | 24.14 | 0.0 / 0.0 | portal (KDE) |
| k72 | `kde-screencast` (KWin 6.6.6) | VA-API `vah264lpenc` | 20.18 | 20.20 | 20.34 | 24.21 | 0.0 / 0.0 | portal (KDE) |
| m74 | X11 | VA-API `vah264lpenc` | 20.08 | 20.13 | 19.93 | 24.55 | 0.0 / 0.0 | portal (GTK) |
| Ubuntu VM | `mutter-screencast` | bundled `x264enc` | 35.76 | 35.71 | 35.48 | 40.16 | 0.0 / 0.0 | portal (GNOME) |
| Aurora VM | `kde-screencast` (KWin 6.7.4) | bundled `x264enc` | 35.78 | 35.76 | 35.27 | 40.48 | 0.0 / 0.0 | portal (KDE) |

Each recording was 11.4–13.0 s with a 2 s pause, and the edit stored the trim (1 s to
end − 1 s) and one timed blur mask. On Plasma, AppRun's `putgd-record-appimage-*` KWin
entry was present during recording and gone after **Quit**. The d69 WebP was scored
with Pillow on the workstation, because d69 has no Pillow. The VMs' pattern moves
more per frame against a smaller canvas, so their scores are higher.

## f44: Fedora 44 GNOME 50.0 Wayland (physical)

- A–E, G, I and K pass. **F** fails (#326, glib2 2.88.4). **H**: the themes repaint, but **Check now** fails TLS (#329). **J**: appimageupdatetool segfaults (#297).
- GNOME 50 needs the release's own `putgd@put.gd` extension: it was installed as a user copy and loaded by re-login #1. `capabilities` reported extension 2, the packaged version (not outdated). The tray comes from AppIndicator.
- C: the window started right after re-login #1, after a 60 s settle (load 1.40), and ended at 0.07. No `libcrypto` is mapped (the RPM build in #303 maps one).
- Timeline (UTC): extension installed 20:01:35; logout 20:01:43, GDM restart 20:01:55; logout 20:15:00, GDM restart 20:15:06 → put.gd at 20:15:09; tray Quit 20:15:54.
- After the slot, GNOME idle-suspended f44 at 20:30:51Z: `sleep-inactive-ac` is 900 s, and the 15 minutes started at re-login #2. It resumed at 20:34:19Z. The coordinator has since turned idle suspend off on f44, for the user session and the GDM greeter. Only the Aurora VM was running then; see Aurora below.

![f44: Tray icon and menu](pictures/f44-tray-menu.png)
![f44: Settings](pictures/f44-settings-window.png)
![f44: A capture in Edit & upload](pictures/f44-editor.png)
![f44: Save a copy…: the portal chooser](pictures/f44-save-copy-dialog.png)
![f44: Review window](pictures/f44-review.png)
![f44: Link-ready notification](pictures/f44-notify-notification.png)

## d69: Debian 13 Plasma 6.3.6 Wayland (physical)

- All items except **F** pass (#326, GLib 2.84.4). E uses `kde-screencast` through AppRun's `putgd-record-appimage-*` KWin entry. The region is 1121×681 for a 1120×680 drag (#333).
- The Alt+Tab switcher frame was deleted because it showed one of Patrick's windows. The icon is shown by the title bar and the task-manager entry in the Settings picture.
- Timeline (UTC): ChatGPT closed 19:45:32; xterm minimized 19:51:33; logout 19:57:04; SDDM greeter, `Relogin=true` drop-in and restart 19:59:46 → put.gd at 19:59:50; drop-in removed 19:59:52; tray Quit 20:00:32.

![d69: Tray icon and menu](pictures/d69-tray-menu.png)
![d69: Settings](pictures/d69-settings-window.png)
![d69: A capture in Edit & upload](pictures/d69-editor.png)
![d69: Save a copy…: the portal chooser](pictures/d69-save-copy-dialog.png)
![d69: Review window](pictures/d69-review.png)
![d69: Link-ready notification](pictures/d69-notify-notification.png)

## k72: Kubuntu 26.04 Plasma 6.6.6 Wayland (physical)

- All items except **F** pass (#326, GLib 2.88.0). `kde-screencast` reports the requester hidden. The region is 1121×681 (#333). Copy image and the link were read through a GTK 4 clipboard witness (no `wl-paste` here).
- K: the driver couldn't see the menu, because the restarted wall VNC sent only damaged areas. So the tray icon and **Quit** were clicked by hand with the same uinput agent, and the leftover check ran afterwards (no process, mount or entry).
- Timeline (UTC): logout 19:41:23 (`org.kde.Shutdown.logout`), SDDM restart 19:41:32 → put.gd at 19:41:41; tray Quit 19:44:29.

![k72: Tray icon and menu](pictures/k72-tray-menu.png)
![k72: Settings](pictures/k72-settings-window.png)
![k72: A capture in Edit & upload](pictures/k72-editor.png)
![k72: Save a copy…: the portal chooser](pictures/k72-save-copy-dialog.png)
![k72: Review window](pictures/k72-review.png)
![k72: Link-ready notification](pictures/k72-notify-notification.png)

## m74: Linux Mint 22.3 Cinnamon 6.6.9 X11 (physical)

- Every item passes. It's the only desktop where F works: Mint 22.3's GLib is no newer than the image's. The first banner click registered no `ActionInvoked`; the second, on the same banner position, opened the link.
- B: in the first Cinnamon session (running since before the pass, after a DEB install and purge earlier that day) the Settings window showed a placeholder gear in the window list and no icon in Alt+Tab. After the re-login, both the login-started and a terminal-started instance showed the put.gd icon, so [#327](https://github.com/patrick-hudson/put.gd-desktop/issues/327) was closed. Mint-Y title bars draw no app icon.
- Timeline (UTC): logout 19:20:20 (`cinnamon-session-quit`), slick-greeter, `systemctl restart lightdm` 19:22:12 → put.gd at 19:22:15; tray Quit 19:26:18.

![m74: Tray icon and menu](pictures/m74-tray-menu.png)
![m74: Settings](pictures/m74-settings-window.png)
![m74: A capture in Edit & upload](pictures/m74-editor.png)
![m74: Save a copy…: the portal chooser](pictures/m74-save-copy-dialog.png)
![m74: Review window](pictures/m74-review.png)
![m74: Link-ready notification](pictures/m74-notify-notification.png)

## Ubuntu 26.04 GNOME 50.1 Wayland (VM on d69)

- All items except **F** pass (#326). It's an overlay of `gnome-provisioned-20261016` (`563730cc…`): QEMU on d69, 2 vCPU/6 GiB, virtio-vga, wall tile via workstation port 5991. The extension was installed as a user copy and loaded by a reboot. The `list` clipboard behaviour was checked here: one upload, link copied.
- I: put.gd started from the AppImage 4 s after GNOME Shell, after a reboot at 19:34:30Z (gdm autologin).

![Ubuntu 26.04 GNOME 50.1 Wayland (VM on d69): Tray icon and menu](pictures/ubuntu-vm-tray-menu.png)
![Ubuntu 26.04 GNOME 50.1 Wayland (VM on d69): Settings](pictures/ubuntu-vm-settings-window.png)
![Ubuntu 26.04 GNOME 50.1 Wayland (VM on d69): A capture in Edit & upload](pictures/ubuntu-vm-editor.png)
![Ubuntu 26.04 GNOME 50.1 Wayland (VM on d69): Save a copy…: the portal chooser](pictures/ubuntu-vm-save-copy-dialog.png)
![Ubuntu 26.04 GNOME 50.1 Wayland (VM on d69): Review window](pictures/ubuntu-vm-review.png)
![Ubuntu 26.04 GNOME 50.1 Wayland (VM on d69): Link-ready notification](pictures/ubuntu-vm-notify-notification.png)

## Aurora 44 Plasma 6.7.4 Wayland (VM on f44)

- All items except **F** pass (#326, glib2 2.88.3). It's an overlay of `aurora-stable-44.20260908.1-v3` (`6a2fb407…`), UEFI: QEMU on f44, 2 vCPU/6 GiB, wall tile via workstation port 5992. Nothing was installed in the guest; FUSE comes from the image (fuse 2 and 3).
- Check now **works** here (Aurora ships `/etc/ssl/certs/ca-certificates.crt`), and so does appimageupdatetool (exit 1). The region is exactly 980×600 on KWin 6.7.4, so #333 is an older-KWin behaviour.
- Aurora has no GStreamer `gtksink`, so the cover and pattern came from [lab/gtk_pattern.py](lab/gtk_pattern.py) (GTK 3 + cairo). The guest reached its own QEMU display at `10.0.2.2:5926`.
- The host suspend (20:30:51–20:34:19Z, see f44) froze the guest with it. C had finished at 20:30:31Z, before the suspend. B's tray and window steps, D–H, J, I and K all ran after the resume, and all passed except F.
- I: reboot at 20:41:31Z (plasmalogin autologin) → put.gd at 20:42:37Z.

![Aurora 44 Plasma 6.7.4 Wayland (VM on f44): Tray icon and menu](pictures/aurora-vm-tray-menu.png)
![Aurora 44 Plasma 6.7.4 Wayland (VM on f44): Settings](pictures/aurora-vm-settings-window.png)
![Aurora 44 Plasma 6.7.4 Wayland (VM on f44): A capture in Edit & upload](pictures/aurora-vm-editor.png)
![Aurora 44 Plasma 6.7.4 Wayland (VM on f44): Save a copy…: the portal chooser](pictures/aurora-vm-save-copy-dialog.png)
![Aurora 44 Plasma 6.7.4 Wayland (VM on f44): Review window](pictures/aurora-vm-review.png)
![Aurora 44 Plasma 6.7.4 Wayland (VM on f44): Link-ready notification](pictures/aurora-vm-notify-notification.png)

## Things done on the hosts

All of these were approved by Patrick, through the coordinator and in this session.

- **f44:** two real logouts. The first loaded the release's extension; the second was item I. Firefox had already been closed. The `putgd@put.gd` user copy was uninstalled at the end, and `enabled-extensions` restored to `[background-logo, appindicatorsupport]`.
- **d69:** ChatGPT/Codex (PID 1532) was closed at 19:45:32Z. Patrick's btop xterm was minimized at 19:51:33Z, after the Alt+Tab check had raised it; that frame and its crop were deleted. One real logout, for item I. Debian's SDDM autologins only on its first start per boot, so a temporary `/etc/sddm.conf.d/zz-afa-relogin.conf` (`Relogin=true`) was added for one SDDM restart and removed 6 s later. Plasma's own session restore reopened the xterm after the login.
- **k72:** one real logout, for item I (Patrick's apps were already closed). After the re-login, the wall's `w0vncserver` restart-looped for about a minute ("Introspect call failed"), then recovered by itself.
- **m74:** one real logout, for item I. LightDM shows its greeter after a logout, so `systemctl restart lightdm` triggered the autologin.
- **Ubuntu VM:** the `gnome-provisioned-20261016` golden carries an old put.gd DEB (0.1.1~dev.63) and its system extension. The overlay removed it (`apt-get remove putgd`) and added `gstreamer1.0-gtk3` for the cover pattern before the AppImage ran.
- **Retired afterwards:** both VM overlays, their tunnels and wall tiles. The goldens still hash `563730cc…` (Ubuntu) and `6a2fb407…` (Aurora v3).
