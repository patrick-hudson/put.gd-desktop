# Native package full acceptance, nightly 26.1023.1058 (2026-10-23)

This is one pass of the [native package full-acceptance profile](../../acceptance-matrix.md#native-package-full-acceptance-profile) (items A–Q). It used the DEBs and the RPM of [`v26.1023.1058-nightly`](https://github.com/patrick-hudson/put.gd-desktop/releases/tag/v26.1023.1058-nightly) (`05991d9d`) on five desktops. It covers the package lifecycle of [#375](https://github.com/patrick-hudson/put.gd-desktop/issues/375) (DEB) and [#377](https://github.com/patrick-hudson/put.gd-desktop/issues/377) (RPM), and the observations Patrick needs for [#382](https://github.com/patrick-hudson/put.gd-desktop/issues/382).

**Result:** every item passes on every desktop, except P on m74. That failure is a product bug, [#490](https://github.com/patrick-hudson/put.gd-desktop/issues/490): on Mint Cinnamon the login-started resident survives logout, and the next login reuses it. Five smaller findings are filed below.

**The build:**
- **Assets:** every package matched the release's `SHA256SUMS`. The `pkg.put.gd/releases/<tag>/SHA256SUMS` mirror copy is identical and its `.minisig` verifies.
- **Live consistency:** the update profile's U1 check passed all 33 checks at about 04:15Z ([results/u1-1058-start.json](results/u1-1058-start.json)). The feeds, the apt and dnf repositories and the AppStream catalogs all named 1058.
- **Nightly freeze:** the coordinator held nightly dispatches until items L and M2 were done everywhere, at 05:57:52Z. That kept 1058 the newest version in the repositories, as L and M2 require.

## Desktops

| Desktop | Package | Placement | Session | Versions |
| --- | --- | --- | --- | --- |
| Ubuntu 26.04.1 GNOME 50.1 | `ubuntu26.04` DEB | VM: overlay of `gnome-provisioned-20261016` on d69, retired afterwards (golden `563730cc…` unchanged) | Wayland, gdm autologin | Qt 6.10 |
| Linux Mint 22.3 Cinnamon 6.6.9 | `ubuntu24.04` DEB | physical m74 | X11 (muffin 6.6.3), lightdm autologin | Qt 6.4.2 |
| Kubuntu 26.04.1 Plasma 6.6.6 | `ubuntu26.04` DEB | physical k72 | Wayland (kwin 6.6.6), plus its Plasma X11 session for E's X11 half | Qt 6.10.2 |
| Debian 13 Plasma 6.3.6 | `debian13` DEB | physical d69 | Wayland (kwin 6.3.6), sddm autologin | Qt 6.8.2 |
| Fedora 44 GNOME 50.0 | `fedora44` RPM | physical f44 | Wayland (mutter 50.0), gdm autologin | Qt 6.11.2 |

## Results

✅ pass, ❌ fail, ➖ doesn't apply. Every cell is build `26.1023.1058-nightly` (`05991d9d`). The run names are the folders under [results/](results/).

| Item | Ubuntu GNOME VM | m74 Mint Cinnamon | k72 Kubuntu Plasma | d69 Debian Plasma | f44 Fedora GNOME |
| --- | --- | --- | --- | --- | --- |
| **A** identity | ✅ `26.1023.1058~nightly`, CalVer = commit date 26.1023; repository package = SHA256SUMS `a06b63f9…`; `dpkg --verify` clean; AppStream valid | ✅ `f9b01cd2…`; the same checks | ✅ `a06b63f9…`; the same checks | ✅ `d56909f1…`; the same checks | ✅ `26.1023.1058~nightly-1.fc44`; payload = release (pkg.put.gd adds only the signer's RPM signature); `rpm -V` clean |
| **B** start, tray, icon | ✅ GNOME overview search starts it; tray menu by real click; dock and switcher icon | ✅ Mint menu; resident 0.34 s; `putgd-ui` on xcb | ✅ Kickoff; 0.36 s; icon in title bar, taskbar, switcher | ✅ Kickoff; 0.35 s; `putgd-ui` on Wayland | ✅ GNOME overview; tray and switcher |
| **C** idle memory (180 s) | ✅ RSS 31.26 MiB, PSS 20.34 | ✅ 28.44 / 18.08, load ~1.0 | ✅ 31.97 / 9.0, load 0.44 | ✅ 30.7 / 9.1, load ~0.9 (another session's Windows guest) | ✅ 35.25 / 21.45; libcrypto mapped through Fedora's Qt6Core/qt6keychain, as the update pass recorded |
| **D** capture, Edit & upload, Save a copy | ✅ region, window, desktop; redaction verified; GNOME portal chooser | ✅ all three; redaction; portal chooser | ✅ all three; redaction; KDE portal chooser | ✅ all three; redaction; KDE portal chooser | ✅ all three; redaction; GNOME portal chooser |
| **E** recording (motion mp4/webm/gif/webp, static control 0.0) | ✅ mutter-screencast, x264enc: 67.1 / 67.0 / 66.9 / – | ✅ ximagesrc, VA-API `vah264lpenc`: 68.4 / 68.5 / 69.1 / 81.0 | ✅ kde-screencast, `vah264lpenc`: 67.3 / 67.3 / 66.1 / 80.3. X11 session: ximagesrc, `vah264lpenc`: 68.0 / 68.2 / 68.5 / 80.6 | ✅ kde-screencast, `vah264lpenc`: 68.0 / 68.2 / 67.3 / 81.8 | ✅ mutter-screencast, openh264enc: 68.6 / 68.5 / 68.0 / 80.6. `h264_encoder_unusable` shown with the `noopenh264` placeholder (see deviations) |
| **F** link-ready notification → real browser | ✅ guest Firefox loaded `https://put.gd/acceptance/nfa/ubu-vm/…`; prefix then empty | ✅ temporary Firefox profile; prefix 0 objects, 0 versions | ✅ temporary Firefox handler (the default is the ChatGPT app); prefix empty | ✅ temporary Firefox ESR handler; prefix empty | ✅ temporary Firefox profile; prefix empty |
| **G** history | ✅ previews incl. video poster; Copy image; Delete from destination → 404, row kept | ✅ | ✅ | ✅ | ✅ |
| **H** Settings, Updates | ✅ theme live; Channel read-only "Set by your system's put.gd repository"; Up to date; no notification. Open software updates finds no updater it knows (#486) | ✅ the same; Copy gives the documented apt commands; Open software updates starts Update Manager | ✅ the same; Copy = docs; Discover opens | ✅ the same; Copy = docs; GNOME Software opens | ✅ the same with the dnf commands; GNOME Software opens in front |
| **I** start at login | ✅ entry `Exec="/usr/bin/putgd-launcher" --background`; started 4 s after gnome-shell | ✅ started 2 s after cinnamon | ✅ 1 s after plasmashell | ✅ 1 s after plasmashell, `app-putgd@autostart.service` | ✅ 2 s after gnome-shell, `app-gnome-putgd-*.scope` |
| **J** package manager view | ✅ InRelease verified; installed = pkg.put.gd candidate | ✅ | ✅ | ✅ | ✅ `repomd.xml` verified; nothing newer |
| **K** quit from the tray | ✅ no put.gd process left | ✅ (second try, after a driver misclick; see notes) | ✅ (second try, the same) | ✅ | ✅ |
| **L** fresh install from pkg.put.gd | ✅ via file install, `remove` (keeps the source), `apt install putgd` (guest sudo allows only apt-get) | ✅ repo file + key from the package bytes, `apt-get install putgd` from the pool; no prompt | ✅ the same | ✅ the same | ✅ `dnf install putgd` from pkg.put.gd; Recommends openh264 already installed |
| **M1** dev.133 (schema 5, 10,000 entries) → 1058 | ✅ seeded by the dev.133 AppImage (its DEB can't capture on GNOME 50.1). #56 move `migrated`; `user_version` 5→7; integrity ok; rows and counts preserved; config byte-identical; outputs identical | ✅ dev.133 DEB seeds on Mint; the same results; custom shortcut kept | ✅ dev.133 DEB seeds on Plasma; the same; start 0.33 s, list 0.02 s | ✅ seeded by the dev.133 AppImage (no `debian13` DEB then); the same | ✅ seeded by the dev.133 AppImage (its RPM no longer installs on F44); the same |
| **M2** 947 → 1058 through pkg.put.gd, resident running | ✅ ordering ok; no prompt; source file unchanged; config identical; entries, previews, links, theme kept | ✅ the same | ✅ the same | ✅ the same | ✅ `dnf upgrade --refresh putgd`; `putgd.repo` byte-identical |
| **O** companion | ✅ dev.133's extension 1 under 1058 (2): `helper.outdated`, screenshots work, interactive recording refused with `helper_outdated`; fixed by a re-login; disable → captures fail (#493) | ➖ no Cinnamon applet in the package | ✅ installed entries authorize ScreenShot2 and screencast with no prompt; gone after purge | ✅ the same | ✅ as on the VM |
| **P** removal, then a real login | ✅ `remove` keeps `putgd.sources`; `purge` leaves 0 package files; no process after login; user data kept | ❌ files and repository removed, data kept, but a resident from before survived the logouts ([#490](https://github.com/patrick-hudson/put.gd-desktop/issues/490)) | ✅ | ✅ | ✅ `dnf remove`: `putgd.repo` and all `RPM-GPG-KEY-putgd*` gone |
| **Q** the login entry after uninstall (#382) | 3 journal lines per login (systemd generator ×2, gnome-session); with TryExec: 1 (gnome-session) | 1 cinnamon-session warning per login; listed, enabled, in Startup Applications; TryExec changes nothing | 2 generator lines; Autostart lists "Not autostarted yet"; with TryExec: 0 lines and gone from Autostart | 2 generator lines; with TryExec: 0 | 1 gnome-session line; TryExec changes nothing |

## #382: what the retained entry does

After the package is removed, `~/.config/autostart/putgd.desktop` stays. It names a missing `/usr/bin/putgd-launcher`. On every desktop it starts nothing: no process, no failed unit and no notification. What it leaves behind:

| Desktop | Entry as written today | With `TryExec=/usr/bin/putgd-launcher` |
| --- | --- | --- |
| GNOME 50 (f44) | 1 gnome-session line per login | the same 1 line |
| GNOME 50.1 (Ubuntu VM) | 3 lines: systemd-xdg-autostart-generator ×2, gnome-session ×1 | 1 line (gnome-session) |
| Plasma 6.6 (k72) | 2 generator lines; Autostart settings list "put.gd — Not autostarted yet" | 0 lines; not listed |
| Plasma 6.3 (d69) | 2 generator lines | 0 lines |
| Cinnamon 6.6 (m74) | 1 cinnamon-session warning; Startup Applications lists it, enabled | the same |

- **TryExec:** silences systemd's generator and Plasma's settings. gnome-session and cinnamon-session still log one line per login.
- **Accepting the entry as it is:** one to three log lines per login, plus a dangling entry in Plasma's and Cinnamon's startup settings, until the user deletes the file or turns off **Launch at login** before uninstalling.
- **Reinstalling** brings the entry back to life either way.
- Patrick's decision is still open; these observations are on #382.

## Findings

Filed from this pass:
- [#490](https://github.com/patrick-hudson/put.gd-desktop/issues/490) (P1, release-blocker): Mint Cinnamon. The login-started resident survives logout, and the next login hands off to it; its captures fail with `capture_unavailable`.
- [#491](https://github.com/patrick-hudson/put.gd-desktop/issues/491): a repository install shows Channel as an editable choice until the first update check. Seen on the VM and m74.
- [#492](https://github.com/patrick-hudson/put.gd-desktop/issues/492): GNOME. `gnome-extensions enable putgd@put.gd` fails right after installing the package; the documented order needs a re-login first.
- [#493](https://github.com/patrick-hudson/put.gd-desktop/issues/493): GNOME. With the extension disabled, captures fail with a message that doesn't say to enable it.
- [#494](https://github.com/patrick-hudson/put.gd-desktop/issues/494): History says "1 captures".
- [#495](https://github.com/patrick-hudson/put.gd-desktop/issues/495) (lab): the tray finder clicked the wrong panel item; plus the sycoca, resident-survives-logout and keyring traps below.

Already filed elsewhere:
- [#466](https://github.com/patrick-hudson/put.gd-desktop/issues/466): the README's "fetch the newest nightly" command picks `v0.1.1-nightly.947`.
- #486 (from the update pass): Ubuntu 26.04's Software Updater isn't recognised by **Open software updates**.

Old builds only, not filed:
- dev.133's GNOME extension rejects Qt 6.10's image MIME list on Ubuntu 26.04 ("Invalid supported image MIME list").
- dev.133's RPM no longer installs on Fedora 44 (`qt6-qtbase-sqlite`).

## Deviations and how each was handled

- **f44 OpenH264:** `dnf swap openh264 noopenh264` would also have removed `mozilla-openh264`, which the approval didn't cover. Instead the resident alone loaded the extracted `noopenh264` library (`LD_LIBRARY_PATH`); no host package changed. That gave the actionable `h264_encoder_unusable` message, and recording worked again afterwards.
- **Keychain:** none of the desktops had an unlocked keychain after autologin. f44 has no login keyring since the 2026-10-20 home wipe; k72's kdewallet and the Secret Service default collection on d69, m74 and the VM are locked. M1 therefore used session credentials for the uploads, plus a second profile whose keychain reference survives the migration (it reports `credential_setup_required`, as expected).
- **The 10,000-entry history:** it is the real dev.133 rows copied with fresh ids, sharing their 5 operations. History therefore groups them into one capture of 2,000 files, which is the seed's shape, not the product's.
- **VM:**
  - d69's key was typed into the guest over VNC ([#487](https://github.com/patrick-hudson/put.gd-desktop/issues/487)).
  - The guest was reset over QMP twice: once after the rehearsal's runtime-directory bug (below), once to clear stuck keyring prompts.
  - `lab.py cleanup` refused the run because it wasn't under the canonical lab-runs folder. QEMU was stopped and the overlay disk removed by hand; the golden is unchanged.
- **Driver bug caught in the rehearsal:** a default-profile run stored `/run/user/UID` as its runtime folder, and teardown deleted it in the VM. It was fixed (and guarded) before any physical desktop.
- **Misclicks** ([#495](https://github.com/patrick-hudson/put.gd-desktop/issues/495)): the tray finder matched a taskbar launcher.
  - On k72, quit-tray entered Plasma's panel edit mode; it was exited, and `plasma-org.kde.plasma.desktop-appletsrc` is unchanged.
  - On m74 it chose Cinnamon's "Add a new panel"; that was cancelled, and `panels-enabled` is unchanged.
- **d69's application cache** lacked every application until `kbuildsycoca6` ran once. Until then Kickoff couldn't find put.gd and KWin refused capture.
- **k72's portal** failed to start at one login, because the KDE backend's activation timed out while drkonqi processed old AppImage core dumps. The user services were restarted.

## Reuse of the Linux update pass

The [update pass](../2026-10-22-linux-update-acceptance/README.md) (PR #489) proved the apt and dnf N → N+1 routes, the GUI updaters, the channel switch, discovery, notifications and RSS on `26.1022.1017` → `26.1022.1019`. This pass doesn't repeat those items. Their cells name a different build, so they're cited only as the routes' evidence. Everything visible here was pictured again from 1058.

## Timelines

UTC times. The full timelines, with every closed window PID, logout and install, are in `results/<host>/timeline-<host>.txt`.

| Desktop | Lock | L done | M2 done | Logouts | Released |
| --- | --- | --- | --- | --- | --- |
| Ubuntu VM (d69) | none (VM); overlay 04:16–05:28 | 04:33 | 05:05 | 8 guest reboots | 05:28 overlay retired |
| m74 | 05:30:18 | 05:31:51 | 05:52:31 | 6 (relogin.sh) | 05:59:54 `--verified-clean` |
| k72 | 05:30:18 | 05:31:56 | 05:53:27 | I, P, TryExec, X11 there and back | 06:02:40 `--verified-clean` |
| d69 | 05:30:17 | 05:35:04 | 05:57:52 | I, P, TryExec | 06:03:10 `--verified-clean` |
| f44 | 05:30:19 | ~05:33 (after the extension re-login) | 05:55:42 | I ×2, O re-login, P, TryExec | 06:00:35 `--verified-clean` |

**Privacy:**
- Before any full-desktop frame, the windows of Patrick's that were open were closed by exact PID: Firefox on f44 (started by GNOME's search provider), and Mint Welcome and Update Manager on m74.
- Every kept frame was looked at.
- No link reached a browser profile of Patrick's: the stand-in recorder or a temporary Firefox was the handler for every click.
- `mimeapps.list`, `kde-mimeapps.list`, `profiles.ini` and `installs.ini` were restored byte for byte.

**Cleanup:** on every host, `state-end` equals `state-0`: no package, no repository file or key, no default profile, no autostart entry and no process. Older `~/.local/state/putgd` logs that predate this pass were left as found. The live prefix `put.gd/acceptance/nfa/` listed 0 objects and 0 versions on all five desktops.

## Method

- **Drivers:** [lab/](lab/README.md).
  - [nfa.py](lab/nfa.py) runs #415's guarded [afa.py](../2026-10-22-rail-391-notify-415/lab/afa.py) against the installed `/usr/bin/putgd`, and adds the lifecycle subcommands.
  - [nk.sh](lab/nk.sh) runs B and D–H.
  - [annotate.py](lab/annotate.py) draws the pictures.
- **Residents:** they start as `systemd-run --user` units, so the updater they open gets polkit's session rules. A lab resident started this way survives a logout while SSH keeps the user manager alive, so it's quit before every login test.
- **Packages:** package commands go only through `nfa.py pkg`. It simulates the transaction first and refuses anything beyond `putgd`; no other package was installed, upgraded or removed.
- **Uploads:** to the loopback S3 fixture, except F's object. That went to the live test prefix `put.gd/acceptance/nfa/<desktop>/` with key set 2, read on the builder and passed over the session socket only.
- **Results:** under [results/](results/), one folder per desktop. The third-party repository lines of the builders' package logs were removed.

## Pictures

Each picture's footer names the build (`26.1023.1058-nightly`, `05991d9d`) and the desktop.

### Ubuntu 26.04 GNOME Wayland (VM)

| Picture | Shows |
| --- | --- |
| [ubu-B-switcher.png](pictures/ubu-B-switcher.png) | B: the put.gd icon in the Alt+Tab switcher and the dock |
| [ubu-B-tray-menu.png](pictures/ubu-B-tray-menu.png) | B: the tray menu opened with a real click on the AppIndicator icon |
| [ubu-D-save-a-copy.png](pictures/ubu-D-save-a-copy.png) | D: Edit & upload -> Save a copy... opens GNOME's portal file chooser (backend: portal); the copy was saved |
| [ubu-E-review.png](pictures/ubu-E-review.png) | E: the Review window after a mutter-screencast recording (x264enc): trim 1.00-12.73 s and a timed blur |
| [ubu-F-browser.png](pictures/ubu-F-browser.png) | F: a real click on the link-ready notification opened the guest's Firefox, which loaded the live public upload |
| [ubu-H-updates.png](pictures/ubu-H-updates.png) | H: Settings -> Updates on the repository install: Channel read-only, set by the apt repository file; Up to date after Check now |
| [ubu-L-first-run.png](pictures/ubu-L-first-run.png) | L: first start from the GNOME overview search after apt installed put.gd from pkg.put.gd; tray icon in the panel |
| [ubu-O-helper-outdated.png](pictures/ubu-O-helper-outdated.png) | O: dev.133's extension (1) still loaded after the upgrade to 1058 (2): a region recording is refused with helper_outdated |
| [ubu-finding-channel-before-first-check.png](pictures/ubu-finding-channel-before-first-check.png) | Finding: before the first update check, a repository install shows Channel as an editable choice (docs: no choice) |

### m74 Linux Mint 22.3 Cinnamon X11

| Picture | Shows |
| --- | --- |
| [m74-B-tray-menu.png](pictures/m74-B-tray-menu.png) | B: the tray menu opened with a real click |
| [m74-D-save-a-copy.png](pictures/m74-D-save-a-copy.png) | D: Edit & upload -> Save a copy... opens the desktop's portal file chooser |
| [m74-E-review.png](pictures/m74-E-review.png) | E: Review window after recording a moving pattern: trim and a timed blur |
| [m74-F-browser.png](pictures/m74-F-browser.png) | F: a real click on the link-ready notification opened Firefox (temporary profile), which loaded the live public upload on put.gd |
| [m74-H-updater.png](pictures/m74-H-updater.png) | H: Open software updates starts the system's updater |
| [m74-H-updates.png](pictures/m74-H-updates.png) | H: Settings -> Updates on a repository install: Channel read-only, set by the repository file; Other channel lists the documented commands |
| [m74-I-after-login.png](pictures/m74-I-after-login.png) | I: Launch at login: after a real logout and login put.gd runs from /usr/bin/putgd |
| [m74-L-first-run.png](pictures/m74-L-first-run.png) | L: first start on a fresh profile; the window opens and the tray icon appears |
| [m74-L-menu-search.png](pictures/m74-L-menu-search.png) | L: put.gd found in the desktop's application menu after apt/dnf installed it from pkg.put.gd; Return starts it |
| [m74-Q-startup-applications.png](pictures/m74-Q-startup-applications.png) | Q (#382): after uninstall, Cinnamon's Startup Applications still lists the put.gd entry |
| [m74-X-stale-resident.png](pictures/m74-X-stale-resident.png) | #490: on Mint the resident from the previous session survived logout; the new login reused it ('Not checked yet', captures fail) |
| [m74-finding-491-channel.png](pictures/m74-finding-491-channel.png) | #491: before the first update check a repository install shows Channel as an editable choice |

### k72 Kubuntu 26.04 Plasma Wayland

| Picture | Shows |
| --- | --- |
| [k72-D-save-a-copy.png](pictures/k72-D-save-a-copy.png) | D: Edit & upload -> Save a copy... opens the desktop's portal file chooser |
| [k72-E-review-x11.png](pictures/k72-E-review-x11.png) | E (Plasma X11 session): Review window after recording a moving pattern: trim and a timed blur |
| [k72-E-review.png](pictures/k72-E-review.png) | E: Review window after recording a moving pattern: trim and a timed blur |
| [k72-F-browser.png](pictures/k72-F-browser.png) | F: a real click on the link-ready notification opened Firefox (temporary profile), which loaded the live public upload on put.gd |
| [k72-H-updates.png](pictures/k72-H-updates.png) | H: Settings -> Updates on a repository install: Channel read-only, set by the repository file; Other channel lists the documented commands |
| [k72-I-after-login.png](pictures/k72-I-after-login.png) | I: Launch at login: after a real logout and login put.gd runs from /usr/bin/putgd |
| [k72-L-first-run.png](pictures/k72-L-first-run.png) | L: first start on a fresh profile; the window opens and the tray icon appears |
| [k72-L-kickoff-search.png](pictures/k72-L-kickoff-search.png) | L: put.gd found in the desktop's application menu after apt/dnf installed it from pkg.put.gd; Return starts it |
| [k72-M1-history-10k.png](pictures/k72-M1-history-10k.png) | M1: History opens the 10,000-row history migrated from dev.133 (schema 5 -> 7). Seed rows share 5 operations, so a page shows one capture |
| [k72-Q-autostart-kcm-tryexec.png](pictures/k72-Q-autostart-kcm-tryexec.png) | Q (#382) TryExec probe: with TryExec the entry no longer appears in Plasma's Autostart |
| [k72-Q-autostart-kcm.png](pictures/k72-Q-autostart-kcm.png) | Q (#382): after uninstall, Plasma's Autostart lists 'put.gd - Not autostarted yet' |

### d69 Debian 13 Plasma Wayland

| Picture | Shows |
| --- | --- |
| [d69-B-tray-menu.png](pictures/d69-B-tray-menu.png) | B: the tray menu opened with a real click |
| [d69-D-save-a-copy.png](pictures/d69-D-save-a-copy.png) | D: Edit & upload -> Save a copy... opens the desktop's portal file chooser |
| [d69-E-review.png](pictures/d69-E-review.png) | E: Review window after recording a moving pattern: trim and a timed blur |
| [d69-F-browser.png](pictures/d69-F-browser.png) | F: a real click on the link-ready notification opened Firefox (temporary profile), which loaded the live public upload on put.gd |
| [d69-H-updates.png](pictures/d69-H-updates.png) | H: Settings -> Updates on a repository install: Channel read-only, set by the repository file; Other channel lists the documented commands |
| [d69-I-after-login.png](pictures/d69-I-after-login.png) | I: Launch at login: after a real logout and login put.gd runs from /usr/bin/putgd |
| [d69-L-first-run.png](pictures/d69-L-first-run.png) | L: first start on a fresh profile; the window opens and the tray icon appears |
| [d69-L-menu-search.png](pictures/d69-L-menu-search.png) | L: put.gd found in the desktop's application menu after apt/dnf installed it from pkg.put.gd; Return starts it |

### f44 Fedora 44 GNOME Wayland

| Picture | Shows |
| --- | --- |
| [f44-B-tray-menu.png](pictures/f44-B-tray-menu.png) | B: the tray menu opened with a real click |
| [f44-D-save-a-copy.png](pictures/f44-D-save-a-copy.png) | D: Edit & upload -> Save a copy... opens the desktop's portal file chooser |
| [f44-E-h264-unusable.png](pictures/f44-E-h264-unusable.png) | E: with the noopenh264 placeholder, recording reports h264_encoder_unusable with the actionable message |
| [f44-E-review.png](pictures/f44-E-review.png) | E: Review window after recording a moving pattern: trim and a timed blur |
| [f44-F-browser.png](pictures/f44-F-browser.png) | F: a real click on the link-ready notification opened Firefox (temporary profile), which loaded the live public upload on put.gd |
| [f44-H-updates.png](pictures/f44-H-updates.png) | H: Settings -> Updates on a repository install: Channel read-only, set by the repository file; Other channel lists the documented commands |
| [f44-I-after-login.png](pictures/f44-I-after-login.png) | I: Launch at login: after a real logout and login put.gd runs from /usr/bin/putgd |
| [f44-L-first-run.png](pictures/f44-L-first-run.png) | L: first start on a fresh profile; the window opens and the tray icon appears |
| [f44-L-menu-search.png](pictures/f44-L-menu-search.png) | L: put.gd found in the desktop's application menu after apt/dnf installed it from pkg.put.gd; Return starts it |
| [f44-O-helper-outdated.png](pictures/f44-O-helper-outdated.png) | O: dev.133's GNOME extension (1) still loaded after the upgrade to 1058 (2): recording is refused with helper_outdated until a re-login |

Not pictured, with the reason:
- k72 B: the kept tray frame shows the task manager's menu, not put.gd's; the tray menu was checked over D-Bus and by a real right-click (results).
- The updater windows on k72, d69 and f44: their update lists show another app of Patrick's, so only m74's Update Manager is pictured; each updater's start is in the results.
- M1 History on m74, d69 and f44: the same view as k72's (one capture per page, by the seed's shape); the migration itself is shown by the data checks in the results.
