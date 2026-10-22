# AppImage full acceptance rerun: plan (phase 1)

Session `appimage-acceptance-rerun` (`local_e2669089-cf51-4430-94d7-9f3f35a3ee46`), worktree
`appimage-acceptance-rerun-911312`, reset onto `origin/nightly` `33c768ec`. Working plan only; the
evidence README replaces it in the docs PR.

## Build under test

- **N:** the first published nightly whose commit contains all four fix merges: #348+#349 (updater polish), #341 (shortcuts while a menu is open), #333 (Plasma region size) and #344's AppImage channel switch. The coordinator names the tag; until then nothing runs on a desktop.
- **Identity checks, on each desktop:**
  - The AppImage comes from the GitHub release.
  - Its SHA-256 matches `SHA256SUMS`, and `SHA256SUMS.minisig` verifies with the nightly key.
  - `git merge-base --is-ancestor` confirms each of the four merge commits is in N's commit.
- **N-1:** the newest published nightly before N that already has the in-app updater (#343, so 889 or later; today 893). It's used only for the live N-1 → N in-app update.
- **N+1 (requested):** a later nightly, dispatched by the coordinator after N is published, as for 889 → 893. Without it, N's own updater code (#349's cleaned zsync output and #348's "Updated from…" first open) can't install anything from the live feed. Fallback without N+1: #349 is proven by `update.status` on N (no control characters) plus the PR's unit tests and lab evidence, and recorded as "N→N+1 not run".

## Desktops and placement

| Desktop | Placement | Input | Wall |
| --- | --- | --- | --- |
| f44 Fedora 44 GNOME 50 Wayland | physical `.70`, `putgd-gnome50-<N>.zip` as a user copy (re-login #1 loads it) | uinput agent | 5970 |
| d69 Debian 13 Plasma 6.3 Wayland | physical `.69` | uinput agent | 5979 |
| k72 Kubuntu 26.04 Plasma 6.6 Wayland | physical `.72` | uinput agent | 5972 |
| m74 Mint 22.3 Cinnamon X11 | physical `.74` | xdotool | 5974 |
| Ubuntu 26.04 GNOME 50 Wayland VM | overlay of `gnome-provisioned-20261016` (`563730cc…`, d69 `transfer-0d2922e34f4b`), QEMU on **d69** (31 GiB RAM), 2 vCPU/6 GiB, SSH 22341, VNC 5941 | guest QEMU VNC | ws 5991, unit `putgd-vm-ubuntu-afa2` |
| Aurora 44 Plasma 6.7 Wayland VM | overlay of `aurora-stable-44.20260908.1-v3` (`6a2fb407…`), UEFI, QEMU on **f44**, 2 vCPU/6 GiB, SSH 22426, VNC 5926 | guest QEMU VNC (`10.0.2.2:5926` from the guest) | ws 5992, unit `putgd-vm-aurora-afa2` |

**VM rules:**
- Overlays only, through `tools/lab/lab.py launch` from the canonical `~/work/put.gd` checkout on the host. Goldens are never booted or written.
- Hash both goldens **before** launch and **after** retirement.
- Check that the SSH/VNC ports are free on the host first. Check every socket path with `check_socket_paths.py`.

**Ubuntu overlay prep, as in 842:**
- `apt-get remove putgd` (the golden carries 0.1.1~dev.63 and its system extension).
- Add `gstreamer1.0-gtk3` for the cover pattern.
- Install the release's GNOME extension as a user copy, then reboot (autologin).

**Aurora:** nothing installed; FUSE from the image; the cover from `lab/gtk_pattern.py`.

**Wall tiles:**
- One `systemd-run --user` SSH tunnel per VM, as in docs/remote-vm-displays.md.
- Back up `tools/qemu-grid/remote-displays.json` privately, append `{"name": "Ubuntu 26.04 GNOME VM afa2 (d69)", "port": 5991}` and `{"name": "Aurora 44 KDE VM afa2 (f44)", "port": 5992}` atomically, and check **VNC connected** on the wall.
- At the end, remove only those two objects and stop both units.

**Retirement:**
- `lab.py reset`, then `cleanup --evidence-manifest` with an explicit completion manifest (dry run first).
- Re-hash the goldens, and record that the units and tiles are gone.

## Schedule (after the go)

Locks are requested per desktop from the coordinator. I expect all four physical desktops plus the VM hosts at once. `desktop_lock.py acquire` must succeed before anything touches a desktop. Locks are 45 min, renewed with `extend` (heartbeat doesn't extend), and released `--verified-clean`.

| Step | f44 | d69 | k72 | m74 | VMs |
| --- | --- | --- | --- | --- | --- |
| T+0 | identity, stage tools, install extension, **logout #1** (coordinator OK) | identity, stage | identity, stage | identity, stage | hash goldens |
| T+10 | A, B, **C** (quiet host, after a 60 s settle) | A, B, **C** | A, B, **C** | A, B, **C** | (no VM running yet, so d69/f44 stay quiet for C) |
| T+20 | | launch the Ubuntu overlay after d69's C | | D–H, J, #341 | launch Aurora after f44's C; tunnels, tiles |
| T+20–80 | D–H, J | D–H, J | D–H, J | **I** logout | VM prep, then A–K in each guest (C inside the guest) |
| T+80 | **I** logout #2, K | **I** logout (temp `Relogin=true` drop-in), K | **I** logout, K | K | I by guest reboot, K |
| end | live N-1→N (and N→N+1) on all six; cleanup; release `--verified-clean` | | | | retire overlays, re-hash goldens, stop tunnels, remove tiles |

**Ordering and quiet windows:**
- The physical desktops are separate machines, so their C windows run in parallel and the driving interleaves.
- No builds on a host during its C or E windows. I record the load at the start and end of each.
- If the coordinator grants fewer desktops at once, the order is m74 (it has #341) → k72 → d69 → f44, as in 842.

**Logouts:**
- Every logout needs the coordinator's OK and a check that none of Patrick's apps are open.
- Patrick's standing authority allows closing his Firefox and ChatGPT/Codex windows. Each closure gets a timestamp in the timeline.
- Disable GNOME idle suspend checks on f44 beforehand: the 842 pass lost 4 minutes to a suspend.

## Per-item checks (afa.py as in 842, plus `rerun.py`)

Run root on each host: `~/work/put.gd-afa2/` (the tool, runs and `upd/`). It's the only path removed at the end. The private profile and loopback S3 fixture are as in 842.

| Item | What's run | New since 842 |
| --- | --- | --- |
| A | `afa.py identity N SHA256SUMS TAG`, `versions` | minisig verified; merge commits of the four fixes in N |
| B | `start`, `processes`, `tray-menu`, find/click the tray, `window`, `switcher` | — |
| C | `memory --seconds 180` | Compared with 842's 28.9–29.8 MiB. On X11, check the #341 XI2 raw-key listener adds no notable RSS. |
| **D** | `capture region/window/desktop`, `edit --dialog`, clipboard | **#333:** `rerun.py region-size` with a 80×60 and a 1120×680 drag (980×600 in the 1280×800 VMs) on **every** desktop. PNG = drag size. Must hold on d69 (KWin 6.3) and k72 (KWin 6.6), with Aurora (6.7) as the control. |
| E | `record`, `control` | — (motion vs. a static control, encoder, KWin entry) |
| **F** | `i326.py handler-on` (the real `mimeapps.list`, restored byte for byte), `afa.py notify`, `i326.py handler-off` | **#326/#330:** a real click on the banner. The recorder logs the URL with `lib=unset appdir=unset`, and the resident logs `Opened on the host` `route: portal`. Picture of the recorder dialog. Must pass on all six; it failed on 5/6 at 842. |
| G | `history`, `delete` | — |
| **H** | `settings` (theme switch, `update.status`, Check now) | **#329/#334:** Check now **ok** on stock f44 (no TLS error), and everywhere else. **#348:** `rerun.py updates-first-open` on a resident whose Settings was never shown; **one** `ui.show updates`, then a full frame showing the Updates rows. **#349:** `rerun.py status-clean`. **#344:** `rerun.py channel --to stable`, then the PR's documented testable flow (below), then `--to build`. |
| I | `login enable`, real logout/login, `login check` | Also checks that the login script still starts N after the channel round trip (#344 asks about start at login). |
| **J** | `rerun.py update-check` with appimageupdatetool (continuous; 842 and #343 used `8d17a50e…`; re-download from AppImageCommunity/AppImageUpdate and record the hash) | **#297:** on f44 run it bare (the expected upstream segfault, recorded as known) and with `SSL_CERT_FILE=/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem` (the correct answer = pass). **#343 in-app, live:** `upd293.py setup --live --scenario a` with N-1, then `check` → `install` → `restart`. The image ends with N's `SHA256SUMS` hash, the restart reports N, Settings says "Updated from N-1 to N", and pictures name both builds. **N → N+1** the same way, if N+1 exists, plus `status-clean` on N's own install record (#349) and a single `ui.show updates` after the restart (#348). |
| K | `quit-tray`, `leftovers`, `cleanup` | Also: the real `mimeapps.list` matches its backup, and no `~/.local/share/putgd` or autostart entry is left. |
| **#341** | `rerun.py menu-shortcut` | **m74 (required):** with put.gd's tray menu open (real right click), Ctrl+Shift+N for desktop and window must give a capture holding the menu (`menuInCapture` ≥ 0.9), and region must start the selector (80×60 drag → 80×60) or show a clear notice. Then the same with another client's menu (Cinnamon's main menu: real click on the panel menu applet). **Every other desktop:** desktop capture with the tray menu open, recorded as pass or "unaffected/affected" against what the #341 PR documents for Plasma and GNOME. Shortcuts come from `settings.get` (`actions.<a>.shortcut`), and the keys are real input (xdotool/uinput/VNC). |

### #344 channel switch

- **Minimum:**
  - On the nightly build, switch to **Stable** with the live `stable.json` (`release: null`). Settings must show the "no stable release yet" state, with no error and no notification. Record `update.status` and a picture.
  - Switch back to **This build's channel**, and check that status returns to nightly.
- **Beyond the minimum:** whatever the #344 PR documents as testable without live publishing, likely a lab or loopback stable feed through `PUTGD_UPDATE_FEED_URL`. I read the PR's "how to test" when the tag arrives, and record a written reason for anything it can't cover.
- **Never:** no publishing to `app.put.gd`, `pkg.put.gd/releases` or any live channel. A lab prefix is used only if the PR names it and the coordinator agrees.

## Pictures

Annotated full-desktop frames, captured fresh from N with its version and SHA in each caption, using `annotate_all.py` extended for the new items. Per desktop:
- the tray menu;
- Settings on Updates after the first open (#348);
- Check now result;
- the channel set to Stable ("no stable release yet");
- a capture in the editor;
- the native Save a copy… chooser;
- the Review window;
- the link-ready notification and the recorder's "link opened" dialog (#326).

Additionally:
- m74: the tray menu open with the capture holding it (#341);
- k72 and d69: the region-size result (#333);
- the in-app update found → zsync → "Updated from N-1 to N", on f44 and m74 at least.

Every frame is checked before it's kept. Physical desktops are sanitized (full frames OK), and the VMs are always OK.

## Timeline and host actions

- **Per host:** `results/<desk>/timeline.txt` (UTC), covering:
  - lock acquire, extend and release;
  - app closures, extension install, every logout and login (method and times), drop-ins added and removed;
  - VM launch and retirement, tunnel start and stop, tile add and remove;
  - the golden hashes before and after.
- **Pass-wide:** a timeline in the shared notes folder (`appimage-acceptance-rerun.md`).

## Cleanup and proof

- Per desktop: put.gd quit from the tray, then `leftovers` empty.
- Removed: `~/work/put.gd-afa2`; the extension user copy (f44, `enabled-extensions` restored); `mimeapps.list` restored and compared; the temporary SDDM drop-in on d69.
- Locks released with `--verified-clean`.
- VMs: overlays retired, tunnels stopped, tiles removed, goldens re-hashed as unchanged.
- Workstation: only the evidence copies.

## Deliverables

- **Docs-only PR into `nightly`:** `docs/evidence/2026-10-22-appimage-full-acceptance-rerun/`, with:
  - README, the item × desktop table, versions, memory, recording, and a section per fix;
  - results, lab scripts and pictures, embedded by docs commit SHA;
  - a link from the acceptance-matrix profile to the second pass.
- **Issues:** one GitHub issue per new finding.
- **Report:** a message to the coordinator. The coordinator merges.

## Asks to the coordinator (in the ready message)

1. The N tag once all four fixes are in a published nightly.
2. An N+1 nightly after N, for N's own updater path (#348/#349), or a decision to skip it.
3. All four physical locks plus d69/f44 for the VMs, and OK for the logouts: f44 ×2, d69/k72/m74 ×1.
4. Which #344 flow is testable without live publishing, if its PR doesn't say.

## Coordinator's answers (2026-10-22)

1. N comes once all four PRs have merged and a nightly containing them has published (expected in hours). Until then: idle, no locks.
2. N+1 will be dispatched right after N publishes; both tags will be sent. Run the live N → N+1 update once N+1 is published.
3. On the go, all four builders plus both VMs (Ubuntu on d69, Aurora on f44) are mine for the pass. The logouts (f44 ×2, d69/k72/m74 ×1) are covered by Patrick's builder authority: no further ask, but each is recorded in the timeline.
4. #344: Patrick wants the stable side tested through a lab-signed feed under `pkg.put.gd/lab` (lab keys only). If the #344 PR leaves that feed in place, ask the coordinator before using it. Otherwise, "no stable release yet" plus switching back is enough.
