# Linux update acceptance: nightly 1017 → 1019 on six desktops

The first pass of the [Linux update profile](../../acceptance-matrix.md#linux-update-profile-u1u16)
(items U1–U16), recorded 2026-10-22/23 by the linux-update-acceptance session. It ran on four physical
desktops and two VMs, with two consecutive published nightlies, as CI and the signer published them.

| Build | Value |
| --- | --- |
| N | [`v26.1022.1017-nightly`](https://github.com/patrick-hudson/put.gd-desktop/releases/tag/v26.1022.1017-nightly), commit `3582b30a`. Packages `26.1022.1017~nightly`. |
| N+1 | [`v26.1022.1019-nightly`](https://github.com/patrick-hudson/put.gd-desktop/releases/tag/v26.1022.1019-nightly), commit `5ba9de0e`. AppImage SHA-256 `e5b7ad2caecaa7f5737bdfb5d2f0fd4e3c74ce5d98a8a68142caf4efd43525bc` (= `SHA256SUMS`). |
| Pre-#428 hop (f44 only) | [`v26.1022.996-nightly`](https://github.com/patrick-hudson/put.gd-desktop/releases/tag/v26.1022.996-nightly), commit `1887c4c9`. It predates #428 (`1908e5f0`), so it was N only for an extra 996 → 1017 hop. |
| Lab stable (U6 lab, U10, U14 stable) | Lab-key builds of `3582b30a` published under `pkg.put.gd/lab/` only: L1 `26.1019.1019-nightly`, S1 `26.1018.1020`, S2 `26.1020.1021`, L2 `26.1021.1022-nightly`, S3 `26.1021.1023` (AppImage). |

Packages and AppImages came from the GitHub releases, each checked against its `SHA256SUMS`
([lab/fetch_assets.sh](lab/fetch_assets.sh)). The lab builds were made by [lab/lab_build.sh](lab/lab_build.sh)
(native packages on each builder, the AppImage in a podman container on f44). Like #386's lab packages they
were built with `-DPUTGD_BUNDLE_PROTON=OFF`, because the Proton helper is built from source with Node/npm,
which the builders don't have. The helper plays no part in updates.

## Results

✅ pass · ❌ fail (issue linked) · — not in the profile for this desktop

Each cell names the build and the run. The run name is the folder under `~/work/put.gd-ua/{app,pkg}/runs/`
on that host, and its `run.json` is in [results/](results/). N = 1017 (`3582b30a`), N+1 = 1019 (`5ba9de0e`).

| Item | f44 Fedora 44 GNOME | d69 Debian 13 Plasma | k72 Kubuntu 26.04 Plasma | m74 Mint 22.3 Cinnamon | Ubuntu 26.04 GNOME (VM) | Aurora (VM) |
| --- | --- | --- | --- | --- | --- | --- |
| U1 infrastructure | ✅ N and N+1 (`u1-1017-start`, `u1-1019-start`, deep on d69), end: VM_U1_END | | | | | |
| U2 apt / dnf | ✅ RPM N → N+1, `dnf upgrade --refresh putgd`, repo file unchanged (`f44-pkg`) | ✅ DEB `debian13` N → N+1, no conffile prompt (`d69-pkg`) | ✅ DEB `ubuntu26.04` (`k72-pkg`) | ✅ DEB `ubuntu24.04` (`m74-pkg`) | ✅ DEB `ubuntu26.04` N from its file, then `apt-get install --only-upgrade putgd` → N+1 (`ubu-4c0235`) | — |
| U3 GUI updater | ✅ GNOME Software lists N → N+1; applied with `pkcon update putgd` (offline whole-system update) (`f44-pkg`); also 996 → 1017 (`f44-hop`) | ✅ GNOME Software lists it; `pkcon update putgd` (`d69-pkg`) | ✅ Discover, only putgd ticked, **Update Selected**, no prompt (`k72-pkg`) | ✅ Update Manager, only putgd ticked; Patrick typed the prompt (`m74-pkg`) | ❌ [#486](https://github.com/patrick-hudson/put.gd-desktop/issues/486): no **Open software updates**. Software Updater (`update-manager`) is installed and, opened by hand, lists the updates; N+1 applied with apt (`ubu-4c0235`) | — |
| U4 Settings, repository install | ✅ N: Nightly read-only, Check now finds N+1, **Open software updates** opens GNOME Software (real click), no Download and install, no notification; N+1: Up to date (`f44-pkg`) | ✅ same, GNOME Software (`d69-pkg`) | ✅ same, Discover (`k72-pkg`) | ✅ same, Update Manager (`m74-pkg`) | ✅ Nightly, N+1 from the repository, Up to date after the upgrade; the missing button is #486 (`ubu-4c0235`) | — |
| U5 upgrade keeps settings and history | ✅ default profile, `config.json` byte-identical, 2 history entries + previews, link opened in Firefox, delete from destination (`f44-pkg`) | ✅ (`d69-pkg`) | ✅ (`k72-pkg`) | ✅ (`m74-pkg`) | — | — |
| U6 DEB/RPM channel, live | ✅ stable empty: nothing installed, Settings explains; back to nightly, file = `putgd-nightly.repo` (`f44-hop`) | ✅ apt prints an error, see [#482](https://github.com/patrick-hudson/put.gd-desktop/issues/482) (`d69-sw`) | ✅ #482 (`k72-sw`) | ✅ #482 (`m74-sw`) | ✅ Other channel commands run by Patrick in the guest (its passwordless sudo covers only `apt-get`): `putgd.sources` = `putgd-stable.sources`, 1017 stays, Settings says there's no stable release yet; back to nightly, the file = `putgd-nightly.sources` byte for byte and N+1 is offered. Both final `apt install` steps hit the guest's unattended-upgrades dpkg lock; an earlier run of the stable one printed #482's error and installed nothing (`ubu-u6b`) | — |
| U6 DEB/RPM channel, lab | ✅ L1 → S1 (wait path, then the downgrade command), GNOME Software lists S2, `dnf upgrade` S1 → S2, back to L2 (`f44-lab`) | ✅ GNOME Software lists S2 (`d69-lab`) | ✅ Discover lists S2 (`k72-lab`) | ✅ Update Manager lists S2 (`m74-lab`) | — | — |
| U7 in-app AppImage update | ✅ zsync 9.8 s; fallback (crashing tool) 8.9 s full download; backup gone after restart (`f44-zs`, `f44-cr`) | ✅ zsync 9.4 s; fallback 14.4 s (`d69-zs`, `d69-cr`) | ✅ zsync 8.7 s; fallback 8.0 s (`k72-zs`, `k72-cr`) | ✅ real clicks on **Download and install** and **Restart put.gd** (`m74-click`); zsync 9.0 s; fallback 8.6 s (`m74-zs`, `m74-cr`) | ✅ real clicks: zsync, restart into N+1, backup removed (`ubu-4c0235`) | ✅ real clicks: zsync, restart into N+1, backup removed (`aur-3e84a1`) |
| U8 appimageupdatetool | ✅ check exit 1 (update), update exit 0 in 2.5 s, = `SHA256SUMS`, N unchanged; needs `SSL_CERT_FILE` (#297) (`f44-aut`) | ✅ 3.7 s (`d69-aut`) | ✅ 2.4 s (`k72-aut`) | ✅ 2.6 s (`m74-aut`) | ✅ check exit 1, update exit 0 in 3.1 s, = `SHA256SUMS`, N unchanged (`ubu-2c94f9`) | ✅ check exit 1, update exit 0 in 2.9 s with `SSL_CERT_FILE` and 3.7 s without (zsync2 found `/etc/ssl/cert.pem`), = `SHA256SUMS`, N unchanged (`aur-b8e4fd`) |
| U9 Gear Lever, AppImageLauncher | — | — | — | — | ✅ Gear Lever 4.6.2 and AppImageLauncher 3.0.0-beta-2: in-app update from the integrator's entry → N+1 at the same path (= `SHA256SUMS`), entry byte-identical, one entry, both tools then report no update; Gear Lever's own update of a second N → N+1. AppImageLauncher's **Update** menu action fails (missing `libQt5QuickWidgets.so.5`, as in #412) (`ubu-4c0235`) | ✅ Gear Lever 4.6.2: the same results as on Ubuntu, including its own update (`aur-3e84a1`) |
| U10 AppImage channel | ✅ live and lab stable empty: no stable release yet; L1 → S1 refused then confirmed; S1 → S2; S2 → L2 (`f44-nostable`, `f44-sw`) | — | — | ✅ same, with the real **Yes** in the older-version dialog (`m74-nostable`, `m74-sw`) | — | — |
| U11 start at login | ✅ in-app update, no restart, real logout/login starts N+1 (`f44-login`); after appimageupdatetool: after appimageupdatetool (N+1 written beside N): login starts N+1, journal "starting …1019… which is newer than …1017…" (`f44-login`, second run) | ✅ (`d69-login`); after appimageupdatetool (N+1 written beside N): login starts N+1, journal "starting …1019… which is newer than …1017…" (`d69-login`, second run) | ✅ (`k72-login`); after appimageupdatetool (N+1 written beside N): login starts N+1 (`k72-login`, second run) | ✅ (`m74-login`); after appimageupdatetool (N+1 written beside N): login starts N+1 (`m74-login`, second run) | ✅ in-app update without a restart, session reboot, autologin starts N+1 from the same path (`ubu-4c0235`) | ✅ the same, KDE session reboot (`aur-3e84a1`) |
| U12 refusals | ✅ folder not writable, other owner, disk full, upload running (`update_install_busy`), restart pending, `not_appimage` (`f44-pkg`), tampered image, unsigned and rewritten `SHA256SUMS` (`f44-ref`, `f44-cr`) | ✅ restart pending, folder not writable (`d69-ref`, `d69-cr`) | ✅ same (`k72-ref`, `k72-cr`) | ✅ all, as f44 (`m74-ref`, `m74-cr`, `m74-pkg`) | — | — |
| U13 discovery | ✅ GNOME Software: search, install N, remove, no prompt (`f44-gs`) | ✅ GNOME Software (Patrick typed root) (`d69-gs`) | ✅ Discover (Patrick typed) (`k72-dc`) | ✅ Software Manager "Putgd" (Patrick typed) (`m74-mi`) | — | — |
| U14 nightly never | ✅ AppImage N, the scheduled check 5–15 min after start: 0 notifications (`f44-sched`); repository N, Check now: 0 (`f44-pkg`) | ✅ (`d69-sched`, `d69-pkg`) | ✅ (`k72-sched`, `k72-pkg`) | ✅ (`m74-sched`, `m74-pkg`) | ✅ scheduled check (01:46Z) found N+1: 0 notifications (`ubu-4c0235`) | ✅ scheduled check (01:49Z) found N+1: 0 notifications (`aur-3e84a1`) |
| U14 stable once per version | ✅ S1 vs S2: one notification (00:58:23Z), none after restart + Check now, S3: one more (01:26Z), click opens the release page in Firefox (`f44-ann`); repository stable: 0 (`f44-lab`) | ✅ repository stable: 0 (`d69-lab`) | ✅ repository stable: 0 (`k72-lab`) | ✅ S2 once (00:57:05Z), S3 once (01:34Z) (`m74-ann`); repository stable: 0 (`m74-lab`) | — | — |
| U15 idle RSS (N / N+1) | U15_F44 | U15_D69 | ✅ AppImage 30.04 / 30.05 MiB (Pss 23.7 / 23.6); DEB 31.84 / 31.80 MiB (Pss 9.1 / 9.1); no TLS libraries mapped (`k72-mem-*`) | ✅ AppImage 29.59 / 29.39 MiB (Pss 23.8 / 23.6); DEB 28.55 / 28.62 MiB (Pss 18.3 / 18.3); no TLS libraries mapped (`m74-mem-*`) | ✅ AppImage 29.41 / 29.21 MiB (Pss 23.0 / 22.8); no TLS libraries mapped (`ubu-4c0235`, `ubu-2c94f9`) | ✅ AppImage 30.05 / 29.82 MiB (Pss 23.4 / 23.6); no TLS libraries mapped (`aur-3e84a1`, `aur-b8e4fd`) |
| U16 cleanup | U16 | | | | | |

Findings filed during the pass:
- **[#481](https://github.com/patrick-hudson/put.gd-desktop/issues/481):** the in-app docs link in
  `src/core/SystemUpdates.cpp` points at `blob/main`, and that page 404s.
- **[#482](https://github.com/patrick-hudson/put.gd-desktop/issues/482):** while stable has no release, apt's
  last "to stable" command prints `E: Release 'stable' for 'putgd' was not found`. The docs say it installs
  nothing. put.gd stays on the nightly and Settings explains, so U6 passes; the message is the finding.

Not filed:
- **d69, host only:** a GNOME Software install hung in `needrestart` (a debconf prompt with no terminal). I
  killed my own transaction's `needrestart` PIDs (2477313, 2477306). This is the host's configuration, not
  put.gd.
- **Updater opening behind put.gd's window** (f44 and m74, first attempt): not reproduced. The causes were a
  resident started over SSH (f44) and an Update Manager window that was already open (m74).

## Method

The drivers are in [lab/](lab/), run on each host over SSH in the real login session's environment:
- [lab/ua_pkg.py](lab/ua_pkg.py) (DEB/RPM, from #386's `acc386.py`)
- [lab/ua_app.py](lab/ua_app.py) (AppImage, from #344's `upd344.py`)
- [lab/u1_infra.py](lab/u1_infra.py) (U1)
- the step scripts: [pkg_n.sh](lab/pkg_n.sh), [app_n.sh](lab/app_n.sh), [refuse_n.sh](lab/refuse_n.sh),
  [login_n.sh](lab/login_n.sh) with [relogin.sh](lab/relogin.sh), [pkg_lab.sh](lab/pkg_lab.sh) and
  [rss_n.sh](lab/rss_n.sh)
- for the lab channel: [lab_publish.py](lab/lab_publish.py), [lab.sh](lab/lab.sh) and
  [lab_verify_empty.py](lab/lab_verify_empty.py)

**Residents**
- Package residents start as transient user units (`systemd-run --user`), not as children of the SSH shell.
  An updater opened from a resident started over SSH can't get polkit authorisation; started this way, it
  behaves as on a desktop launch.
- One resident ran per desktop at a time. Two AppImage residents can't share `~/.local/share/putgd`.
- Private `XDG_CONFIG_HOME`, `XDG_STATE_HOME`, `XDG_CACHE_HOME` and runtime directory, except for U5 and U11,
  which use the default profile.
- In U5 the driver recorded which files existed in the default profile beforehand. It removed only what the
  run created.

**Uploads**
- U5 used the live bucket with key set 2, read into memory only and passed over the session socket. The
  keys were never put on a command line or in a file.
- The objects went under `acceptance/ua/<host>/`, were deleted with **Delete from destination**, and the
  prefix was listed empty ([results/s3-acceptance-ua-after-u5.json](results/s3-acceptance-ua-after-u5.json)).
- U12's busy refusal used the loopback S3 fixture, with a delay rule on PUT.

**Input and links**
- Input was real: the uinput agent (`tools/lab/host_input.py`) on Wayland, `xdotool` on Cinnamon.
- Clicks were refused unless the session's `http`/`https` default was #326's stand-in recorder or a temporary
  Firefox profile ([lab/recorder.py](lab/recorder.py), #415's guard).
- On d69 and k72 the default had been the ChatGPT app. For the link steps the default was pointed at a
  temporary Firefox profile, then restored byte for byte.

**GUI updaters and polkit**
- Installs were scoped to `putgd`.
- GNOME Software's update is a whole-system offline update that can't be limited to one package, so on f44 and
  d69 the picture shows put.gd listed and `pkcon update putgd` applied it (the coordinator's decision).
- Patrick typed the polkit prompts himself. Nothing typed a password.

**Notifications:** U14 watched `org.freedesktop.Notifications` with `dbus-monitor`, and matched put.gd's
own journal lines to it. The repeat test aged `attemptedAt` by 25 h (#303's method), so the next start
ran its scheduled check.

**Lab channel**
- The lab objects were published on the coordinator's go.
- Afterwards 183 objects were deleted (invalidation `I6186UM62WOKFCBQB912TYGLWR`) and `lab/` listed 0 objects.
  Every key answered 404 at 01:37:42Z ([results/d69/lab-404-check.txt](results/d69/lab-404-check.txt)).
- The lab GPG and minisign keys were shredded.

**Load note:** another session's Aurora F45 golden build loaded f44 from 00:20 to 00:57Z. The f44 U7
timings from that window are informational. U15 ran later, on quiet hosts.

## Notes

**VMs** ([results/vm/SUMMARY.md](results/vm/SUMMARY.md))
- Each VM was an overlay of its golden: `gnome-provisioned-20261016` (`563730cc…`) on d69 and
  `aurora-stable-44.20260908.1-v3` (`6a2fb407…`) on f44. Each was retired with `lab.py reset` and `cleanup`
  (receipts in `results/vm/`), and both goldens hashed the same afterwards.
- The Ubuntu golden doesn't trust d69's bootstrap key. For these overlays the key was typed in over VNC
  ([#487](https://github.com/patrick-hudson/put.gd-desktop/issues/487)).
- The guest's passwordless sudo covers only `apt-get`. Patrick added a passwordless sudoers rule to the
  second Ubuntu overlay and ran U6's commands himself; the rule went with the overlay.
- Aurora 44's `uupd` staged an OS update about 18 minutes after the first overlay booted, so that overlay's
  U11 reboot came up on the new image. On the second overlay `uupd.timer` was stopped first.
- AppImageLauncher 3.0.0-beta-2's own **Update** menu action fails with a missing `libQt5QuickWidgets.so.5`
  (as #412 saw on Mint). Its updater binary works, and reported no update after put.gd's.
- In both VMs, U9's in-app update went through `putgd-cli update.install` from the integrator-started
  resident, not a click. With no `appimageupdatetool` on `PATH`, it took the verified full download.
- U11 in the VMs, and on the physical desktops: the run's settings live in a private profile, but the
  resident started at login uses the default profile. So its window shows **Launch at login** unticked and
  "Not checked yet", and on the VMs the 1017 backup stayed beside the image. This comes from the lab method,
  not the product.

**Physical desktops**
- `*-app_n.log` is the first AppImage attempt. Its `zs` and `cr` runs failed with "resident not ready",
  because two AppImage residents shared `~/.local/share/putgd`. `*-app_n2.log` is the valid rerun, one
  resident at a time.
- `*-rss.log` on k72 and m74 measured 1017 only; the 1019 files weren't on those hosts yet.
  `*-rss-1019.log` has the 1019 legs. The package 1017 line appears twice in the first log, and the two
  agree to 0.1 MiB.
- U15's cells give window A (cold idle) medians as N / N+1. Window B (after Check now) is within 0.15 MiB
  of A everywhere.
- U11's second run deleted its run folder before its frame was pulled on k72 and m74, so those two runs are
  evidenced by their login check logs: the image path, version 1019 and the parent process.
- After the logout, put.gd records the login-started resident's SIGTERM as a `signal` unclean exit. That is
  by design ([diagnostics.md](../../diagnostics.md)). The state files the login runs left in the default
  profile were removed one by one; older logs there are not this pass's.

## Pictures

Every picture is a full desktop. Its footer names the item, the desktop, the build (version and commit),
the run and the frame. Boxes mark the evidence.

### Fedora 44 GNOME Wayland (f44, physical)

| Item | Picture | Shows |
| --- | --- | --- |
| U2/U5 | [f44-u5-after-upgrade.png](pictures/f44-u5-after-upgrade.png) | After the upgrade: running N+1, settings and history kept |
| U3 | [f44-u3-gnome-software-lists-putgd.png](pictures/f44-u3-gnome-software-lists-putgd.png) | GNOME Software lists put.gd N → N+1; applied with pkcon update putgd (the GUI update is whole-system offline) |
| U3 hop | [f44-u3-hop-gnome-software-996-1017.png](pictures/f44-u3-hop-gnome-software-996-1017.png) | Pre-#428 hop: GNOME Software lists put.gd 996 → 1017; the old putgd.repo is kept |
| U4 | [f44-u4-settings-n1-available.png](pictures/f44-u4-settings-n1-available.png) | Settings offers the newer nightly from the repository, with Open software updates |
| U5 | [f44-u5-history-link-in-browser.png](pictures/f44-u5-history-link-in-browser.png) | A link from history made before the upgrade loads in the default browser (temporary Firefox profile) |
| U6 lab | [f44-u6lab-gnome-software-s2.png](pictures/f44-u6lab-gnome-software-s2.png) | GNOME Software lists put.gd S1 → S2 from the lab stable repository |
| U6 lab | [f44-u6lab-stable-older-wait.png](pictures/f44-u6lab-stable-older-wait.png) | Repository on stable S1, older than the nightly installed: the updater waits; Settings shows the downgrade command |
| U6 live | [f44-u6-live-stable-empty.png](pictures/f44-u6-live-stable-empty.png) | Channel set to Stable while the live stable channel is empty: Settings says there is no stable release yet |
| U7 | [f44-u7-fallback-full-download.png](pictures/f44-u7-fallback-full-download.png) | Forced fallback (appimageupdatetool crashes): verified full download instead |
| U7 | [f44-u7-zsync-installed.png](pictures/f44-u7-zsync-installed.png) | In-app AppImage update: installed with a zsync update, restart offered |
| U7 | [f44-u7-zsync-restarted.png](pictures/f44-u7-zsync-restarted.png) | After the restart: running N+1, updated with zsync |
| U10 | [f44-u10-back-to-nightly.png](pictures/f44-u10-back-to-nightly.png) | Stable S2 → nightly L2 (Switched from stable) |
| U10 | [f44-u10-no-stable-yet.png](pictures/f44-u10-no-stable-yet.png) | AppImage on Stable while the live stable channel is empty: stays as it is until the first stable release |
| U10 | [f44-u10-s1-to-s2.png](pictures/f44-u10-s1-to-s2.png) | Stable S1 → S2 with a zsync update |
| U11 | [f44-u11-started-at-login-after-aut.png](pictures/f44-u11-started-at-login-after-aut.png) | Second run: appimageupdatetool wrote N+1 beside N; after a real logout and login, login started N+1 |
| U11 | [f44-u11-started-at-login.png](pictures/f44-u11-started-at-login.png) | After a real logout and login the autostart entry started the updated image (fresh default profile shown) |
| U12 | [f44-u12-refuse-diskfull.png](pictures/f44-u12-refuse-diskfull.png) | Refused: not enough free space beside the AppImage |
| U12 | [f44-u12-refuse-foreign.png](pictures/f44-u12-refuse-foreign.png) | Refused: the AppImage belongs to another user |
| U12 | [f44-u12-refuse-readonly.png](pictures/f44-u12-refuse-readonly.png) | Refused: put.gd can't write to the AppImage's folder |
| U12 | [f44-u12-restart-pending.png](pictures/f44-u12-restart-pending.png) | Restart pending: Settings offers Restart; a second install before it is refused (reply in the run log) |
| U12 | [f44-u12-tamper-image.png](pictures/f44-u12-tamper-image.png) | Tampered image: the download doesn't match SHA256SUMS; nothing changed |
| U12 | [f44-u12-tamper-unsigned.png](pictures/f44-u12-tamper-unsigned.png) | Unsigned SHA256SUMS: refused; nothing changed |
| U13 | [f44-u13-gnome-software-installed.png](pictures/f44-u13-gnome-software-installed.png) | Installed from GNOME Software without a prompt |
| U13 | [f44-u13-gnome-software-search.png](pictures/f44-u13-gnome-software-search.png) | GNOME Software search finds put.gd in the nightly repository |
| U14 | [f44-u14-notification-click.png](pictures/f44-u14-notification-click.png) | Clicking the notification opens the release page in the default browser (temporary Firefox; 404 because S3 is a lab tag) |
| U14 | [f44-u14-stable-announced.png](pictures/f44-u14-stable-announced.png) | Stable S3 announced once by a desktop notification |

### Debian 13 Plasma 6.3 Wayland (d69, physical)

| Item | Picture | Shows |
| --- | --- | --- |
| U2/U5 | [d69-u5-after-upgrade.png](pictures/d69-u5-after-upgrade.png) | After the upgrade: running N+1, Dark theme and other settings kept |
| U3 | [d69-u3-gnome-software-opened.png](pictures/d69-u3-gnome-software-opened.png) | Open software updates opened GNOME Software on Updates, put.gd listed (applied with pkcon update putgd) |
| U4 | [d69-u4-settings-n1-available.png](pictures/d69-u4-settings-n1-available.png) | Settings offers the newer nightly from the repository, with Open software updates |
| U5 | [d69-u5-history-link-in-browser.png](pictures/d69-u5-history-link-in-browser.png) | A link from history made before the upgrade loads in the default browser (temporary Firefox profile) |
| U6 lab | [d69-u6lab-gnome-software-s2.png](pictures/d69-u6lab-gnome-software-s2.png) | GNOME Software lists put.gd S1 → S2 from the lab stable repository |
| U6 lab | [d69-u6lab-stable-older-wait.png](pictures/d69-u6lab-stable-older-wait.png) | Repository on stable S1, older than the nightly installed: the updater waits; Settings shows the downgrade command |
| U6 live | [d69-u6-live-stable-empty.png](pictures/d69-u6-live-stable-empty.png) | Channel set to Stable while the live stable channel is empty: there is no stable release yet |
| U7 | [d69-u7-fallback-full-download.png](pictures/d69-u7-fallback-full-download.png) | Forced fallback (appimageupdatetool crashes): verified full download instead |
| U7 | [d69-u7-zsync-restarted.png](pictures/d69-u7-zsync-restarted.png) | After the restart: running N+1, updated with zsync |
| U11 | [d69-u11-started-at-login-after-aut.png](pictures/d69-u11-started-at-login-after-aut.png) | Second run: appimageupdatetool wrote N+1 beside N; after a real logout and login, login started N+1 |
| U12 | [d69-u12-refuse-readonly.png](pictures/d69-u12-refuse-readonly.png) | Refused: put.gd can't write to the AppImage's folder |
| U13 | [d69-u13-gnome-software-installing.png](pictures/d69-u13-gnome-software-installing.png) | Installing from GNOME Software (after the root password Patrick typed) |
| U13 | [d69-u13-gnome-software-search.png](pictures/d69-u13-gnome-software-search.png) | GNOME Software search finds put.gd in the nightly repository |

### Kubuntu 26.04 Plasma 6.6 Wayland (k72, physical)

| Item | Picture | Shows |
| --- | --- | --- |
| U3 | [k72-u3-discover-update-selected.png](pictures/k72-u3-discover-update-selected.png) | Discover: only putgd ticked, Update Selected, installing N → N+1 without a prompt |
| U4 | [k72-u4-settings-n1-available.png](pictures/k72-u4-settings-n1-available.png) | Settings offers the newer nightly from the repository, with Open software updates |
| U5 | [k72-u5-history-link-in-browser.png](pictures/k72-u5-history-link-in-browser.png) | A link from history made before the upgrade loads in the default browser (temporary Firefox profile) |
| U6 lab | [k72-u6lab-discover-s2.png](pictures/k72-u6lab-discover-s2.png) | Discover lists putgd S1 → S2 from the lab stable repository |
| U6 lab | [k72-u6lab-stable-older-wait.png](pictures/k72-u6lab-stable-older-wait.png) | Repository on stable S1, older than the nightly installed: the updater waits; Settings shows the downgrade command |
| U6 live | [k72-u6-live-stable-empty.png](pictures/k72-u6-live-stable-empty.png) | Channel set to Stable while the live stable channel is empty: there is no stable release yet |
| U7 | [k72-u7-fallback-full-download.png](pictures/k72-u7-fallback-full-download.png) | Forced fallback (appimageupdatetool crashes): verified full download instead |
| U7 | [k72-u7-zsync-restarted.png](pictures/k72-u7-zsync-restarted.png) | After the restart: running N+1, updated with zsync |
| U11 | [k72-u11-started-at-login.png](pictures/k72-u11-started-at-login.png) | After a real logout and login the autostart entry started the updated image (fresh default profile shown) |
| U12 | [k72-u12-refuse-readonly.png](pictures/k72-u12-refuse-readonly.png) | Refused: put.gd can't write to the AppImage's folder |
| U13 | [k72-u13-discover-installed.png](pictures/k72-u13-discover-installed.png) | Installed from Discover (prompts typed by Patrick) |
| U13 | [k72-u13-discover-search.png](pictures/k72-u13-discover-search.png) | Discover search finds put.gd in the nightly repository |

### Linux Mint 22.3 Cinnamon X11 (m74, physical)

| Item | Picture | Shows |
| --- | --- | --- |
| U3 | [m74-u3-update-manager-putgd-only.png](pictures/m74-u3-update-manager-putgd-only.png) | Update Manager with only putgd ticked (1 update selected); installed after Patrick typed the prompt |
| U4 | [m74-u4-settings-n1-available.png](pictures/m74-u4-settings-n1-available.png) | Settings offers the newer nightly from the repository, with Open software updates |
| U5 | [m74-u5-history-link-in-browser.png](pictures/m74-u5-history-link-in-browser.png) | A link from history made before the upgrade loads in the default browser (temporary Firefox profile) |
| U6 lab | [m74-u6lab-stable-older-wait.png](pictures/m74-u6lab-stable-older-wait.png) | Repository on stable S1, older than the nightly installed: the updater waits; Settings shows the downgrade command |
| U6 lab | [m74-u6lab-update-manager-s2.png](pictures/m74-u6lab-update-manager-s2.png) | Update Manager lists putgd S2 from the lab stable repository |
| U6 live | [m74-u6-live-stable-empty.png](pictures/m74-u6-live-stable-empty.png) | Channel set to Stable while the live stable channel is empty: there is no stable release yet |
| U7 | [m74-u7-click-installed.png](pictures/m74-u7-click-installed.png) | Real click on Download and install: installed with a zsync update, restart offered |
| U7 | [m74-u7-click-restarted.png](pictures/m74-u7-click-restarted.png) | Real click on Restart put.gd: running N+1 |
| U7 | [m74-u7-fallback-full-download.png](pictures/m74-u7-fallback-full-download.png) | Forced fallback (appimageupdatetool crashes): verified full download instead |
| U10 | [m74-u10-back-to-nightly.png](pictures/m74-u10-back-to-nightly.png) | Stable S2 → nightly L2 (Switched from stable) |
| U10 | [m74-u10-no-stable-yet.png](pictures/m74-u10-no-stable-yet.png) | AppImage on Stable while the live stable channel is empty: stays as it is until the first stable release |
| U10 | [m74-u10-older-confirm.png](pictures/m74-u10-older-confirm.png) | Switching to an older stable asks first; Yes clicked for real |
| U10 | [m74-u10-s1-running.png](pictures/m74-u10-s1-running.png) | Switched from nightly L1 to stable S1 with a zsync update |
| U11 | [m74-u11-started-at-login.png](pictures/m74-u11-started-at-login.png) | After a real logout and login the autostart entry started the updated image (behind the Mint welcome screen) |
| U12 | [m74-u12-tamper-sums.png](pictures/m74-u12-tamper-sums.png) | Tampered SHA256SUMS signature: refused; nothing changed |
| U13 | [m74-u13-software-manager-search.png](pictures/m74-u13-software-manager-search.png) | Software Manager search finds put.gd in the nightly repository |
| U14 | [m74-u14-notification-click.png](pictures/m74-u14-notification-click.png) | Clicking the notification opens the release page in the default browser (temporary Firefox; 404 because it is a lab tag) |
| U14 | [m74-u14-stable-announced.png](pictures/m74-u14-stable-announced.png) | Stable S2 announced once by a desktop notification |

### Ubuntu 26.04 GNOME Wayland (VM, overlay of gnome-provisioned-20261016 on d69)

| Item | Picture | Shows |
| --- | --- | --- |
| U2/U4 | [ubu-u4-up-to-date.png](pictures/ubu-u4-up-to-date.png) | apt --only-upgrade putgd took N → N+1; after a restart Settings says Up to date |
| U3 | [ubu-u3-software-updater-by-hand.png](pictures/ubu-u3-software-updater-by-hand.png) | Ubuntu's Software Updater (update-manager), opened by hand, offers the updates |
| U4/U3 | [ubu-u4-no-open-software-updates.png](pictures/ubu-u4-no-open-software-updates.png) | Repository install offers N+1, but there is no Open software updates button: Software Updater is not recognised (#486) |
| U6 live | [ubu-u6-live-back-to-nightly.png](pictures/ubu-u6-live-back-to-nightly.png) | Back to nightly: putgd.sources = putgd-nightly.sources byte for byte; N+1 offered again |
| U6 live | [ubu-u6-live-stable-empty.png](pictures/ubu-u6-live-stable-empty.png) | "To stable" commands run by Patrick: nothing installs, Settings says there's no stable release yet |
| U7 | [ubu-u7-zsync-installed.png](pictures/ubu-u7-zsync-installed.png) | Real click on Download and install: installed with a zsync update, restart offered |
| U7 | [ubu-u7-zsync-restarted.png](pictures/ubu-u7-zsync-restarted.png) | Real click on Restart put.gd: running N+1 |
| U9 | [ubu-u9-appimagelauncher-no-update.png](pictures/ubu-u9-appimagelauncher-no-update.png) | AppImageLauncher's updater on the integrated image after put.gd's update: no updates found |
| U9 | [ubu-u9-gear-lever-entry.png](pictures/ubu-u9-gear-lever-entry.png) | Started from Gear Lever's menu entry after the in-app update: N+1 at the same path |
| U11 | [ubu-u11-started-at-login.png](pictures/ubu-u11-started-at-login.png) | After an in-app update without a restart and a session reboot, login started N+1 (fresh default profile shown) |

### Aurora 44 Plasma (VM, overlay of aurora-stable-44.20260908.1 on f44)

| Item | Picture | Shows |
| --- | --- | --- |
| U7 | [aur-u7-zsync-installed.png](pictures/aur-u7-zsync-installed.png) | Real click on Download and install: installed with a zsync update, restart offered |
| U7 | [aur-u7-zsync-restarted.png](pictures/aur-u7-zsync-restarted.png) | Real click on Restart put.gd: running N+1 |
| U9 | [aur-u9-gear-lever-entry.png](pictures/aur-u9-gear-lever-entry.png) | Started from Gear Lever's menu entry after the in-app update: N+1 at the same path |
| U11 | [aur-u11-started-at-login.png](pictures/aur-u11-started-at-login.png) | After an in-app update without a restart and a session reboot, login started N+1 (fresh default profile shown) |
