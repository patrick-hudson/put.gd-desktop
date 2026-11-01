# Linux acceptance coverage and execution placement

The required Linux test distributions are **Linux Mint, Ubuntu, Kubuntu, Fedora
and Debian**. Cover Cinnamon plus GNOME and KDE/Plasma, and real X11 and Wayland
sessions. Use a combination of the physical build servers and VMs that provides
this coverage on the exact candidate being accepted. This document sets required
coverage; it does not establish that any row has passed.

## Distribution coverage

| Distribution | Desktop/session baseline | Preferred execution placement |
| --- | --- | --- |
| Linux Mint | Cinnamon/X11 | Physical Mint 22.3 Cinnamon/X11 server `.74` (m74) for gates requiring physical Cinnamon; fresh VM on a capable build server for other versions; local VM fallback when needed |
| Ubuntu | GNOME/Wayland, with GNOME/X11 coverage on a release that supplies it | Builder-hosted VM of the required Ubuntu version, or an appropriate physical Ubuntu machine |
| Kubuntu | KDE/Plasma Wayland, with KDE/X11 coverage on a release that supplies it | Builder-hosted VM of the required Kubuntu version, or an appropriate physical Kubuntu machine |
| Fedora | GNOME/Wayland on the existing Fedora 44 server; other declared variants need their matching session/profile | Direct Fedora server desktop when its version matches the gate; VM for missing variants/versions |
| Debian | KDE/Plasma Wayland on the existing Debian 13 server; other declared variants need their matching session/profile | Direct Debian server desktop when its version matches the gate; VM for missing variants/versions |

Across that distribution coverage, require GNOME/Wayland, GNOME/X11,
KDE/Wayland and KDE/X11 results, as well as Mint Cinnamon/X11. Use native distro
sessions and supported packages. This is coverage across supported combinations;
it does not assume that every distro version ships every desktop/session pair.
Choose and record the exact distro release for each row before a campaign.

The current checked-in VM profiles are `cinnamon-x11`, `gnome-wayland`,
`kde-wayland` and `fedora-gnome-wayland` under `tools/lab/profiles/`. They do not
provide GNOME/X11, KDE/X11 or a Debian-specific automated profile. Provision the
missing VM/profile or use an appropriate native machine and retain manual
acceptance evidence. Do not relabel an existing profile as another distro or
session. An unavailable required combination remains a recorded gate.

`cinnamon-x11-appimage` and `gnome-wayland-appimage` are AppImage installation
variants of the Mint Cinnamon/X11 and Ubuntu GNOME/Wayland profiles. They use
the same corresponding immutable goldens and desktop/session requirements,
with `install: appimage`; they do not replace distro-package acceptance or
stand in for another distribution. Run them on the same capable builders as
their package variants, recording the actual guest release and exact AppImage.
For these Debian-derived AppImage worker rows, set `package: appimage` and
`probePackageManager: deb`: the app comes from the image while the existing
probe tools come from apt. GNOME preparation stages the matching extension
from the installed image before the witness reboot; the image alone does not
install a host Shell extension.

`aurora-kde-wayland` (UEFI, `install: appimage`) is the Universal Blue Aurora
(Fedora Atomic KDE) VM; see [Aurora VM](aurora-vm.md). It has no row in the
package matrix above, because Aurora installs no DEB or RPM, and it doesn't
replace the Kubuntu or Debian KDE/Wayland evidence. It is a required VM desktop,
alongside the Ubuntu GNOME VM, in the
[AppImage full-acceptance profile](#appimage-full-acceptance-profile) below. The
Flatpak's acceptance will use it too. The profile has two goldens on `f44`, one
per Fedora base, and the runner takes either through `--golden`:

| Aurora base | Golden | Plasma / KWin | Role |
| --- | --- | --- | --- |
| Fedora 45 (`ghcr.io/ublue-os/aurora:testing`, `testing-45.20261022.1`) | `aurora-testing-45.20261022.1-852d34e885c1` | 6.7.5 / 6.7.5, kernel 7.2.11 | **Required** for the KDE/Wayland AppImage checks. #478 reproduced only on this base. |
| Fedora 44 (`ghcr.io/ublue-os/aurora:stable`, `44.20260908.1`) | `aurora-stable-44.20260908.1-v3-492c3f8945d1` | 6.7.4 / 6.7.4, kernel 7.1.8 | The base the `stable` tag still shipped on 2026-10-22; run it when a pass compares the two bases or a finding is base-specific. |

`neon-kde-wayland` (BIOS, `install: appimage`) runs two KDE neon goldens for
catching KDE bugs early; see [KDE neon VMs](neon-vm.md). Like Aurora, it has no
row in the package matrix and replaces no required evidence. Run it when a
change touches KWin, the portal or Plasma integration, or when a KDE finding
needs splitting into "already released" and "new on master":

| neon edition | Golden | Plasma / KWin | Role |
| --- | --- | --- | --- |
| Unstable (git master) | `neon-unstable-noble-20261023-d613510c07bd` | master / 6.8.80, Qt 6.11.1 | Furthest-edge KDE target |
| User (release) | `neon-user-6.8.1-20261023-9ef808278853` | 6.8.1 / 6.8.1, Qt 6.11.1 | Newest released Plasma; the comparison point for Unstable findings |

XWayland or `QT_QPA_PLATFORM=xcb` inside a Wayland session does not establish a
native GNOME/X11 or KDE/X11 result. Verify the actual login session/compositor
and Qt platform, along with the OS and desktop versions, in the evidence.

## Package format for issue acceptance

Acceptance for a single issue or pull request runs the exact candidate
**AppImage** on each required desktop by default. One image runs unchanged on
every distro, so a difference between desktops comes from the desktop rather
than from a different build, and the hosts need no install or removal. The
AppImage still uses the host's glibc, compositor, portals, D-Bus, AT-SPI and
clipboard.

Use the matching native DEB or RPM as well, on every desktop the issue
concerns, when the change or the finding involves:

- packaging: dependencies, maintainer scripts, the apt/dnf repositories,
  signing, upgrade or removal;
- the distro's own Qt, platform theme or accessibility stack (for example the
  Qt 6.4 AT-SPI gaps on Debian);
- the distro's GStreamer or FFmpeg (recording);
- desktop integration that differs by format: the installed desktop entry,
  icons, link handlers, autostart or the update path;
- a memory budget. The AppImage maps a different set of libraries, so its
  RSS/PSS is not what a native-package user sees; keep at least one native row.

