# AppImage full acceptance, second pass: nightly 942 on six desktops

This is the second pass of the [AppImage full-acceptance profile](../../acceptance-matrix.md#appimage-full-acceptance-profile) (items A–K). It was recorded on 2026-10-22 by the appimage-acceptance-rerun session, on four physical desktops and two VMs, all running the same published AppImage.

- **Why a second pass:** the [first pass](../2026-10-21-appimage-full-acceptance/README.md) (nightly 842) failed F on five desktops (#326), H on f44 (#329) and J on f44 (#297).
- **The build under test:** the first nightly with the fix round Patrick asked for: #348/#349 (updater polish, [#358](https://github.com/patrick-hudson/put.gd-desktop/pull/358)), #341 (shortcuts while a menu is open, [#357](https://github.com/patrick-hudson/put.gd-desktop/pull/357)), #333 (Plasma region size, [#356](https://github.com/patrick-hudson/put.gd-desktop/pull/356)) and the AppImage part of #344 (channel switch, [#359](https://github.com/patrick-hudson/put.gd-desktop/pull/359)).
- **Item F changed during the pass:** Patrick asked for a real browser. On every desktop, F was redone by clicking the notification for a public upload to the live S3 test destination.

| Build | Value |
| --- | --- |
| N | [`v0.1.1-nightly.942`](https://github.com/patrick-hudson/put.gd-desktop/releases/tag/v0.1.1-nightly.942), published 05:13:42Z, commit `0435ed6a` (contains `3e27b254` #356, `51ac8a3a` #358, `d50c227b` #357, `c7d6a013` #359) |
| File | `putgd-0.1.1-nightly.942-x86_64.AppImage`, 115,370,488 bytes |
| SHA-256 | `3594d1bd8a291f7d4c2caa7ef15c6bcc5b7bcc5036b872d9334e5dc487c2a47b`, the same as `SHA256SUMS` on every desktop. `SHA256SUMS.minisig` from `pkg.put.gd/releases` verifies with the nightly key (trusted comment `put.gd v0.1.1-nightly.942 nightly`). |
| GNOME extension | `putgd-gnome50-0.1.1-nightly.942.zip`, installed as a user copy on f44 and in the Ubuntu VM |
| N+1 (for J) | `v0.1.1-nightly.947`, published 05:40:12Z (`476e5c7b`, docs only), AppImage `e75d5a01…`. Its `SHA256SUMS.minisig` verifies too. |
| N−1 (for J) | `v0.1.1-nightly.925` (`124fb3c0`). It has #356, #357 and #358, but not #359. |

## Results

✅ pass · ❌ fail · 🔸 pass with a note (below the table)

| Item | f44 Fedora 44 GNOME 50.0 | d69 Debian 13 Plasma 6.3.6 | k72 Kubuntu 26.04 Plasma 6.6.6 | m74 Mint 22.3 Cinnamon 6.6.9 X11 | Ubuntu 26.04 GNOME 50.1 (VM) | Aurora 44 Plasma 6.7.4 (VM) |
| --- | --- | --- | --- | --- | --- | --- |
| A. Identity | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| B. Start, tray, window icon | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| C. Idle memory (< 40 MiB) | ✅ 29.28 MiB | ✅ 30.09 MiB | ✅ 30.02 MiB | ✅ 29.58 MiB | ✅ 29.36 MiB | ✅ 30.08 MiB |
| D. Capture, Edit & upload, Save a copy… | ✅ | ✅ | ✅ | ✅ | 🔸 origin −1,−1 | 🔸 origin −1,−1 |
| D. #333 region size W×H | ✅ exact, origin 0,0 | ✅ exact, origin 0,0 | ✅ exact, origin 0,0 | ✅ exact, origin 0,0 | ✅ exact | ✅ exact |
| E. Recording | ✅ | ✅ | ✅ | ✅ | ✅ | 🔸 control 0.015 |
| F. Notification → real browser (#326) | ✅ | 🔸 ✅ | 🔸 ✅ | ✅ | ✅ | ✅ |
| G. History | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| H. Settings: theme, Updates, Check now (#329) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| H. #348 first open lands on Updates | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| H. #349 clean `update.status` / `zsync.output` | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| H. #344 Channel: Stable/Nightly only, Nightly preselected; Stable says "no stable release yet" | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| #341 shortcut with put.gd's tray menu open | ✅ | ✅ | ✅ | ✅ (+ window, notice, Cinnamon menu) | ✅ | ✅ |
| I. Start at login | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| J. `appimageupdatetool --check-for-update` | 🔸 ✅ with `SSL_CERT_FILE` (#297) | ✅ | ✅ | ✅ | ✅ | ✅ |
| J. In-app update, live feed: 942 → 947 | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| K. Clean exit and cleanup | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |

**Notes**

- **D, VMs:** the size is exact, but the region starts one pixel up and left (−1,−1). #356 floors fractional pointer positions, and VNC's absolute input lands just below the integer. Physical Plasma and GNOME, and X11, give origin 0,0. Per the coordinator, this is a note, not a fail.
- **E, Aurora:** the static control scores 0.015 (MP4) and 0.005 (GIF), against 26 with the moving pattern. This is the software x264 path's noise in that guest: two clean runs agree, and nothing moves in the region.
- **F, d69 and k72:** their default `http(s)` handler is `chatgpt.desktop`, the ChatGPT desktop app, not a browser. With Patrick's OK, the test made Firefox the https default for the test only, with a temporary profile, so the click reached a real browser. Afterwards both `mimeapps.list` files were restored byte for byte, and `gio` resolved `chatgpt.desktop` again. A user on those hosts would get the ChatGPT app, which is how they're configured, not a put.gd defect.
- **F, k72:** with the loopback-handler check, the first banner click registered no `ActionInvoked` at the notification server, and the second one opened the link. 842 saw the same on m74. The real-browser click worked first time.
- **J, f44:** stock Fedora 44's appimageupdatetool still segfaults (#297, upstream). The documented workaround, `SSL_CERT_FILE=/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem`, gives the correct answer. The in-app updater (#343) doesn't depend on it: it passes the host CA bundle itself, and 942 → 947 by zsync worked on stock f44.

## Fixes since 842

**#326 / #330, F, notification "open in browser":**
- On every desktop the click goes `ActionInvoked default` → `Opened on the host` `route: portal`.
- With the loopback recorder as the default handler, the opener ran with no `LD_LIBRARY_PATH`, `QT_PLUGIN_PATH` or `APPDIR` (`lib=unset plugins=unset appdir=unset`).
- With the real browser, it loaded the uploaded image from `https://put.gd/acceptance/afa2/<desktop>/…` (table below).

**#329 / #334, H, Check now on stock Fedora 44:** it finished in 0.28 s with no TLS error ("up to date", 942 = latest at that moment). Every desktop answered in under 0.8 s.

**#297 / #343, J, updates:**
- `appimageupdatetool --check-for-update` gives the right answer everywhere: exit 0 while 942 was the newest, exit 1 in the VMs once 947 was out. On f44 that needs `SSL_CERT_FILE`.
- The in-app update ran against the live `app.put.gd` feed and the `pkg.put.gd` mirror, with the real appimageupdatetool (build 254, `8d17a50e…`):

| Update | Desktops | Method, fetched | Time | Result |
| --- | --- | --- | --- | --- |
| 942 → 947 (N → N+1) | f44, d69, k72, m74, Ubuntu VM, Aurora VM | zsync: 115.0 MB reused, 1.06 MB fetched | 8.3–11.0 s | The file is now `e75d5a01…` (947's `SHA256SUMS`), and the hidden backup held `3594d1bd…` (942) until **Restart put.gd**. 947 then runs from the same path and reports up to date, and the backup is removed. |
| 925 → 947 (N−1 → newest) | f44, d69, k72, m74 | zsync: 110.1 MB reused, 5.78 MB fetched | 9.9–12.4 s | Same as above. 947 published before this ran, so N−1 → N (925 → 942) couldn't be taken from the live feed. |

**#349, clean output:**
- Every `zsync.output` reads like a terminal, for example `zsync2: used 115023872 local, fetched 1063416 | 100.00% done (110.03 of 110.03 MiB)... | Validation warning: AppImage not signed | Update successful…`, with no control characters.
- `update.status` had no control characters on any desktop, at start or after Check now (`status-clean*.json`).

**#348, Settings on Updates:**
- On a resident whose Settings had never been shown, a single `ui.show {"view":"updates"}` landed with the Updates group in view on all six. That includes the 1280×800 VMs, where it used to need a second call.
- The frames after the 942 → 947 restart were taken with one call too.

**#333, region size:**
- An 80×60 and a 1120×680 real-input drag (980×600 in the VMs) gave exactly those sizes on every desktop.
- A static full-screen pattern encodes each pixel's own coordinates ([lab/coord_pattern.py](lab/coord_pattern.py)), so the captured PNG gives its exact origin. The origin is 0,0 on d69 (KWin 6.3.6), k72 (6.6.6), f44 and m74, and −1,−1 on the VNC-driven VMs.

**#341, shortcuts with a menu open:**
- With put.gd's own tray menu open (a real click), the desktop shortcut's capture contains the menu on all six (`menuInCapture` 1.0).
- The region shortcut gave an exact 80×60 on all six. On X11 the selection starts once the menu closes; on GNOME and Plasma it starts at once.
- m74 (Cinnamon X11) also passed these:
  - the window shortcut with the tray menu open captures the focused window (584×232), and the panel menu isn't in it, as #357 documents;
  - with the menu held open, region selection gives up after 3.0 s with the `input_grab` notice;
  - with Cinnamon's own main menu (another client's grab) open, the desktop capture holds that menu, and the region gives 80×60.

**#344 (#359), channel switch:**
- On a fresh profile, Settings → Updates → **Channel** offers exactly **Stable** and **Nightly**, with Nightly selected. The stored value is still `build`, as #359 specifies. A real click opened the list for the picture on all six.
- Switching to Stable gives `install.reason: no_release` against the live feed, with no error and no button. Switching back gives nightly again.
- The lab-signed stable feed that #359 used for a real switch was deleted after its own acceptance, so no cross-channel install ran here.

## Findings

- **[#391](https://github.com/patrick-hudson/put.gd-desktop/issues/391) (new, spotted by Patrick on the wall):** at 1280×800, once the rail shows **Update available**, the rail's **Settings** item is drawn over the **Capture output** footer. It shows in the Aurora and the Ubuntu VM frames. The rail is a fixed `QVBoxLayout` with no scroll area.
- **Not product findings, all from my harness:**
  - **Tray icon finder:** on Plasma it matched the task-manager entry for the open Settings window, and on Aurora the Klipper icon. The fixes are in "Things done on the hosts" below. The affected B pictures and #341 runs were redone with put.gd's own icon; the superseded records are kept under `superseded-*/`.
  - **Aurora's uupd notification:** its "System Warning" overlapped the capture region once. The privacy check then deleted that capture (an uncovered edge), so D's edit and F were rerun after it closed.

## Method

The drivers are in [lab/](lab/), and they reuse the 842 pass's.
- [afa.py](lab/afa.py) is unchanged, except that the profile now has the default shortcuts Ctrl+Shift+1/2/3, which #341 needs.
- [rerun.py](lab/rerun.py) adds one check per fix: `updates-first-open`, `status-clean`, `channel`, `channel-options`, `menu-shortcut`, `region-size --coords`, `update-check --ssl-cert-file`, `live-s3` and `browser-shot`.
- [upd293.py](lab/upd293.py) is #343's driver, with a `--live` mode: no lab URL overrides, so the real feed and mirror are used.
- [i326.py](lab/i326.py) supplies #326's loopback recorder.
- The shell scripts drive one desktop each: `ac.sh` (A–C), `dk.sh` (B, D–H), `live.sh` (in-app update), `relogin.sh` (I), `fa.sh`/`fa2.sh` (real-browser F) and `redo341.sh`.
- [verdict.py](lab/verdict.py) prints one line per item from a run's results, and [annotate_rerun.py](lab/annotate_rerun.py) draws the pictures.

**Profiles, uploads and screen:**
- **Profiles:** each run had a private config, state, cache and runtime directory, and the user's own data home, as in 842.
- **Uploads:** A–E, G and H used the loopback S3 fixture with synthetic keys.
- **Cover:** the magenta cover with the moving pattern was up during captures and recordings, and every kept capture passed `cover_player.py check-frame`.
- **Input:** real input only: the uinput agent on physical Wayland, xdotool on X11, QEMU's VNC in the VMs.

**Real-browser F (Patrick, 2026-10-22):**
- **Upload:** a separate private run uploads a synthetic PNG through Edit & upload. The destination is the live bucket `put.gd` (us-east-2) under `acceptance/afa2/<desktop>/`, with `aclMode public-read` and `publicUrlBase https://put.gd`.
- **Credentials:** key set 2 was read on the builder and piped straight into the resident's `setup.storeCredentials`. It was never printed or stored.
- **Permissions:** the first upload had no ACL and answered 403, and key set 2 couldn't set ACLs. Patrick then added `s3:PutObjectAcl` (inline policy `putgd-acceptance-acl`). A single public-read probe answered `HTTP/2 200 image/png` before any F run.
- **The click:** notifications on, a real click on the link-ready banner. The browser is the desktop's own default, in a temporary profile:
  - **Physical desktops:** [ffprofile.py](lab/ffprofile.py) made a temporary profile the default through `profiles.ini`/`installs.ini`, both backed up. On d69 and k72, [ffdefault.py](lab/ffdefault.py) instead set a Firefox `--profile` entry as the https handler.
  - **VMs:** the guest's own first-run Firefox. With Patrick's OK, "Continue" on Firefox's first-run welcome screen was accepted in the disposable VMs only.
- **Proof:** `browser-shot` records the browser process and the resident's `Opened on the host` line, and keeps a full-desktop frame.
- **Cleanup:** the object was removed with **Delete from destination** (the link answers 403 after). The browser was closed by its PID, both files were restored byte for byte, and the temporary profile was deleted. So was anything that profile created in the Firefox root (by birth time, against a listing taken before).
- **Final check:** `put.gd/acceptance/afa2/` listed empty with key set 2 (0 objects, 0 versions, 0 incomplete uploads) after every desktop. The last listing was at 07:11:00Z.

**Everything else:**
- **VMs:** overlays of the `gnome-provisioned-20261016` golden (on d69) and the Aurora v3 golden (on f44), launched by nightly's `tools/lab/lab.py` from a private tree. Each had a wall tile (workstation ports 5991 and 5992).
- **Retirement:** `lab.py reset`, then the guarded `cleanup` with an explicit evidence manifest ([lab/manifest.py](lab/manifest.py)). Receipts are in `results/*/lab-runs/`.
- **Raw results:** per desktop in [results/](results/). The timelines (`results/timeline-*.txt`) record every lock, logout, login, app closure, config change and cleanup, with UTC times.

## Versions

| Desktop | OS, kernel | Session (`loginctl`) | Desktop, compositor | Qt platform (putgd-ui) | GPU |
| --- | --- | --- | --- | --- | --- |
| f44 (physical) | Fedora 44, 6.19.10-300.fc44 | wayland, gdm-autologin | GNOME Shell 50.0 | `libqwayland-generic.so` | HD 530 |
| d69 (physical) | Debian 13, 6.12.107+deb13 | wayland, sddm-autologin | Plasma 6.3.6, KWin 6.3.6 | `libqwayland-generic.so` | HD 530 |
| k72 (physical) | Kubuntu 26.04.1, 7.0.0-38 | wayland, sddm-autologin | Plasma 6.6.6, KWin 6.6.6 | `libqwayland-generic.so` | HD 530 |
| m74 (physical) | Linux Mint 22.3, 6.14.0-37 | x11, lightdm-autologin | Cinnamon 6.6.9, muffin 6.6.3 | `libqxcb.so` | HD 530 |
| Ubuntu VM on d69 | Ubuntu 26.04.1, 7.0.0-38 | wayland, gdm-autologin | GNOME Shell 50.1 | `libqwayland-generic.so` | virtio-vga |
| Aurora VM on f44 | Aurora 44.20260908.1, 7.1.8-200.fc44 | wayland, plasmalogin-autologin | Plasma 6.7.4, KWin 6.7.4 | `libqwayland-generic.so` | virtio-vga |

The resident (`putgd`) maps no Qt platform plugin on any desktop.

## Memory (C)

These are 180 s idle windows right after start, with nothing opened. No GTK, KDE platform theme, `libssl`/`libcrypto`/`libgnutls` or `libQt6Network` was mapped on any desktop.

| Desktop | RSS (MiB) | PSS (MiB) | Load at start → end | 842 RSS |
| --- | --- | --- | --- | --- |
| f44 | 29.28 | 22.98 | 1.18 → 0.21 (60 s after re-login #1) | 28.94 |
| d69 | 30.09 | 24.35 | 0.08 → 0.13 | 29.77 |
| k72 | 30.02 | 23.55 | 0.06 → 0.01 | 29.74 |
| m74 | 29.58 | 23.69 | 0.71 → 0.06 | 29.17 |
| Ubuntu VM | 29.36 | 22.99 | 0.08 → 0.02 | 29.21 |
| Aurora VM | 30.08 | 23.55 | 2.54 → 0.23 (guest settling) | 29.44 |

The +0.1 to +0.4 MiB over 842 matches what #357 (XI2 raw keys, +0.28 MiB on m74) and #358/#359 measured.

## Recording (E)

| Desktop | Acquisition | Live encoder | MP4 | WebM | GIF | WebP | Static control (MP4/GIF) | Save copy… |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| f44 | `mutter-screencast` | bundled `x264enc` | 20.65 | 20.64 | 21.19 | 25.30 | 0.0 / 0.0 | portal (GNOME) |
| d69 | `kde-screencast` | VA-API `vah264lpenc` | 20.14 | 20.19 | 19.85 | 24.28 (scored on the workstation) | 0.0 / 0.0 | portal (KDE) |
| k72 | `kde-screencast` | VA-API `vah264lpenc` | 20.09 | 20.15 | 20.03 | 23.84 | 0.0 / 0.0 | portal (KDE) |
| m74 | X11 | VA-API `vah264lpenc` | 20.23 | 20.28 | 20.16 | 24.13 | 0.0 / 0.0 | portal (GTK) |
| Ubuntu VM | `mutter-screencast` | bundled `x264enc` | 26.16 | 26.16 | 25.94 | 30.39 | 0.0 / 0.0 | portal (GNOME) |
| Aurora VM | `kde-screencast` | bundled `x264enc` | 26.17 | 26.18 | 25.99 | 31.02 | 0.015 / 0.005 | portal (KDE) |

- On Plasma, AppRun's `putgd-record-appimage-*` KWin entry was present during recording and gone after **Quit**.
- Each recording had a trim (1 s in, 1 s before the end) and one timed blur, set in the Review window.

## Real-browser F

| Desktop | Default browser | Link opened by the click | After |
| --- | --- | --- | --- |
| f44 | Firefox (RPM), temporary profile | `https://put.gd/acceptance/afa2/f44/Screen-Capture-2026-10-22-01-48-59.png` | deleted (403), profile and its new `Profile Groups` store removed, ini files restored |
| d69 | ChatGPT app → for the test, Firefox ESR `--profile` (temporary) | `…/afa2/d69/Screen-Capture-2026-10-22-02-08-16.png` | deleted, `mimeapps.list` files restored, gio resolves `chatgpt.desktop` again |
| k72 | ChatGPT app → for the test, snap Firefox `--profile` (temporary) | `…/afa2/k72/Screen-Capture-2026-10-22-02-04-39.png` | deleted, `mimeapps.list` files restored, gio resolves `chatgpt.desktop` again |
| m74 | Firefox (DEB), temporary profile | `…/afa2/m74/Screen-Capture-2026-10-22-01-44-36.png` | deleted, profile, its new store and `Crash Reports/glean` removed, ini files restored |
| Ubuntu VM | snap Firefox (guest default, first run) | `…/afa2/ubuntu-vm/Screen-Capture-2026-10-22-01-32-51.png` | deleted |
| Aurora VM | Firefox Flatpak (guest default, first run, "Continue" with Patrick's OK) | `…/afa2/aurora-vm/Screen-Capture-2026-10-22-01-36-18.png` | deleted |

## Pictures

Every picture is a full-desktop frame from 942, captured during this pass. The caption names the host, the desktop and the build, and each frame was looked at before it was kept. Physical desktops were captured full-screen after Patrick's sanitising, with his apps closed first. The d69 tray picture was retaken during the F slot, because the first one showed an emoji popup opened by a harness mis-click.

### f44: Fedora 44 GNOME 50.0 Wayland (physical)

![f44: Tray menu](pictures/f44-tray-menu.png)
![f44: Settings](pictures/f44-settings-window.png)
![f44: #348: first open lands on Updates](pictures/f44-348-first-open.png)
![f44: #344: Channel offers Stable and Nightly](pictures/f44-344-channel.png)
![f44: Edit & upload](pictures/f44-editor.png)
![f44: Save a copy…: portal chooser](pictures/f44-save-copy-dialog.png)
![f44: Review window](pictures/f44-review.png)
![f44: F: the default browser loads the uploaded image](pictures/f44-F-browser.png)
![f44: J: 942 installs 947 by zsync](pictures/f44-update-zsync.png)
![f44: J: updated from 942 to 947](pictures/f44-update-restarted.png)

### d69: Debian 13 Plasma 6.3.6 Wayland (physical)

![d69: Tray menu](pictures/d69-tray-menu.png)
![d69: Settings](pictures/d69-settings-window.png)
![d69: #348: first open lands on Updates](pictures/d69-348-first-open.png)
![d69: #344: Channel offers Stable and Nightly](pictures/d69-344-channel.png)
![d69: Edit & upload](pictures/d69-editor.png)
![d69: Save a copy…: portal chooser](pictures/d69-save-copy-dialog.png)
![d69: Review window](pictures/d69-review.png)
![d69: F: the default browser loads the uploaded image](pictures/d69-F-browser.png)

### k72: Kubuntu 26.04 Plasma 6.6.6 Wayland (physical)

![k72: Tray menu](pictures/k72-tray-menu.png)
![k72: Settings](pictures/k72-settings-window.png)
![k72: #348: first open lands on Updates](pictures/k72-348-first-open.png)
![k72: #344: Channel offers Stable and Nightly](pictures/k72-344-channel.png)
![k72: Edit & upload](pictures/k72-editor.png)
![k72: Save a copy…: portal chooser](pictures/k72-save-copy-dialog.png)
![k72: Review window](pictures/k72-review.png)
![k72: F: the default browser loads the uploaded image](pictures/k72-F-browser.png)

### m74: Linux Mint 22.3 Cinnamon 6.6.9 X11 (physical)

![m74: Tray menu](pictures/m74-tray-menu.png)
![m74: Settings](pictures/m74-settings-window.png)
![m74: #348: first open lands on Updates](pictures/m74-348-first-open.png)
![m74: #344: Channel offers Stable and Nightly](pictures/m74-344-channel.png)
![m74: Edit & upload](pictures/m74-editor.png)
![m74: Save a copy…: portal chooser](pictures/m74-save-copy-dialog.png)
![m74: Review window](pictures/m74-review.png)
![m74: F: the default browser loads the uploaded image](pictures/m74-F-browser.png)
![m74: #341: desktop capture holds the tray menu](pictures/m74-341-desktop-capture.png)
![m74: #341: window capture with the menu open](pictures/m74-341-window-capture.png)
![m74: #341: region gives up with a notice while the menu stays open](pictures/m74-341-region-notice.png)

### Ubuntu 26.04 GNOME 50.1 Wayland (VM on d69)

![ubuntu-vm: Tray menu](pictures/ubuntu-vm-tray-menu.png)
![ubuntu-vm: Settings](pictures/ubuntu-vm-settings-window.png)
![ubuntu-vm: #348: first open lands on Updates](pictures/ubuntu-vm-348-first-open.png)
![ubuntu-vm: #344: Channel offers Stable and Nightly](pictures/ubuntu-vm-344-channel.png)
![ubuntu-vm: Edit & upload](pictures/ubuntu-vm-editor.png)
![ubuntu-vm: Save a copy…: portal chooser](pictures/ubuntu-vm-save-copy-dialog.png)
![ubuntu-vm: Review window](pictures/ubuntu-vm-review.png)
![ubuntu-vm: F: the default browser loads the uploaded image](pictures/ubuntu-vm-F-browser.png)

### Aurora 44 Plasma 6.7.4 Wayland (VM on f44)

![aurora-vm: Tray menu](pictures/aurora-vm-tray-menu.png)
![aurora-vm: Settings](pictures/aurora-vm-settings-window.png)
![aurora-vm: #348: first open lands on Updates](pictures/aurora-vm-348-first-open.png)
![aurora-vm: #344: Channel offers Stable and Nightly](pictures/aurora-vm-344-channel.png)
![aurora-vm: Edit & upload](pictures/aurora-vm-editor.png)
![aurora-vm: Save a copy…: portal chooser](pictures/aurora-vm-save-copy-dialog.png)
![aurora-vm: Review window](pictures/aurora-vm-review.png)
![aurora-vm: F: the default browser loads the uploaded image](pictures/aurora-vm-F-browser.png)
![aurora-vm: #391: rail overlap at 1280×800](pictures/aurora-vm-391-rail-overlap.png)

## Things done on the hosts

All of this was approved by Patrick, through the coordinator.

- **f44:**
  - **Logouts:** two real ones. The first loaded the 942 GNOME extension (user copy); the second was item I. Both were `gnome-session-quit`, then a GDM restart for the autologin.
  - **Extension:** uninstalled at the end, and `enabled-extensions` was restored to `[background-logo, appindicatorsupport]`.
  - **F:** `profiles.ini`/`installs.ini` were changed for the test and restored byte for byte. The temporary profile and its new `Profile Groups/87c9febe.sqlite*` store were removed.
- **d69:**
  - **Before the pass:** Patrick's session-restored xterm (an idle bash) was closed by its PID.
  - **Logout:** one real logout for item I, with a temporary `/etc/sddm.conf.d/zz-afa2-relogin.conf` (`Relogin=true`), removed 6 s later.
  - **Wall server:** `w0vncserver.service` didn't come back after the logout, so it was started by hand.
  - **F:** the https default was changed and restored as described above.
  - **Harness mis-clicks:** one put the panel into edit mode, which I exited without changing anything. A second one hit the keyboard-layout indicator, which opened an emoji popup; it closed by itself.
- **k72:**
  - **Logout:** one real logout for item I, then an SDDM restart.
  - **Harness mis-clicks:** one put the panel into edit mode, which I exited without changing anything. A second opened `systemsettings kcm_notifications`, which I closed by its PID without changing anything.
  - **F:** the https default was changed and restored as described above.
- **m74:**
  - **Before the pass:** the Mint Welcome window was closed twice, by PID: once before, and once after it autostarted again on re-login.
  - **Logout:** one real logout for item I, then a LightDM restart.
  - **F:** `profiles.ini`/`installs.ini` were changed for the test and restored. The temporary profile, its store `Profile Groups/5f33ebb7.sqlite*` and `Crash Reports/glean` were removed; Firefox only appended to the pre-existing `crash_helper_server.log`.
- **Ubuntu VM:**
  - **Prep:** removed the golden's old put.gd DEB, added `gstreamer1.0-gtk3`, installed the 942 extension as a user copy, then a reboot through the session.
  - **Item I:** another reboot.
  - **F:** "Continue" on the first-run Firefox welcome screen, approved by Patrick.
- **Aurora VM:**
  - **Item I:** `sudo systemctl reboot`. The session reboot request did nothing on Plasma 6.7.
  - **Second overlay:** a fresh one (`aur-afa2b`) was booted only to retake the Channel picture, because the first click had missed the list.
- **Everywhere:**
  - **Locks:** all were released `--verified-clean`.
  - **Uinput agents:** stopped by the PIDs recorded at start.
  - **Run roots:** `~/work/put.gd-afa2` removed on all four builders.
  - **Overlays:** all three retired with the guarded cleanup. Tunnels were stopped, and only this pass's wall tiles were removed; another session's Windows tile, added meanwhile, stayed.
  - **Goldens:** both still hash the same: `563730cc…` (Ubuntu) and `6a2fb407…` (Aurora v3).
- **Not mine, left alone:** the long-running builder collectors (python, PIDs 2067, 4615, 978298 and 1956962), d69's signer `gpg-agent`, another session's Windows VM on d69, and a podman `conmon` on f44.
