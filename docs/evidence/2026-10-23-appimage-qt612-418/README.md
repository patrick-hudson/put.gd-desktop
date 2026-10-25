# AppImage on Qt 6.12.0 (#418): A–K re-acceptance on six desktops

[#418](https://github.com/patrick-hudson/put.gd-desktop/issues/418): the AppImage bundled Qt 6.8.3, which carries about 23 later Qt CVEs, some in the image parsers put.gd feeds user files to. Patrick decided to bump Qt. [#497](https://github.com/patrick-hudson/put.gd-desktop/pull/497) moves the AppImage to the official **Qt 6.12.0** binaries; the DEBs and RPM keep distro Qt. This is the [AppImage full-acceptance profile](../../acceptance-matrix.md#appimage-full-acceptance-profile) (A–K) on that build, recorded on 2026-10-23 by the appimage-qt-bump-418 session. It follows the [#397 pass](../2026-10-22-appimage-full-acceptance-rerun/README.md) and adds:
- an idle-memory comparison against Qt 6.8.3;
- malformed SVG/ICNS/TIFF files in History;
- the in-app update from the current nightly to this build through the lab feed.

## Builds

| Build | What | Identity |
| --- | --- | --- |
| **Under test** | the CI AppImage of #497's head `a272c922` (CI run [40000171082](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/40000171082), merge commit `6a325a72`, source tree `a0a449b2`) | `putgd-26.1023.1068-dev+g6a325a722afd-x86_64.AppImage`, 116,439,544 bytes, SHA-256 `4c1bfa9ecc18c4f474d6c35ca8a4719cf6cc30ee7dd119582f1e1c0f1368f280`, Qt 6.12.0 |
| Baseline (Qt 6.8.3) | the current nightly | [`v26.1023.1058-nightly`](https://github.com/patrick-hudson/put.gd-desktop/releases/tag/v26.1023.1058-nightly), AppImage `ce707fcf…`, checked against its `SHA256SUMS` |
| Lab nightly for J | the same tree plus two empty local lab commits, tagged and built like a real nightly in a pinned `ubuntu:24.04` container ([lab_build.sh](lab/lab_build.sh) `build-real`); published only under `pkg.put.gd/lab` | `v26.1023.1069-nightly`, AppImage `f253c1ec…` |
| Lab nightly for H | the same tree plus one empty lab commit, with the lab permalink as its update information ([lab_build.sh](lab/lab_build.sh) `build`) | `v26.1023.1068-nightly`, AppImage `7bbda043…` |

The CI image is untagged, so it doesn't check for updates (`development_build`). H's update parts (Channel, Check now, #344) and J therefore ran on the lab nightlies, which are built from the same tree. The commits are never pushed and carry only the version.

## Merged head re-check (rule 16)

After the pass, #497 merged `nightly` in (head `786ed9fc`, which brings #420's SBOM and the Qt version read from `tools.lock.json`). The CI AppImage of that head was checked again on 2026-10-23: CI run [40026118521](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/40026118521), `putgd-26.1023.1101-dev+gaa047473ad2e-x86_64.AppImage`, 116,472,312 bytes, SHA-256 `9918e7ea37126035fca7d957d8e8e174afb32261759597129c30abb7e2f8d9cb`, checked against the run's `SHA256SUMS`.

- **Same Qt as the accepted image** ([qt-bundle-compare.txt](results/merged-head/qt-bundle-compare.txt)):
  - **Qt files:** all 36 Qt libraries and plugins are byte-identical to those in `26.1023.1068-dev+g6a325a722afd`. The two images hold the same 607 files.
  - **Versions:** `libQt6Core` and `libQt6Gui` export `Qt_6.12` symbols, and the running app reports Qt 6.12.0.
  - **Plugins:** the platform plugins are `libqoffscreen`, `libqwayland` and `libqxcb`. The shell integrations are `xdg-shell` and `layer-shell`.
  - **Notices:** the image's `bundled-components.json` still lists 137 components. Apart from put.gd itself, only QtKeychain's label changed (`8eac9889775c` to `0.14.2`, #420's notices change).
  - **SBOM:** the CI SBOM lists qtbase, qtimageformats, qtsvg, qttranslations and qtwayland at 6.12.0.
- **On f44** (GNOME 50.0 Wayland; private profile, loopback fixture, wall VNC):
  - **Identity:** checksum, tag and root `.desktop` file pass.
  - **Region capture:** `completed`, 1120×680, cover check passed, History entry present.
  - **Idle RSS:** 29.88 MiB (PSS 23.58 MiB) after 180 s, with no flagged library.
  - **CVE file:** `cve-2025-10729-pattern-orphan.svg` went in through **Upload clipboard** as `image/svg+xml`. Its preview is `ready` and it renders in History. The resident and `putgd-ui` kept their PIDs.
  - **Results:** [f44-rck3](results/merged-head/f44-rck3/).
- **Harness, not product:** the first two runs fell over in the driver, and both are kept:
  - **Run 1** ([f44-rck1](results/merged-head/f44-rck1/), where the RSS above comes from): f44 has `wl-clipboard`, and GNOME has no data-control protocol. `wl-paste` therefore hung after the capture, and `wl-copy` offered text only, so the upload was a text file.
  - **Fix:** [afa.py](lab/afa.py) and [qt418.py](lab/qt418.py) now skip `wl-*` on GNOME.
  - **Run 2** ([f44-rck2](results/merged-head/f44-rck2/)): it passed but kept no frame. The SVG's rendered thumbnail hides the placeholder colour the card finder looks for, so run 3 names the newest card's row (`CARD_AT`).
  - **Driver:** [rck.sh](lab/rck.sh). Timeline: [timeline-f44.txt](results/merged-head/timeline-f44.txt).
- **Host changes on f44:**
  - **GNOME extension:** a user copy of 1058's extension was installed with one re-login and uninstalled afterwards. `enabled-extensions` was restored.
  - **Left alone:** the two old `0.1.1-dev.133` FUSE mounts.
  - **Lock:** released `--verified-clean` at 21:54:10Z.
- **Scanner findings on this head** (`vulnerability-report`, Grype):
  - **Cleared by Qt 6.12:** the Critical/High findings in Qt's embedded PCRE2, libpng, SQLite and libtiff that [#511](https://github.com/patrick-hudson/put.gd-desktop/issues/511) lists against Qt 6.8.3.
  - **Left in Qt's code:**
    - ICU 73.2 (CVE-2025-5222, fixed in 77.1): Qt 6.12.0 still ships ICU 73.2.
    - libtiff 4.7.2 (CVE-2026-4775): the scanner reported no fixed version. The
      [subsequent pinned-source assessment](../2026-10-24-release-blockers-511-416-419/README.md)
      confirms that Qt's source already contains the upstream correction.
  - **Tracking:** both are recorded on #511 with FFmpeg's.

![f44: merged head 786ed9fc: region capture and the CVE-2025-10729 SVG in History](pictures/f44-merged-head-recheck.png)

## Results

✅ pass · ❌ fail · 🔸 pass with a note (below the table)

| Item | f44 Fedora 44 GNOME 50.0 | d69 Debian 13 Plasma 6.3.6 | k72 Kubuntu 26.04 Plasma 6.6.6 | m74 Mint 22.3 Cinnamon 6.6.9 X11 | Ubuntu 26.04 GNOME 50.1 (VM) | Aurora 45 Plasma (VM) |
| --- | --- | --- | --- | --- | --- | --- |
| A. Identity | 🔸 | 🔸 | 🔸 | 🔸 | 🔸 | 🔸 |
| B. Start, tray, window icon | ✅ | ✅ | 🔸 | ✅ | ✅ | 🔸 |
| C. Idle memory (< 40 MiB) | ✅ 29.73 MiB | ✅ 30.88 MiB | ✅ 30.34 MiB | ✅ 30.28 MiB | ✅ 29.63 MiB | ✅ 30.84 MiB |
| D. Capture, Edit & upload, Save a copy… | ✅ | ✅ | ✅ | ✅ | 🔸 origin −1,−1 | 🔸 origin −1,−1 |
| E. Recording | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| F. Notification → real browser | ✅ | ✅ | ✅ | ✅ | ✅ | 🔸 |
| G. History | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Malformed SVG/ICNS/TIFF in History | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| H. Settings: theme, Updates, Check now, #344, #348, #349 | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| #341 shortcut with put.gd's tray menu open | ✅ | ✅ | ✅ | ✅ (+ window, notice) | ✅ | ✅ |
| I. Start at login | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| J. In-app update 1058 (Qt 6.8.3) → lab 1069 (Qt 6.12.0) | ✅ 11.1 s | ✅ 14.8 s | ✅ 8.6 s | ✅ 9.8 s | ✅ 8.7 s | ✅ 9.1 s |
| J. `appimageupdatetool --check-for-update` | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| K. Clean exit and cleanup | ✅ | ✅ | 🔸 | ✅ | ✅ | ✅ |

**Notes**

- **A:** the hash matches the CI artifact's own record, the tag matches the version, and the root holds exactly one `putgd.desktop`. An untagged CI image has no update information, so that sub-check is shown on the lab nightlies instead: `zsync|https://app.put.gd/dl/nightly/appimage.zsync` (1069) and the lab permalink (1068).
- **B, k72:** the tray finder first matched a pinned task-manager launcher and opened its context menu; nothing was chosen. The tray picture and #341 were redone from put.gd's own icon at (1553,1052), as #397 did.
- **B, Aurora:** after the clipboard uploads, Klipper's icon appeared beside put.gd's and the first #341 run clicked it (nothing was chosen in its menu). Those results are kept under `superseded-341-klipper/`, and #341 was redone from put.gd's own icon.
- **D, Ubuntu VM and Aurora:** the region starts one pixel up and left (−1,−1), the VNC input note from #397. The size is exact.
- **K, k72:**
  - After the re-login, the wall's `w0vncserver` failed while xdg-desktop-portal took about two minutes to start. Once the portal was up, a restart fixed it.
  - In the first frames afterwards, the put.gd window was black; that was the new VNC server, not put.gd. Spectacle showed the window drawn ([results/k72/runs/k72/](results/k72/runs/k72/)).
  - One K click hit the new screen-sharing indicator, whose **End** menu was dismissed with Escape without choosing anything. K then passed from put.gd's own icon.
- **E, Aurora:** `kde-screencast` with the bundled `x264enc`; motion 68.3 (MP4), 68.3 (WebM), 68.4 (GIF) and 76.6 (WebP), against a 0.0 static control. The guest has no GStreamer `gtksink`, so the cover ran with `AFA_COVER=gtk`.
- **F, Aurora:** in the first overlay Firefox opened the link, but its first-run Terms of Use screen covered the page, and nobody had approved accepting it. Patrick then allowed it: "Yes: the Qt bump session may click Continue (accepting Mozilla's Terms of Use) in a fresh disposable Aurora overlay's temporary Firefox profile, for item F only, and rerun F". F was rerun in a fresh overlay: Continue was clicked once in that temporary profile at `about:blank`, then the notification click loaded the image (`results/aurora-vm/runs/aurora-vm-F2/`).
- **J, appimageupdatetool:** on each 1068 lab image it follows the lab permalink, which names 1069 by then, so it answers "update available" (exit 1). The VM ran it while 1068 was still the newest and got exit 0. On f44 it needs `SSL_CERT_FILE` (#297).

## Recording (E) and the browser (F)

| Desktop | Acquisition | Live encoder | Motion MP4 / WebM / GIF / WebP | Static control | Browser that loaded the live upload (F) |
| --- | --- | --- | --- | --- | --- |
| f44 | `mutter-screencast` | bundled `x264enc` | 69.07 / 69.06 / 68.27 / 81.09 | 0.0 / 0.0 | Firefox 150 (RPM), temporary profile: `…/qt418/f44/Screen-Capture-2026-10-23-15-08-01.png` |
| d69 | `kde-screencast` | VA-API `vah264lpenc` | 67.84 / 67.77 / 68.17 / 80.22 | 0.0 / 0.0 | Firefox ESR `--profile` (temporary) as the https handler for the test: `…/qt418/d69/Screen-Capture-2026-10-23-14-14-46.png` |
| k72 | `kde-screencast` | VA-API `vah264lpenc` | 68.53 / 68.62 / 68.42 / 80.83 | 0.0 / 0.0 | snap Firefox `--profile` (temporary): `…/qt418/k72/Screen-Capture-2026-10-23-14-14-47.png` |
| m74 | X11 | VA-API `vah264lpenc` | 69.01 / 69.14 / 68.23 / 81.05 | 0.0 / 0.0 | Firefox (DEB), temporary profile: `…/qt418/m74/Screen-Capture-2026-10-23-14-11-25.png` |
| Ubuntu VM | `mutter-screencast` | bundled `x264enc` | 64.96 / 64.96 / 65.27 / 74.03 | 0.0 / 0.0 | snap Firefox, temporary profile: `…/qt418/ubuntu-vm/Screen-Capture-2026-10-23-14-46-41.png` |
| Aurora VM | `kde-screencast` | bundled `x264enc` | 68.3 / 68.3 / 68.4 / 76.6 | 0.0 / 0.0 | the Firefox Flatpak, temporary profile, in a second overlay (note F, Aurora): `…/qt418/aurora-vm/Screen-Capture-2026-10-23-16-11-45.png` |

Links are under `https://put.gd/acceptance/`. Each was removed afterwards with **Delete from destination** (403 after). The recorder's bar overlapping the Review window in some Review pictures also shows in 942's (Qt 6.8.3) pictures, so it predates this change.

## What Qt 6.12 changed in the AppImage

- **Platform plugins:** since Qt 6.10 a single `libqwayland.so` serves both the `wayland` and `wayland-egl` platform names. It replaces `libqwayland-generic.so`, `libqwayland-egl.so` and `libQt6WaylandEglClientHwIntegration.so`. `putgd-ui` runs on it natively on Plasma and GNOME, and on `libqxcb.so` on Cinnamon X11. The resident maps no platform plugin.
- **Image contents:** apart from that, the image's `usr/lib` and `usr/plugins` hold the same files as 1058's. It's 2.9 MB larger unpacked (291.7 vs 288.7 MB) and 116.4 MB as an AppImage.
- **LayerShellQt:** Qt 6.12's private Wayland headers declare `wl_fixes` from Qt's own `wayland.xml` (1.24). They share an include guard with Ubuntu 24.04's `wayland-client-protocol.h` (1.22), which then hides it, so LayerShellQt is now compiled with Qt's header included first. At run time Qt carries its own protocol code and needs only libwayland's core functions from the host.
- **Embedded third-party code** (from the installed binaries' SBOM):

  | Component | in 6.8.3 | in 6.12.0 |
  | --- | --- | --- |
  | libpng | 1.6.47 | 1.6.58 |
  | libjpeg-turbo | 3.1.0 | 3.2.0 |
  | libtiff | 4.7.0 | 4.7.2 |
  | libwebp | 1.5.0 | 1.6.0 |
  | SQLite | 3.49.1 | 3.53.4 |
  | ICU | 73.2 | 73.2 |

  The generated `bundled-components.json` and `runtime-notices.txt` name these versions.
- **Tests:** the full suite passes against Qt 6.12.0: 104/104 in a dev build on d69. Three test-only deprecations were fixed in a way that still builds against Qt 6.4.2. Product code shows no Qt deprecation warnings.

## Idle memory: Qt 6.8.3 against Qt 6.12.0

Each desktop ran the same driver four times on a quiet host, alternating 1058 and the PR build. Each run had a private profile and the loopback fixture, and was measured over 180 s idle after start with nothing opened ([mem.sh](lab/mem.sh)). f44's pair ran once the coordinator had paused the other sessions' compiles there.

| Desktop | Qt 6.8.3 RSS (MiB) | Qt 6.12.0 RSS (MiB) | Change | PSS 6.8.3 → 6.12 (MiB) |
| --- | --- | --- | --- | --- |
| f44 | 29.26, 29.22 | 29.94, 29.72 | +0.59 | 22.98 → 23.55 |
| d69 | 29.88, 30.03 | 30.75, 30.78 | +0.81 | 24.23 → 25.02 |
| k72 | 29.93, 30.00 | 30.20, 30.43 | +0.35 | 23.60 → 23.89 |
| m74 | 29.71, 29.64 | 30.30, 30.31 | +0.63 | 23.99 → 24.50 |

No GTK, KDE platform theme, `libssl`/`libcrypto` or `libQt6Network` was mapped in the resident on either version. The 40 MiB budget holds with about 9 MiB to spare. The growth is the larger Qt 6.12 libraries; no platform theme was added. Raw numbers: `results/<desktop>/mem-<desktop>.txt`.

## Malformed SVG, ICNS and TIFF files

The files ([make_samples.py](lab/make_samples.py), seeded) follow the advisories of fixed Qt CVEs, plus fuzz:

| File | What it is | Qt 6.8.3 | Qt 6.12.0 |
| --- | --- | --- | --- |
| `cve-2025-10729-pattern-orphan.svg` | a `<pattern>` outside a structural node (use-after-free) | **SIGSEGV** | decodes (64×64) |
| `cve-2026-8168-deep-nesting.svg` | 200,000 nested `<g>` | refused | refused |
| `cve-2025-10728-pattern-recursion.svg`, `cve-2026-6210-marker-confusion.svg` | pattern recursion; a marker naming a non-marker | refused / decodes | refused / decodes |
| `cve-2025-5683-icns-block-length.icns` and two more malformed ICNS | block lengths out of bounds | refused | refused |
| 24 seeded TIFF mutations, 8 seeded ICNS mutations | fuzz | no crash | no crash |

- **Without the app** ([decode.cpp](lab/decode.cpp), [rundecode.sh](lab/rundecode.sh), [results](results/decode-qt683-vs-qt612.txt)): each file goes through `QImageReader` with format detection, `size()`, then `read()` — what History does — in one process per file and version.
- **In the app, on every desktop** ([qt418.py](lab/qt418.py) `clipfile`):
  - Each file was copied as a file (`text/uri-list`) and uploaded with **Upload clipboard**. That's the realistic path for a file the user didn't make: it's stored as itself (`image/svg+xml`, `image/x-icns`, `image/tiff`).
  - Then `history.preview` let the resident's image worker decode it with every image plugin. A real click opened its History card, where `putgd-ui` reads the original's size with format detection.
  - The CVE-2025-10729 SVG renders. The deep-nesting SVG, the ICNS and the TIFF end in a clean `preview_failed` and "Preview unavailable". The resident and `putgd-ui` kept their PIDs throughout, on all six desktops.
- **An earlier attempt** swapped the bytes of existing captures. The resident refused those (`preview_source_missing`), because an artifact whose bytes no longer match its recorded SHA-256 isn't decoded at all. That's a defence in its own right, but it doesn't exercise the decoders, so the clipboard upload replaced it ([results/d69/runs/d69/superseded-badfile-1/](results/d69/runs/d69/)).

## In-app update: 1058 → this build through the lab feed

- **The run:** [labupd.sh](lab/labupd.sh) runs 1058 with the lab feed and mirror (`PUTGD_UPDATE_FEED_URL`, `…_MIRROR_URL` and `…_RELEASE_URL` under `https://pkg.put.gd/lab`) and the real appimageupdatetool (build 254, `8d17a50e…`).
- **Result on all six:** 1058 found `26.1023.1069-nightly` and installed it by zsync, reusing 86.8 MB and fetching 31.4 MB (the Qt libraries changed). After **Restart put.gd** it runs 1069 from the same path, Settings says "Updated from 26.1023.1058-nightly to 26.1023.1069-nightly with a zsync update", and the backup is removed.
- **A first lab build** embedded the lab permalink as its update information. 1058 refused it with `update_verification_failed` ("The new version follows other update information"). That's the installer's protection against an image from another channel, so it was correct.
- **The 1069 lab build** therefore carries the real nightly's update information. Its `.zsync` was rewritten to name the lab mirror copy, which the signer's lab path requires.

**Lab publication** (pre-approved by the coordinator):
- Both lab nightlies went through the package signer's lab scope ([lab-publish.sh](lab/lab-publish.sh), using #386's [lab_publish.py](../2026-10-22-linux-update-acceptance/lab/lab_publish.py)), with a dry run first. They're signed with the real nightly key, and only `lab/` keys were written.
- The lab stable key the tool requires was a throwaway, unused.
- After the pass every lab object was deleted and answers 404 (below).

## Things done on the hosts

- **Builders:**
  - **d69:** the Qt 6.12 dev build and test run. It needed `libsecret-1-dev` (with `libgcrypt20-dev`, `libgpg-error-dev`) for QtKeychain.
  - **k72:** `wl-clipboard`, for the clipboard upload.
  - **f44:** a podman `ubuntu:24.04` container (`putgd-qt418-applab`) for the lab builds.
- **Desktops:**
  - **Logouts:** one real logout per physical desktop for item I. f44 had a second one to load the GNOME extension, a user copy of 1058's (`integrations/gnome` is unchanged since 1058); it was uninstalled at the end and `enabled-extensions` restored.
  - **F's browser state:** d69 and k72 default to the ChatGPT app for https, so F set a temporary-profile Firefox as handler and restored `mimeapps.list` byte for byte (gio resolves `chatgpt.desktop` again). On f44 and m74 F made a temporary Firefox profile the default; `profiles.ini`/`installs.ini` were restored byte for byte and only the new `Profile Groups/*.sqlite*` files were removed.
  - **f44 Firefox root:** Firefox 150 keeps profiles in `~/.config/mozilla/firefox`. A first F attempt pointed at the absent `~/.mozilla/firefox` and created it; it was removed again.
  - **m74:** the Mint Welcome window that autostarted after the re-login was closed by its PID.
  - **Stray processes:** a stale 1058 AppImage runtime from the memory slot on k72 exited by itself. Two FUSE mounts of an old `0.1.1-dev.133` image on f44 aren't this pass's and were left alone.
- **Links and locks:**
  - **Link opening:** every click that could open a link ran under #415's guard: the stand-in recorder as the http(s) default, or, for F, the temporary-profile browser checked by `AFA_F_GUARD`. No link reached Patrick's browser profiles.
  - **Locks:** every lock was released `--verified-clean`.
- **Live S3:** `put.gd/acceptance/qt418/` listed 0 objects, 0 versions and 0 incomplete uploads after the last F. Key set 2 went from `~/Documents/creds.txt` to the resident's socket over a pipe and was never printed or stored.
- **VMs:**
  - **Ubuntu:** an overlay of `gnome-provisioned-20261016` on d69, retired by QMP quit with its overlay disk removed. The golden still hashes `563730cc…`. Its tunnel was stopped and only its tile removed.
  - **Aurora:** an overlay of the required F45 golden `aurora-testing-45.20261022.1-852d34e885c1` on f44 (4 GiB, 2 vCPUs, `uupd.timer` stopped), retired by QMP quit with its overlay disk removed. The golden still hashes `33fedf97…`. Its tunnel was stopped and only its tile removed. Item I used `sudo systemctl reboot` and autologin, as in #397. The other session's Aurora VM on f44 wasn't touched.

## Method

The drivers in [lab/](lab/) are #397's `afa.py` (the #415 guarded version), `rerun.py`, `i326.py` and `upd293.py`. This pass added:
- [qt418.py](lab/qt418.py) (`clipfile`) and [setclip.py](lab/setclip.py);
- [mem.sh](lab/mem.sh);
- [labupd.sh](lab/labupd.sh) and [labh.sh](lab/labh.sh);
- the real-browser F pair [fqt.sh](lab/fqt.sh) and [fqtb.sh](lab/fqtb.sh);
- [annotate418.py](lab/annotate418.py), for the pictures.

Each desktop ran `ak1.sh` (A–H through `ac.sh`/`dk.sh`), `clipfile`, `ak2a.sh` (#341) and `ak2b.sh` (I), then K, F, `labupd.sh` and `labh.sh`. Timelines with every lock, logout, closure and cleanup are in `results/<desktop>/timeline-*.txt`.

## Versions

The desktops are unchanged since #397: f44 Fedora 44 (6.19.10, GNOME Shell 50.0, gdm autologin, Wayland), d69 Debian 13 (6.12.107, Plasma/KWin 6.3.6, sddm, Wayland), k72 Kubuntu 26.04.1 (7.0.0-38, Plasma/KWin 6.6.6, sddm, Wayland), m74 Linux Mint 22.3 (6.14.0-37, Cinnamon 6.6.9, muffin 6.6.3, lightdm, X11) and the Ubuntu 26.04.1 GNOME 50.1 VM. All physical desktops use HD Graphics 530. Aurora testing-45.20261022.1 runs kernel 7.2.11-300.fc45, Plasma/KWin 6.7.5 and plasmalogin autologin on Wayland, with virtio-vga.

## Pictures

Every picture is a full-desktop frame from this pass. Each caption names the desktop and the exact build: **26.1023.1068-dev+g6a325a722afd** (the CI image of #497's head) unless it says lab nightly. Every frame was looked at before it was kept, and the physical desktops had none of Patrick's apps open.

### f44: Fedora 44 GNOME 50.0 Wayland (physical)

![f44: B: tray menu](pictures/f44-tray-menu.png)
![f44: B: Settings](pictures/f44-settings-window.png)
![f44: D: Edit & upload](pictures/f44-editor.png)
![f44: D: Save a copy… portal chooser](pictures/f44-save-copy-dialog.png)
![f44: E: Review window](pictures/f44-review.png)
![f44: CVE-2025-10729 SVG in History](pictures/f44-cve-svg.png)
![f44: F: the browser loads the live upload](pictures/f44-F-browser.png)
![f44: J: 1058 installs the lab nightly by zsync](pictures/f44-update-install-zsync.png)
![f44: J: 1069 runs after Restart put.gd](pictures/f44-update-restarted.png)
![f44: H (#344): Stable has no release yet](pictures/f44-344-stable.png)

### d69: Debian 13 Plasma 6.3.6 Wayland (physical)

![d69: B: tray menu](pictures/d69-tray-menu.png)
![d69: B: Settings](pictures/d69-settings-window.png)
![d69: D: Edit & upload](pictures/d69-editor.png)
![d69: D: Save a copy… portal chooser](pictures/d69-save-copy-dialog.png)
![d69: E: Review window](pictures/d69-review.png)
![d69: CVE-2025-10729 SVG in History](pictures/d69-cve-svg.png)
![d69: F: the browser loads the live upload](pictures/d69-F-browser.png)
![d69: J: 1058 installs the lab nightly by zsync](pictures/d69-update-install-zsync.png)
![d69: J: 1069 runs after Restart put.gd](pictures/d69-update-restarted.png)
![d69: H (#344): Stable has no release yet](pictures/d69-344-stable.png)

### k72: Kubuntu 26.04 Plasma 6.6.6 Wayland (physical)

![k72: B: tray menu](pictures/k72-tray-menu.png)
![k72: B: Settings](pictures/k72-settings-window.png)
![k72: D: Edit & upload](pictures/k72-editor.png)
![k72: D: Save a copy… portal chooser](pictures/k72-save-copy-dialog.png)
![k72: E: Review window](pictures/k72-review.png)
![k72: CVE-2025-10729 SVG in History](pictures/k72-cve-svg.png)
![k72: F: the browser loads the live upload](pictures/k72-F-browser.png)
![k72: J: 1058 installs the lab nightly by zsync](pictures/k72-update-install-zsync.png)
![k72: J: 1069 runs after Restart put.gd](pictures/k72-update-restarted.png)
![k72: H (#344): Stable has no release yet](pictures/k72-344-stable.png)

### m74: Linux Mint 22.3 Cinnamon 6.6.9 X11 (physical)

![m74: B: tray menu](pictures/m74-tray-menu.png)
![m74: B: Settings](pictures/m74-settings-window.png)
![m74: D: Edit & upload](pictures/m74-editor.png)
![m74: D: Save a copy… portal chooser](pictures/m74-save-copy-dialog.png)
![m74: E: Review window](pictures/m74-review.png)
![m74: CVE-2025-10729 SVG in History](pictures/m74-cve-svg.png)
![m74: F: the browser loads the live upload](pictures/m74-F-browser.png)
![m74: J: 1058 installs the lab nightly by zsync](pictures/m74-update-install-zsync.png)
![m74: J: 1069 runs after Restart put.gd](pictures/m74-update-restarted.png)
![m74: H (#344): Stable has no release yet](pictures/m74-344-stable.png)

### Ubuntu 26.04 GNOME 50.1 Wayland (VM on d69)

![ubuntu-vm: B: tray menu](pictures/ubuntu-vm-tray-menu.png)
![ubuntu-vm: B: Settings](pictures/ubuntu-vm-settings-window.png)
![ubuntu-vm: D: Edit & upload](pictures/ubuntu-vm-editor.png)
![ubuntu-vm: D: Save a copy… portal chooser](pictures/ubuntu-vm-save-copy-dialog.png)
![ubuntu-vm: E: Review window](pictures/ubuntu-vm-review.png)
![ubuntu-vm: CVE-2025-10729 SVG in History](pictures/ubuntu-vm-cve-svg.png)
![ubuntu-vm: F: the browser loads the live upload](pictures/ubuntu-vm-F-browser.png)
![ubuntu-vm: J: 1058 installs the lab nightly by zsync](pictures/ubuntu-vm-update-install-zsync.png)
![ubuntu-vm: J: 1069 runs after Restart put.gd](pictures/ubuntu-vm-update-restarted.png)
![ubuntu-vm: H (#344): Stable has no release yet](pictures/ubuntu-vm-344-stable.png)

### Aurora 45 Plasma 6.7.5 Wayland (VM on f44)

![aurora-vm: B: tray menu](pictures/aurora-vm-tray-menu.png)
![aurora-vm: B: Settings](pictures/aurora-vm-settings-window.png)
![aurora-vm: D: Edit & upload](pictures/aurora-vm-editor.png)
![aurora-vm: D: Save a copy… portal chooser](pictures/aurora-vm-save-copy-dialog.png)
![aurora-vm: E: Review window](pictures/aurora-vm-review.png)
![aurora-vm: CVE-2025-10729 SVG in History](pictures/aurora-vm-cve-svg.png)
![aurora-vm: F: the browser loads the live upload](pictures/aurora-vm-F-browser.png)
![aurora-vm: J: 1058 installs the lab nightly by zsync](pictures/aurora-vm-update-install-zsync.png)
![aurora-vm: J: 1069 runs after Restart put.gd](pictures/aurora-vm-update-restarted.png)
![aurora-vm: H (#344): Stable has no release yet](pictures/aurora-vm-344-stable.png)