A native-only regression report is accepted on the native package where it was
reported. Record the package format and its SHA-256 in every row; never present
an AppImage result as native evidence or the reverse.

This sets the default for issue-level runs only. The release profiles below
are unchanged: the [screen recording rows](#screen-recording-rows), the
[Linux update profile](#linux-update-profile-u1u16) and the
[native package full-acceptance profile](#native-package-full-acceptance-profile)
still test each format, and the AppImage profile does not replace them.

## Screen recording rows

Recording ([recording](recording.md)) is accepted per package format, because
each format brings its own media stack: the DEBs and the RPM use the distro's
GStreamer and FFmpeg, while the AppImage carries its own and uses only the host's
VA-API, PipeWire and GPU drivers. Each row installs the exact package (or runs
the exact AppImage) and records a moving pattern through the production CLI:
prepare, start, pause, resume, stop, edit (trim and a timed blur), preview,
save copy and Upload to an isolated local destination, for MP4, WebM, GIF and
WebP. It keeps screenshots of the ready, recording and review states, the
capabilities JSON, the installed identity, and each clip's motion score next
to a static control from the same desktop.

| Package | Desktop/session | Placement | What the row must show |
| --- | --- | --- | --- |
| Ubuntu 24.04 DEB | Mint 22.3 Cinnamon/X11 | physical `.74` (m74), or a Cinnamon VM on a builder | `ximagesrc` capture with the distro encoders |
| Ubuntu 26.04 DEB | Kubuntu 26.04 Plasma/X11 | physical `.72` switched to its Plasma X11 session for the check, or a Kubuntu 26.04 X11 VM | `ximagesrc` capture; VA-API live encoding |
| Ubuntu 26.04 DEB | Ubuntu 26.04 GNOME/Wayland, Kubuntu 26.04 Plasma/Wayland | physical `.72` (Plasma 6.6 Wayland, its default session) for Plasma; VMs on a builder for GNOME | Mutter and KWin native streams from the installed helper; the portal path |
| Debian 13 DEB (`putgd-VERSION-debian13-x86_64.deb`) | Debian 13 Plasma/Wayland | physical `.69` | `kde-screencast` from the installed `putgd-record.desktop`; the portal path |
| Fedora 44 RPM | Fedora 44 GNOME/Wayland | physical `.70` | `mutter-screencast` and the portal; explicit Cisco OpenH264 installation (the one-line installer performs it when the repo is enabled); the actionable `h264_encoder_unusable` message with the `noopenh264` placeholder. Recommends alone do not replace that conflicting placeholder |
| AppImage | the Kubuntu, Debian and Fedora physical desktops above | physical | every plugin loaded from the image; the host GPU encoder used where it works and the bundled x264 where it does not (stock Fedora); AppRun's KWin entry giving `kde-screencast` on Plasma/Wayland and removed on exit |

Each DEB installs only on the release it was built for. The Ubuntu 24.04 and
26.04 DEBs require FFmpeg 6.1 and 8.0 sonames (`libavcodec60`, `libavcodec62`),
and Debian 13 ships 7.1, so Debian rows install the release `debian13` DEB, built
in a Debian 13 container against `libavcodec61` (issue #85). Before that asset
existed they used a DEB built on the Debian host.

## AppImage full-acceptance profile

One pass of the exact published AppImage on every desktop below, with every item
evidenced on every desktop. A failed item is a finding and gets its own issue; the
pass still reports it as a fail. An item that can't run on a desktop needs a
written reason, and Patrick decides whether the pass counts as complete. Nothing
is skipped silently. The passes so far are
[2026-10-21, nightly 842](evidence/2026-10-21-appimage-full-acceptance/README.md),
[2026-10-22, nightly 942](evidence/2026-10-22-appimage-full-acceptance-rerun/README.md) and
[2026-10-23, Qt 6.12.0 (#418)](evidence/2026-10-23-appimage-qt612-418/README.md).

| Desktop | Placement |
| --- | --- |
| Fedora 44 GNOME Wayland | physical `.70` (f44), with the release's own `putgd-gnome50` extension as a user copy |
| Debian 13 Plasma 6.3 Wayland | physical `.69` (d69) |
| Kubuntu 26.04 Plasma 6.6 Wayland | physical `.72` (k72) |
| Linux Mint 22.3 Cinnamon X11 | physical `.74` (m74) |
| Ubuntu 26.04 GNOME Wayland | VM: an overlay of `gnome-provisioned-20261016` on f44 or d69. Remove that golden's old put.gd DEB first, and install the release's extension. |
| Aurora 45 (Universal Blue, Fedora Atomic KDE, Fedora 45 base) | VM: an overlay of `aurora-testing-45.20261022.1-852d34e885c1` on f44 ([Aurora VM](aurora-vm.md#fedora-45-golden)). Required. |
| Aurora 44 (Fedora 44 base) | VM: an overlay of `aurora-stable-44.20260908.1-v3-492c3f8945d1` on f44. Optional second Aurora row; its updater pulls a new `stable` image two minutes after boot unless `uupd.timer` is stopped first ([Aurora VM](aurora-vm.md#automatic-updates)). |

Each desktop uses a private profile: `XDG_CONFIG_HOME`, `XDG_STATE_HOME`,
`XDG_CACHE_HOME` and `XDG_RUNTIME_DIR` of its own. The data home is the user's,
because AppRun registers KWin's permissions and the Wayland icon entries there.
Uploads go to the loopback S3 fixture (`tests/fixtures/s3_server.py`) and never to
a real bucket. Physical desktops need a coordinator lock, a quiet host for the
memory window and the coordinator's OK for each logout.

- **A. Identity:** the AppImage is downloaded from its GitHub release.
  - Its SHA-256 matches `SHA256SUMS`, and `--version-json` reports the release tag.
  - `--appimage-updateinformation` is `zsync|https://app.put.gd/dl/nightly/appimage.zsync` (stable releases: the stable channel).
  - The image root holds exactly one desktop entry, `putgd.desktop`, without `NoDisplay`.
  - Record the OS, desktop and compositor versions, the type of the real login session (`loginctl`, not XWayland) and the Qt platform plugin each put.gd process loads.
- **B. Start:** the AppImage starts from a terminal or the file manager with FUSE, with no extra installs on Aurora.
  - The resident, `putgd-ui` and `session.sock` are up, and `putgd-ui` uses the native platform on Wayland.
  - The tray icon is present, and its menu opens with a real click.
  - The put.gd icon shows in the title bar (where the window decoration draws app icons) and in the task switcher.
- **C. Idle memory:** after a 180 s idle window, the resident's RSS and PSS are under the 40 MiB budget.
  - No GTK, KDE platform theme, `libssl` or `libQt6Network` is mapped in the resident.
- **D. Capture:** region, window and desktop captures each produce a PNG with a history entry.
  - **Edit & upload:** a blur and a solid redaction, checked in the uploaded pixels, then Upload.
  - **Save a copy…** opens the desktop's portal chooser (the themed fallback only where no portal FileChooser exists), and the saved file exists.
  - The link is copied according to the clipboard setting: each link on every desktop, plus the list behaviour on at least one.
- **E. Recording:** a moving pattern, recorded through the production path: prepare, start, pause, resume, stop.
  - In the Review window: a trim and a timed blur.
  - Export MP4, WebM, GIF and WebP, and save one copy through the chooser.
  - Each clip's motion score is compared with a static control recorded on the same desktop.
  - Record the live encoder (VA-API or the bundled x264).
  - On Plasma Wayland, AppRun's KWin entry gives `kde-screencast`, and the entry is removed on exit.
- **F. Notifications:** the link-ready notification appears, and a real click on it opens the link in the desktop's real default browser, which loads the image.
  - The image is a synthetic PNG uploaded public-read to the live S3 test prefix (`put.gd/acceptance/`, key set 2), so `https://put.gd/<key>` serves it. It's deleted afterwards with **Delete from destination**, and the prefix is listed empty.
  - The browser runs in a temporary profile, never one of Patrick's. Where the default `https` handler isn't a browser (the ChatGPT app on d69 and k72), Firefox is made the default for the test only, and `mimeapps.list` is restored byte for byte afterwards.
  - The frame shows the address bar and the image. A recorded `x-scheme-handler/https` handler (#326's recorder) stays as a supporting check of the opener's environment.
- **G. History:** entries show with previews, including a video poster, and **Copy image** puts the same pixels on the clipboard.
  - **Delete from destination…** removes the uploaded object from the fixture, and the row shows "Deleted from destination". The local file is kept by design.
- **H. Settings:** a theme switch (System, Dark, Light) repaints the open window live.
  - **Updates** shows **Channel** on *Nightly*, the build's own (*Stable* and *Nightly* are the only choices since #344), with a status from the live app.put.gd feed.
  - **Check now** works, and the nightly channel shows no notification.
- **I. Start at login:** turn on **Launch at login**, then do a real logout and login. Put.gd must start from the AppImage.
  - Physical desktops: only with the coordinator's OK, and only after checking that none of Patrick's apps are open.
  - VMs: through their autologin.
- **J. Update check:** `appimageupdatetool --check-for-update` against the published zsync gives the correct answer for the feed at that moment. The full N→N+1 update is #296's check.
- **K. Clean exit and cleanup:** quit from the tray, and nothing of put.gd keeps running.
  - The AppImage, the `putgd-appimage-*` and icon entries, the autostart entry, the profile and the caches are removed.
  - VM overlays, tunnels and wall tiles are removed, and locks are released with `--verified-clean`.

Evidence per desktop:
- a desktops × A–K pass/fail table, with versions, memory numbers, encoders and motion scores;
- annotated full-desktop pictures of the tray menu, Settings, a capture in the editor, the Review window, the native **Save a copy…** chooser and the link-ready notification;
- every frame checked before it is kept.

## Linux update profile (U1–U16)

One pass proves that every Linux install format updates from one published nightly to the next,
through every route a user would take. The pass uses two consecutive published nightlies, N and N+1, as
CI and the signer published them. Neither is rebuilt.

The same rules as the AppImage profile apply:
- every item is evidenced on every desktop where it applies;
- a failed item is a finding with its own labelled issue, and the pass reports it as a fail;
- an item that can't run needs a written reason, and Patrick decides whether the pass counts.

The passes so far are listed in the evidence folders named `*-linux-update-acceptance`.

**Inputs:**
- **N and N+1:** the published GitHub releases, each checked against its `SHA256SUMS`. Both must contain
  every change the pass accepts, and N+1 must be the newest nightly while the desktops run.
- **Lab stable, until the first stable release exists:** U6's populated-stable leg, U10 and U14's stable
  half need a stable channel. Real nightlies trust only the real stable key, so this leg uses lab-key
  builds of N+1's commit:
  - one local commit puts a lab-only stable key in `packaging/linux/repo/keys/`, and empty commits give
    the versions;
  - the builds are nightly L1, stable S1 (older than L1) and stable S2, for every target (the three DEBs,
    the RPM and the AppImage), plus stable S3 for the AppImage;
  - they are published under `pkg.put.gd/lab/` only: nightlies by the signer's lab scope, stable signed
    with the lab key, as in #344 and #386;
  - afterwards every lab object is deleted and answers 404, and the lab keys are shredded.

  Once a real stable release exists, these legs run on the real stable channel instead, with no lab
  builds. Live feeds, the stable channel and the real stable key are never written by the pass.

**Where each item runs:**

| Item | f44 Fedora 44 GNOME | d69 Debian 13 Plasma | k72 Kubuntu 26.04 Plasma | m74 Mint 22.3 Cinnamon | Ubuntu 26.04 GNOME VM | Aurora VM |
| --- | --- | --- | --- | --- | --- | --- |
| U2–U5 package updates | RPM, dnf | DEB `debian13`, apt | DEB `ubuntu26.04`, apt | DEB `ubuntu24.04`, apt | DEB `ubuntu26.04`, apt | n/a (no DEB or RPM) |
| U3 GUI updater | GNOME Software | GNOME Software (the installed updater; Discover isn't installed) | Discover | Update Manager | whatever **Open software updates** finds, or its message | n/a |
| U6 DEB/RPM channel | live + lab | live + lab | live + lab | live + lab | live | n/a |
| U7, U8, U11 AppImage updates | yes | yes | yes | yes | yes | yes |
| U9 Gear Lever, AppImageLauncher | | | | | both | Gear Lever |
| U10 AppImage channel | lab | | | lab | | |
| U12 refusals | all | restart pending, folder not writable | restart pending, folder not writable | all | | |
| U13 discovery | GNOME Software | GNOME Software | Discover | Software Manager | | |
| U14 notifications | nightly, stable | nightly | nightly | nightly, stable | nightly | nightly |
| U15 idle RSS | yes | yes | yes | yes | yes | yes |

Physical desktops need a coordinator lock (`tools/lab/desktop_lock.py`: check the acquire, `extend`
before expiry, release with `--verified-clean`). VMs are overlays of the goldens, retired afterwards.
Installs are scoped to `putgd`: `apt install --only-upgrade putgd`, `dnf upgrade putgd`, never a bare
upgrade. A GUI updater installs only put.gd: other updates are unticked, or the reason it can't be
scoped is written down. Before any full-desktop frame, close Patrick's Firefox, ChatGPT and Codex
windows, or leave that desktop until he has. No link may reach his browser:
- The drivers refuse every click unless the session's `http` and `https` default is #326's stand-in
  recorder, or a temporary Firefox profile from #397's item F helpers (the guard #415 added).
- Turn the recorder on before the first click.
- Switch to the temporary Firefox profile only for the link steps (U5, U14).

Each resident runs with private `XDG_CONFIG_HOME`,
`XDG_STATE_HOME`, `XDG_CACHE_HOME` and runtime directory unless the item tests the default profile
(U5, U11).

- **U1. Live infrastructure agrees.** Check this when N+1 is published, before the desktops start, and
  again at the end:
  - `app.put.gd` and `pkg.put.gd` `update/v1/nightly.json` name N+1, and the `.minisig` verifies with the
    nightly key, with the trusted comment naming N+1. `stable.json` verifies too and names the real stable
    release, or `release: null`.
  - `/dl/nightly/<target>` resolves to N+1's asset for `appimage`, `appimage.zsync`, `ubuntu24.04`,
    `ubuntu26.04`, `debian13` and `fedora44`.
  - The `pkg.put.gd/releases/<tag>/` mirror holds the AppImage, its `.zsync`, `SHA256SUMS` and
    `SHA256SUMS.minisig`, with the same SHA-256 as the GitHub release. The `.zsync` names the mirror's
    image and its SHA-1.
  - Each apt suite's `InRelease` verifies with the nightly key, and its `Packages` lists N+1 with the
    release's SHA-256. dnf's `repomd.xml` signature verifies, and `primary` lists N+1 with that SHA-256.
  - The DEP-11 and dnf AppStream catalogs name N+1.
  - Every mismatch is a finding.
- **U2. apt and dnf.** On a desktop without put.gd, install N from its release file (`apt install ./…deb`,
  `dnf install ./…rpm`), the documented way a user starts. Check the repository file it adds.
  - Then upgrade it to N+1 with `sudo apt update && sudo apt install --only-upgrade putgd`, or
    `sudo dnf upgrade --refresh putgd`.
  - apt and dnf verify the signature. N+1 installs with no conffile prompt, and the repository file is
    unchanged.
- **U3. The desktop's GUI updater.** With N installed again, the updater lists put.gd N → N+1 (picture),
  then installs it with only put.gd selected. A polkit prompt is Patrick's to answer: ask for him through
  the coordinator, never type it. Where the updater can't install one package alone, write down why and
  apply N+1 through PackageKit (`pkcon update putgd`), the updater's own back end.
- **U4. Settings on a repository install.** On N, before the upgrade:
  - Settings → Updates shows Channel *Nightly*, read-only, set by the repository file. **Check now**
    finds N+1 "from put.gd's nightly repository".
  - **Open software updates** opens the U3 updater, shown by a real click. Where none is installed, the
    message gives the command.
  - There's no **Download and install** and no notification ("doesn't nag"). After the upgrade and a
    restart of put.gd, Settings says *Up to date*.
- **U5. An upgrade keeps settings and history.** Use put.gd's default profile, with the resident running
  through the upgrade, as a user has it:
  - Beforehand, change settings (theme, clipboard, a destination) and make history: two captures, one
    uploaded public-read to the live S3 test prefix (`put.gd/acceptance/`, key set 2).
  - After the upgrade, put.gd restarts, or it's started again. `config.json` is byte-identical, or differs
    only by a documented migration. The history entries, previews and links are all there.
  - A real click on the uploaded entry's link opens it in the desktop's real default browser, as in
    profile item F above.
  - The object is then removed with **Delete from destination**, and the prefix is listed empty.
- **U6. DEB/RPM channel switch** ([package-repositories.md](package-repositories.md#switching-channels)).
  Use the commands Settings → **Other channel** copies, run as given (`apt-get -y` only in place of the
  prompt):
  - **Live:** move N+1 to stable. While stable has no release, the stable suite is empty: the last
    command installs nothing, the nightly stays and Settings says so. Move back to nightly: no conffile
    prompt, and the file equals `putgd-nightly.*`. Then N → N+1 updates through the nightly suite
    as in U2.
  - **Populated stable (lab):**
    - L1 → stable S1 (older; downgrade). S2 is published, the GUI updater shows it, and `--only-upgrade`
      takes S1 → S2.
    - The channel is kept. Back to nightly L1, then a newer lab nightly (or the downgrade when L1 is
      older), with no prompt.
- **U7. AppImage in-app update N → N+1**, through Settings → **Download and install** with a real click
  on at least one desktop, and `update.install` elsewhere:
  - **zsync:** with the real `appimageupdatetool` on `PATH`. The mirror's `.zsync` is used and only the
    changed blocks are fetched.
  - **The fallback, forced once per desktop:** a crashing `appimageupdatetool` on `PATH`, so the verified
    full download is taken and `zsync.fallbackReason` names the crash.
  - The new image is on the same path, with its mode kept. **Restart put.gd** runs N+1, and the hidden
    backup is gone after that first start.
- **U8. appimageupdatetool** (continuous build, digest recorded) on N: `--check-for-update` says yes, and
  the update writes N+1 beside N, which matches `SHA256SUMS`. On Fedora this needs
  `SSL_CERT_FILE=/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem` ([#297](https://github.com/patrick-hudson/put.gd-desktop/issues/297)).
  Run put.gd from N's path afterwards: login (U11) picks N+1.
- **U9. Gear Lever and AppImageLauncher** (as in [#412](evidence/2026-10-22-appimage-integrators-412/README.md)):
  - Each integrates N. Then put.gd's in-app update N → N+1 runs from the integrator's menu entry.
  - The entry and the record still start N+1, with no second entry and no adopted backup.
  - Each tool's own check then reports no update. Gear Lever's own **Update** on a second copy of N goes
    to N+1 too.
- **U10. AppImage channel switch.** Before the first stable, on the lab builds:
  - L1 watching Stable while lab `stable.json` has `release: null` says "no stable release yet", with
    no button. The same check against the live `app.put.gd` stable feed does too.
  - L1 → S1 (older: refused without `allowOlder`, then confirmed). S1 → S2 is an ordinary update.
  - S2 → nightly with **Switch to nightly**. A tampered stable release is refused.
- **U11. Start at login after an update.** Turn on **Launch at login** on N. Update in-app without a
  restart (and, on a second run, with appimageupdatetool). Then a real logout and login. Login starts
  N+1 from the same folder.
  - Physical desktops: only with the coordinator's OK, after checking that none of Patrick's apps are
    open. VMs: through their autologin.
- **U12. Refusals.** Each reason is shown in Settings and in `update.status`, and the image is unchanged:
  - the AppImage's folder isn't writable;
  - the file belongs to another user;
  - no room for a second copy (a small tmpfs);
  - an upload is running (`update_install_busy`);
  - an installed update still waits for its restart;
  - a DEB or RPM build (`not_appimage`);
  - a tampered image, an unsigned `SHA256SUMS` and a rewritten `SHA256SUMS` (loopback copy of N+1).
- **U13. Discovery** ([#314](https://github.com/patrick-hudson/put.gd-desktop/issues/314)). With put.gd not
  installed, add only the live nightly repository file and its key. These are the bytes the DEB or RPM
  would install, taken from N+1's package without installing it.
  - The software centre's search finds put.gd, with name, icon and summary (Mint: "Putgd", from apt;
    [#430](https://github.com/patrick-hudson/put.gd-desktop/issues/430)).
  - Installing it from there installs N+1. A later remove leaves the system as before.
- **U14. Notifications, each checked against its feed at that moment** (a `dbus-monitor` log of
  `org.freedesktop.Notifications` beside the picture):
  - **Nightly never:** an AppImage and a repository DEB/RPM on N, against the live feed naming N+1,
    through **Check now** and a scheduled check (5–15 min after start). There is no notification,
    and the tray entry follows [updates.md](updates.md#who-is-told).
  - **Stable once per version** (lab until the first stable): S1 against a stable feed naming S2 gives
    exactly one notification. A restart and **Check now** don't repeat it, and S3 gives one more.
  - A real click on the notification opens the release notes in the real default browser.
  - A stable DEB from the repository never notifies.
- **U15. Idle RSS.** The resident on N+1 after each desktop's updates, quiet host, `--background`, 30 s
  settle, then RSS every second for 180 s. The median, Pss and the mapped TLS/network libraries are
  recorded, compared with N on the same desktop. Both stay under the 40 MiB budget.
- **U16. Cleanup.** Remove put.gd, the repository files and keys the pass added, the AppImages, profiles,
  autostart entries, lab objects (404 confirmed), lab keys, VM overlays, tunnels and wall tiles.
  - Restore each desktop's default browser byte for byte.
  - Release the locks with `--verified-clean`.

**Evidence:**
- **Pictures:** one annotated full-desktop picture per desktop per route, whose footer names the build
  (version and commit). Each is captured from that build during this pass (rule 16 in the coordination
  notes), and every frame is checked for privacy before it is kept.
- **A results table:** desktops × U1–U16, with versions, methods, timings and RSS. Every cell names its build
  (version and commit), host and run name, so later passes can cite a result instead of repeating it.
- **The run logs** (`results/`) and the drivers (`lab/`).
- **A docs-only pull request** into `nightly`, with each failure linked to its issue.

## Native package full-acceptance profile

One pass of one exact published nightly's DEBs and RPM on every desktop below,
with every item evidenced on every desktop where it applies. It covers the
AppImage profile's A–K, adapted for an installed package, and the package
lifecycle of [#375](https://github.com/patrick-hudson/put.gd-desktop/issues/375)
(DEB), [#377](https://github.com/patrick-hudson/put.gd-desktop/issues/377) (RPM)
and [#382](https://github.com/patrick-hudson/put.gd-desktop/issues/382) (the
login-startup entry after uninstall). The AppImage has its own profile above.

The same rules as the AppImage profile apply:
- every item is evidenced on every desktop where it applies;
- a failed item is a finding with its own labelled issue, and the pass reports it as a fail;
- an item that can't run needs a written reason, and Patrick decides whether the pass counts as complete.

Nothing is skipped silently. The passes are in the evidence folders named
`*-native-full-acceptance`. The first was
[2026-10-23, nightly 1058](evidence/2026-10-23-native-full-acceptance/README.md), and its drivers are in
[`2026-10-23-native-full-acceptance/lab/`](evidence/2026-10-23-native-full-acceptance/lab/README.md).

| Desktop | Package | Placement |
| --- | --- | --- |
| Linux Mint 22.3 Cinnamon X11 | `ubuntu24.04` DEB | physical `.74` (m74) |
| Kubuntu 26.04 Plasma 6.6 Wayland | `ubuntu26.04` DEB | physical `.72` (k72); its Plasma X11 session for the recording row's X11 half (item E) |
| Ubuntu 26.04 GNOME 50 Wayland | `ubuntu26.04` DEB, with the package's `putgd@put.gd` extension | VM: an overlay of `gnome-provisioned-20261016` on f44 or d69. The golden carries an old put.gd DEB (`0.1.1~dev.63`) and a system extension: purge both first and record it. |
| Debian 13 Plasma 6.3 Wayland | `debian13` DEB | physical `.69` (d69) |
| Fedora 44 GNOME 50 Wayland | `fedora44` RPM, with the package's `putgd@put.gd` extension | physical `.70` (f44) |

**Inputs:**
- **The accepted nightly:** the exact `vYY.MDD.N-nightly` release the coordinator names (calendar versions, [#373](https://github.com/patrick-hudson/put.gd-desktop/issues/373)). Every asset is checked against the release's `SHA256SUMS`, and the `pkg.put.gd/releases/<tag>/SHA256SUMS` mirror copy against its `.minisig` and the nightly minisign key. The apt suites and the dnf repository carry only the newest nightly, so the items that install through pkg.put.gd (L and M2) run while the accepted nightly is still the newest. If a newer nightly is published mid-pass, those items stop, and the coordinator either names the newer nightly or approves rerunning them.
- **The schema-5 build:** `v0.1.1-dev.133.20261013004314+gb0ed0461c600` (`b0ed0461`), the last published build with history `user_version=5`. Every later build writes 7. It has `ubuntu24.04` and `ubuntu26.04` DEBs, a `fedora44` RPM and an AppImage, but no `debian13` DEB. It also predates [#56](https://github.com/patrick-hudson/put.gd-desktop/issues/56): its config and data live in `~/.config/putgd/putgd/` and `~/.local/share/putgd/putgd/`, which the first newer start moves to `~/.config/putgd/` and `~/.local/share/putgd/`. So this upgrade runs both migrations.
- **The old-scheme nightly:** `v0.1.1-nightly.947`, the last nightly before calendar versions. It adds the same pkg.put.gd nightly repository as the CalVer packages.
- **Payloads:** synthetic only. Uploads go to the loopback S3 fixture (`tests/fixtures/s3_server.py`), except item F's single object, which goes to the live S3 test prefix `put.gd/acceptance/` with builder key set 2. The key set is read on the builder into the driver's stdin and passed to put.gd over its session socket. It's never in a command line, file, log, picture or message.

**Rules on the desktops:**
- Physical desktops need a coordinator lock (`tools/lab/desktop_lock.py`): check the acquire, `extend` before expiry, release with `--verified-clean`. Every logout follows the coordinator's notice rule. VMs are overlays of the goldens, retired afterwards.
- Installs are scoped to put.gd: `apt install ./putgd-….deb`, `apt install --only-upgrade putgd`, `dnf install`/`dnf upgrade putgd`, never a bare upgrade. Check each transaction before confirming it, so nothing else comes with it. Item E's OpenH264 checks on f44 change `openh264`/`noopenh264` too, and only with the coordinator's OK; they're restored afterwards.
- A software-centre step that asks for Patrick's polkit password is batched and asked for through the coordinator. Nobody types or handles it for him.
- **Privacy:**
  - Before any full-desktop frame, close Patrick's Firefox, ChatGPT and Codex windows by exact PID (builder authority), and check every frame before keeping it.
  - No link may reach his browser profile. The drivers refuse every click unless the `http` and `https` default is #326's stand-in recorder (`i326.py handler-on`), or a temporary Firefox (`ffdefault.py on` where the default isn't a browser, as on d69 and k72 with the ChatGPT app; `ffprofile.py on` where it's Firefox). Turn the recorder on before the first click. Switch to the temporary Firefox only for item F, and restore `mimeapps.list`, `kde-mimeapps.list` and `profiles.ini`/`installs.ini` byte for byte afterwards.
- **Profiles:**
  - Items A–H, J and K use a private profile: `XDG_CONFIG_HOME`, `XDG_STATE_HOME`, `XDG_CACHE_HOME` and a runtime directory of the run's own.
  - Items L, M, I, P and Q use put.gd's default profile, as a user has it. The drivers refuse to start when `~/.config/putgd`, `~/.local/state/putgd`, `~/.cache/putgd` or `~/.local/share/putgd` already exists; someone else's folders go to the coordinator. Cleanup removes only the folders the run created.

**Overlap with the [Linux update profile](#linux-update-profile-u1u16):** the
update pass covers N → N+1 through apt and dnf (U2), the GUI updaters (U3), the
repository channel switch (U6), software-centre discovery (U13), notifications
(U14) and idle RSS (U15). This profile doesn't repeat them. An item here cites
an update-pass cell only when that cell's build is this pass's accepted nightly,
on the same desktop and with the same check, and names its build, host and run.
Anything visible is pictured again from the accepted build here (rule 16 in the
coordination notes). Items L, M1, M2, I and P–Q aren't in the update profile.

### Accepted-build items (A–K)

On the accepted nightly, installed by item L.

- **A. Identity:**
  - The installed version is the release's, as the package manager has it: `dpkg-query -W putgd` gives `YY.MDD.N~nightly`, and `rpm -q putgd` gives `putgd-YY.MDD.N~nightly-…fc44`. `YY.MDD` is the UTC date of the tag's commit, and `N` is the tag's run number.
  - `putgd --version-json` reports the tag and commit that `release-builds.json` names.
  - The package file in the apt or dnf cache has the `SHA256SUMS` digest, and `dpkg --verify putgd` or `rpm -V putgd` is clean.
  - The package installs these entries:
    - `putgd.desktop`, which shows in the menu, and `putgd-record.desktop`;
    - the hicolor icon and the AppStream metainfo (`appstreamcli validate` passes);
    - the repository file and key ([package-repositories.md](package-repositories.md#what-the-packages-add));
    - on GNOME, the system extension `putgd@put.gd`.
  - Record the OS, desktop and compositor versions, the type of the real login session (`loginctl`, not XWayland), and the Qt platform plugin each put.gd process loads.
- **B. Start:**
  - put.gd starts from the desktop's application menu with a real click: Cinnamon's menu, Kickoff, or the GNOME overview search. The resident, `putgd-ui` and `session.sock` are up, and `putgd-ui` uses the native platform on Wayland.
  - The tray icon is present, and its menu opens with a real click.
  - The put.gd icon shows in the menu, the title bar (where the decoration draws app icons) and the task switcher.
- **C. Idle memory:** after a 180 s idle window on a quiet host, the resident's RSS and PSS are under the 40 MiB budget.
  - Record what the resident maps: GTK, the KDE platform theme, `libssl` and `libQt6Network`, against the AppImage's C (none).
  - Record the host load beside the numbers.
- **D. Capture:**
  - Region, window and desktop captures each produce a PNG with a history entry, through the desktop's native path:
    - X11 on Cinnamon;
    - KWin's ScreenShot2 on Plasma, authorized by the installed entry;
    - the packaged extension on GNOME.
  - **Edit & upload:** a blur and a solid redaction, checked in the uploaded pixels, then Upload.
  - **Save a copy…** opens the desktop's portal FileChooser ([file-dialogs.md](file-dialogs.md)). The themed fallback is used only where no portal FileChooser exists. The saved file exists.
  - The link is copied according to the clipboard setting.
- **E. Recording:** the [recording row](#screen-recording-rows) for this package, with the distro's GStreamer and FFmpeg.
  - A moving pattern, recorded through the production path: prepare, start, pause, resume, stop.
  - In the Review window: a trim and a timed blur. Export MP4, WebM, GIF and WebP, and save one copy through the chooser.
  - Each clip's motion score is compared with a static control recorded on the same desktop.
  - Record the capture source (`ximagesrc`, `kde-screencast`, `mutter-screencast` or the portal) and the live encoder.
  - On Plasma Wayland, `kde-screencast` comes from the installed `putgd-record.desktop`.
  - On k72, a second recording in the Plasma X11 session covers the row's `ximagesrc` and VA-API half. That's two extra logouts, each needing the coordinator's OK.
  - On f44, run `sudo dnf install openh264 gstreamer1-plugin-openh264` explicitly (the one-line installer does this with the Cisco repo enabled). The earlier item L transaction does not establish that Recommends replace an installed `noopenh264` conflict; issue #1091 confirms they do not. With the placeholder loaded, the actionable `h264_encoder_unusable` message shows, and recording works again once `openh264` is restored. Where `dnf swap` would remove other packages too (`mozilla-openh264` on f44), the extracted placeholder library is loaded into the resident only (`LD_LIBRARY_PATH`), and no host package changes.
- **F. Notification link:** the link-ready notification for one synthetic PNG uploaded public-read to `put.gd/acceptance/nfa/<desktop>/` (key set 2, in memory only), so that `https://put.gd/<key>` serves it.
  - A real click on the notification opens the link in the desktop's real default browser, which loads the image. The browser runs a temporary profile, never one of Patrick's. On d69 and k72 the default `https` handler is the ChatGPT app, so a temporary Firefox handler (`ffdefault.py`) is used there, and the files are restored byte for byte.
  - The frame shows the address bar and the image. An unauthenticated GET returns the uploaded bytes.
  - Afterwards the object is removed with **Delete from destination**, and the prefix is listed empty with key set 2.
- **G. History:**
  - Entries show with previews, including a video poster, and **Copy image** puts the same pixels on the clipboard.
  - **Delete from destination…** removes an uploaded object from the fixture, and the row shows "Deleted from destination". The local file is kept by design.
- **H. Settings:**
  - A theme switch (System, Dark, Light) repaints the open window live.
  - **Updates** shows **Channel** *Nightly*, read-only, "set by the repository file", with no **Download and install** ([#428](https://github.com/patrick-hudson/put.gd-desktop/pull/428)).
  - The **Other channel** commands, copied with a real click, match [package-repositories.md](package-repositories.md#switching-channels) for this distro. They're compared as clipboard text, and not run; U6 runs them.
  - **Check now** gives a status from the live feed, and there's no notification.
  - **Open software updates**, with a real click, opens this desktop's updater: Update Manager on m74, Discover on k72, GNOME Software on f44 and d69, and on the VM whatever it finds, or its message with the command.
- **I. Start at login:** turn on **Launch at login** on the default profile.
  - `~/.config/autostart/putgd.desktop` is written with `Exec="/usr/bin/putgd-launcher" --background` and `X-putgd-Managed=true` (record its bytes).
  - After a real logout and login, the resident runs from `/usr/bin/putgd`.
  - Physical desktops: only with the coordinator's OK, and only after checking that none of Patrick's apps are open. VMs: through their autologin.
- **J. Package manager view:**
  - `apt update` verifies the live `InRelease`, and `apt-cache policy putgd` shows the installed version as pkg.put.gd nightly's candidate. On Fedora, `dnf check-update putgd` verifies `repomd.xml` and lists nothing newer.
  - Settings says *Up to date*.
  - N → N+1 through the package manager is U2's, cited as above.
- **K. Clean exit:** quit from the tray, and no put.gd process keeps running: the resident, `putgd-ui`, `putgd-record`, the S3 worker or any other helper. The run's private profile is removed.

### Lifecycle items (L–Q)

These run in this order on each desktop, around A–K: L, then A–K, then M1, M2, O, I, P and Q.

- **L. Fresh install from live pkg.put.gd:**
  - The system starts clean: no `putgd` package, no repository file or key, and no default profile. Record each.
  - Add the live nightly repository file and key. These are the bytes the accepted package installs, taken from it with `dpkg-deb -x` or `rpm2cpio` without installing it.
  - Install with `sudo apt update && sudo apt install putgd`, or `sudo dnf install putgd`. apt or dnf verifies the live signature, and the package comes from pkg.put.gd's pool with the release's digest. There's no conffile prompt. Record the full transaction, including the Recommends on Fedora.
  - First run on the default profile, started from the menu:
    - the first-run setup appears, and a destination is added (the loopback fixture);
    - a region capture uploads and its link is copied.
  - The README's file route (`apt install ./putgd-….deb`) is item M's, for the older builds.
- **M1. Upgrade from the schema-5 build:**
  - Install dev.133 from its release file, on the default profile, and seed it through its own CLI (`action.invoke`, `setup.storeCredentials`, `delete.prepare`/`confirm`, `history.clear`):
    - a customized `config.json` (theme, clipboard mode, a fixture destination);
    - a keychain credential reference;
    - uploaded, failed (retained operation) and deleted-from-destination entries, and cleared entries;
    - local outputs.
  - Grow the history to 10,000 entries by copying those real rows with fresh ids; nothing else changes. dev.133 reopens it, and `history.list` gives a total of 10,000.
  - Record, before and after:
    - the `config.json` digest;
    - `user_version`;
    - row counts per upload, local and remote status;
    - the deletion journals;
    - the local output digests.
  - Upgrade to the accepted package with `apt install ./putgd-….deb` (dev.133 predates the repository) or `dnf upgrade ./putgd-….rpm`. Start it:
    - #56's move: `config.json` and `history.sqlite` are now in `~/.config/putgd/` and `~/.local/share/putgd/`, the `putgd/putgd` folders are gone, and the storage report says `migrated`;
    - `PRAGMA user_version` is 7 and `integrity_check` says `ok`.
  - Every row and count is preserved, and the deletion outcomes are in `item_state`. `config.json` is byte-identical, or differs only by a documented migration. The credential reference still resolves: **Test connection** passes against the fixture.
  - History opens, searches, copies a link and an image, and deletes one entry from the destination. Record the start time with 10,000 entries.
  - **d69:** dev.133 has no `debian13` DEB, and its Ubuntu DEBs need FFmpeg sonames Debian 13 doesn't have. The seed is written by the dev.133 AppImage on the same default profile, which uses the same paths. Then the accepted `debian13` DEB is installed and does the migration.
  - **Mint, Kubuntu and Ubuntu:** if a dev.133 DEB no longer installs, the same AppImage seed is used, and the reason is written down.
- **M2. Upgrade from the old-scheme nightly through pkg.put.gd:**
  - Install `0.1.1-nightly.947` from its release file. It adds the nightly repository file; record its digest.
  - Populate the default profile: settings changes, a destination, two captures, one uploaded.
  - With the resident running, upgrade it as a user would: `sudo apt update && sudo apt install --only-upgrade putgd`, or `sudo dnf upgrade --refresh putgd`.
  - Check the version ordering (`dpkg --compare-versions 0.1.1~nightly.947 lt YY.MDD.N~nightly`, `rpmdev-vercmp`). The signature is verified, the accepted version installs with no conffile prompt, and the repository file is unchanged.
  - After put.gd restarts, `config.json` and the history are as before (both entries, previews, the link).
- **O. Companion:**
  - **GNOME** (f44, the Ubuntu VM): after M1 the running shell still has dev.133's extension (`version` 1; 947 and 1058 ship 2, so M2 can't show it).
    - `capabilities` reports `helper.outdated: true` with both versions, screenshots keep working, and an interactive region recording is refused with `helper_outdated` and its message ([gnome.md](desktops/gnome.md)). A re-login fixes it.
    - `gnome-extensions disable putgd@put.gd` gives the documented message, and `enable` restores capture.
    - After item P, the extension is gone at the next login. `enabled-extensions` is restored to its value before the pass.
  - **Plasma** (k72, d69):
    - The installed `putgd.desktop` and `putgd-record.desktop` authorize KWin's ScreenShot2 and screencast: capture and recording work with no prompt.
    - After item P they're gone, and `kbuildsycoca6` no longer lists them.
  - **Cinnamon** (m74): the package ships no applet. Record that it has none, and check the tray only.
- **P. Removal, then a real login:** with **Launch at login** on (from item I):
  - `sudo apt remove putgd`:
    - the package files are gone (compared with `dpkg -L` before);
    - the conffile `putgd.sources` stays (record it);
    - a resident that was running keeps running from its deleted executable until it quits or the session ends, as for any package (record it).
  - `sudo apt purge putgd`: the repository file and key are gone.
  - Fedora: `sudo dnf remove putgd`. `rpm-postun.sh` removes `putgd.repo`, and the `RPM-GPG-KEY-putgd*` files go with the package.
  - User data is preserved: the default profile's digests match the ones from before.
  - After a real logout and login, there's no put.gd process.
- **Q. The login-startup entry after uninstall (#382):** observe and record what's there, without changing anything. Patrick hasn't decided between `TryExec` and accepting the entry as it is.
  - After P's login, `~/.config/autostart/putgd.desktop` is still there: record its bytes, which name a missing `/usr/bin/putgd-launcher`.
  - What the session does with it at login:
    - the user journal's lines for it, and `systemctl --user --failed` (systemd's `app-putgd@autostart.service` on GNOME and Plasma);
    - any notification;
    - whether the desktop's startup-applications settings list it (picture): Cinnamon's Startup Applications, Plasma's Autostart, GNOME's Background Apps or Tweaks.
  - **TryExec probe:** replace that entry with the same bytes plus `TryExec=` its Exec program (`/usr/bin/putgd-launcher`), in the run's own recorded file, and log in again. Record the same observations: whether the entry is now ignored silently.
  - Put both options' consequences in the results, from these observations:
    - **TryExec:** the entry does nothing and logs nothing after removal, and a reinstall brings the preference back. It needs a code PR (`LoginStartup`, unit test) and docs.
    - **Accept as is:** whatever the first login showed (failed unit, journal lines, a listing in the settings), on every login until the user removes the file. Plus documentation: turn off **Launch at login** before uninstalling.
  - At cleanup, the run removes the entry it created.

**Cleanup:**
- Remove put.gd, the repository files and keys the pass added, the private profiles and caches, the default-profile folders the run created, the keychain entry it stored (by its recorded name), the autostart entry, the temporary Firefox state and the recorder entry. Do this last, once nothing can open a link.
- Restore the `openh264` state on f44.
- Retire the VM overlays, tunnels and wall tiles.
- Release the locks with `--verified-clean`.

**Evidence:**
- **Pictures:** annotated full-desktop pictures per desktop. Each footer names the build (version and commit), and each is captured from that build during this pass (rule 16). They show:
  - the menu entry and tray menu;
  - Settings → Updates (read-only channel, copied commands);
  - the updater that **Open software updates** opened;
  - a capture in the editor and the native **Save a copy…** chooser;
  - the Review window;
  - the link-ready notification, and the browser showing the live image;
  - History after the 10,000-entry migration;
  - GNOME's `helper_outdated`;
  - the startup-applications listing after removal.
- **A results table:** desktops × A–Q, with versions, digests, memory numbers, encoders and motion scores. Every cell names its build, host and run, and update-pass citations say so.
- **A timeline per desktop:** lock, installs, logins and release, in UTC.
- **The run logs** (`results/`) and the drivers (`lab/`).
- **A docs-only pull request** into `nightly`, with each failure linked to its issue. #375, #377 and #382 are updated with the results.

## Choose physical hardware or a VM for each gate

Prefer Patrick's [Fedora and Debian build servers](remote-builders.md), including
their own visible GNOME/Plasma desktops. Build and test directly there when the
host matches the required release/desktop/session. Provision a fresh VM on a
capable builder for missing distributions, versions or sessions. Discover/copy
[immutable golden inputs](remote-golden-images.md) and expose each remote guest
on the [workstation display wall](remote-vm-displays.md). Use local VMs when the
builders cannot run the profile, and record the fallback reason.

**A release gate that requires physical/native hardware must run on the
appropriate physical machine.** A VM result cannot close that gate. Mark the
required form factor before testing; record physical gates as blocked until the
appropriate machine is available. Do not downgrade hardware, display topology,
acceleration or compositor requirements to fit an available VM. VM functional
coverage and physical release evidence remain separately identifiable.

Check KVM, capacity and visible session access on the actual VM host before
launch. A server can still run direct desktop acceptance while its VM readiness
is blocked. Preserve the user's existing host projects/windows; use isolated
payloads, settings and test destinations.

## Ordering, evidence and status

Run a new candidate's rows in parallel, each with its functional and memory gates
(see [AGENTS.md](../AGENTS.md)). A scoped user
waiver applies only to its named task/gate and does not certify the full matrix.

Use the same exact accepted source/build candidate across the declared matrix.
For every result retain distro/release, desktop/compositor version, X11/Wayland
session, Qt platform, physical machine or VM identity, acceleration, installed
and running build identities, scenario parameters, outcome and blocked gates.
Keep graphical interactions visible on the wall and save screenshots/recordings,
including failures, alongside independent artifact/paste witnesses as required.
SSH logs, offscreen tests, a live tile or historical acceptance cannot certify a
new row/candidate. Existing performance, geometry and fresh-agent release gates
remain in [verification](verification.md); preserve their historical evidence.
