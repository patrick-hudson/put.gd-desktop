# Aurora (Universal Blue) VM layer for AppImage and Flatpak testing

The new visibility-helper provisioning below requires the companion
[code PR #584](https://github.com/patrick-hudson/put.gd-desktop/pull/584).
This documentation-only PR is intended to merge after that code. Stage the
bootstrap script and `guest_idle.py` from the same code revision; the older
nightly script does not install the new helper. Existing golden bytes are unchanged.


Aurora is Universal Blue's KDE Plasma desktop, an image-based (bootc) Fedora
Atomic variant: `ghcr.io/ublue-os/aurora:stable`. It installs nothing through a
package manager, ships Flathub as its only Flatpak remote, and represents the
user population that will run put.gd as an AppImage or a Flatpak rather than a
DEB or RPM. It has no row in the package matrix, but the Fedora 45 guest below
is a required desktop of the
[AppImage full-acceptance profile](acceptance-matrix.md#appimage-full-acceptance-profile).

The guest was provisioned on the Fedora 44 builder `f44` (`192.168.68.70`) on
2026-10-19. Private receipts, screenshots and logs are in the workstation's
`work/aurora-lab/2026-10-19-aurora-f44/` and on the builder under
`~/work/put.gd/work/aurora-lab/`.

## Source image

| Item | Value |
| --- | --- |
| Download | `https://dl.getaurora.dev/aurora-stable-x86_64.iso` (Intel/AMD); `aurora-nvidia-open-stable-x86_64.iso` for Nvidia. The published checksum is the same URL plus `-CHECKSUM`. Older `aurora-stable.iso` and `-live.iso` names return 404. |
| ISO on `f44` | `~/work/put.gd/work/lab-bootstrap-f44/aurora-iso/aurora-stable-x86_64.iso` (read-only), 7,133,960,192 bytes, server date 2026-09-14 |
| ISO SHA-256 | `5050ff4c96d678f15cdb69ab4d5ca6f03eeb99d264f97011de4eceede90bb9e0` (matches the published `-CHECKSUM`) |
| Installer | Live Plasma session with the Anaconda web UI, version `44.20260908.1`. It has no user step; the first boot runs Plasma Initial Setup. |
| Firmware | UEFI only. Boot QEMU with `/usr/share/edk2/ovmf/OVMF_CODE_4M.qcow2` read-only and a private copy of `OVMF_VARS_4M.qcow2` as the two pflash drives. Secure Boot is off, so no MOK enrollment is needed. |

The installer deploys the embedded container image to the disk. With 3 vCPUs
and 8 GiB on `f44` the software-installation step took about 45 minutes and is
CPU-bound; disk growth stalls for minutes near the end while it builds the
initramfs and relabels. Check the QEMU process CPU and the overlay's size on the
host before calling it hung.

## Goldens on `f44`

The four original goldens below are standalone qcow2 files (virtual 48 GiB) under
`~/work/put.gd/work/lab-images/`, each with `source.manifest.json`,
`image-info.json`, the pinned guest `known_hosts` and the bootstrap inputs.
The [immutable-golden rules](vm-lab.md#base-protection-and-inventory) apply.
The three Fedora 44 goldens exist only on `f44` (no Debian or S3 copy, so the
[two-copies rule](remote-golden-images.md) is not satisfied for them); the
older Fedora 45 disk has a verified Debian copy but no complete S3 image.
The independently rebuilt Fedora 45 cloud golden has verified Fedora, Debian
and S3 copies; see [cloud golden and Boat snapshot](remote-golden-images.md#aurora-fedora-45-cloud-golden-and-boat-snapshot).

| Directory | Base | SHA-256 | State |
| --- | --- | --- | --- |
| `aurora-testing-45.20261022.1-852d34e885c1` | Fedora 45 (`aurora:testing`) | `33fedf977f1e0cb7e34b944700f1959f4da5c708fa85dbbc9879077af1278556` | **Use this one for acceptance.** v3 rebased to `testing-45.20261022.1`, updater timer masked, single deployment; 24.0 GB allocated. See [Fedora 45 golden](#fedora-45-golden). |
| `aurora-stable-44.20260908.1-v3-492c3f8945d1` | Fedora 44 (`aurora:stable`) | `6a2fb407830f95228bb06f4d20d7cd0bd1aec8ed937ef59430e50eb608b62c2d` | The Fedora 44 base: v2 plus lab idle settings (no screen lock, dimming, display power-off or suspend). Its `uupd.timer` is still enabled; see [automatic updates](#automatic-updates). 15.4 GB allocated. |
| `aurora-stable-44.20260908.1-v2-af46889e2a54` | Fedora 44 | `b9ee08f09d8a4fbd4e57de04c8c1b6e9833400685492a1ab58a341d5e76dd7e7` | v1 plus `/etc/plasmalogin.conf.d/putgd-lab-autologin.conf`; autologin verified (`Service=plasmalogin-autologin`, Wayland user session). |
| `aurora-stable-44.20260908.1-f9c006a376e1` | Fedora 44 | `65cc3ae1cfb91dc5054ff2c4c50b82e8dddeed8ba53d6c29fd16282b1a0f1811` | First boot plus bootstrap. Its SDDM autologin drop-in is ignored (Aurora uses plasmalogin), so it boots to the greeter. |

Guest facts recorded from v1: Aurora 44.20260908.1 (`OSTREE_VERSION`), bootc
booted image `ghcr.io/ublue-os/aurora:stable` built 2026-09-08, kernel-side
Fedora 44 userland with glibc 2.43, Qt 6.11.2 (`qt6-qtbase`, `qt6-qtwayland`),
Plasma 6.7.4 and KWin 6.7.4 on Wayland, `plasma-login-manager` 6.7.4 (no SDDM),
flatpak 1.18.2 with the `flathub` system remote and Universal Blue's curated
system Flatpaks preinstalled, fuse 2.9.9 and fuse3 3.18.2 libraries present,
`libappimage` absent. `/` is a read-only composefs; writable state is under
`/var` (`/var/home/lab` is the home directory).

Guest login is the [standard remote lab login](remote-golden-images.md#standard-remote-guest-login),
`lab` / `putgd-lab`, hostname `aurora-lab`. `lab` is in `wheel`. The bootstrap
key is the builder-private `~/work/put.gd/work/lab-bootstrap-f44/aurora/identity`
(its public key sits in each golden directory); the guest's Ed25519 host key
(`SHA256:yeRsgHkd8AI0lIuNZ4H6YE2i4d1D7cxeFI86/uKoWiA`) was compared against the
console before pinning. The sudo rule grants `lab` only `rpm-ostree`, `bootc`,
`flatpak`, `systemctl`, `loginctl` and `journalctl` without a password.

### How they were made

1. Fresh 48 GiB qcow2, the ISO as an IDE CD with `bootindex=0`, pflash as
   above, `virtio-vga`, `usb-tablet`, user networking with
   `hostfwd=tcp:127.0.0.1:22425-:22`, `-vnc 127.0.0.1:25`, QMP socket,
   `-daemonize`. The exact command is `qemu-install.cmd` beside the golden.
2. Installer: English, automatic time, use entire disk, no encryption. The
   live session ignores QMP `system_powerdown`; after "Successfully installed"
   the disk is quiescent and QMP `quit` is safe.
3. First boot without the ISO: Plasma Initial Setup created `lab`, hostname
   `aurora-lab`, America/Chicago.
4. A 16 MiB FAT seed (`PUTGDLAB`) carried the public key and
   [`tools/lab/bootstrap-aurora-guest.sh`](../tools/lab/bootstrap-aurora-guest.sh),
   with the current `tools/lab/guest_idle.py` companion beside `bootstrap.sh`;
   run from Konsole as `sudo mount /dev/disk/by-label/PUTGDLAB /mnt; sudo sh /mnt/bootstrap.sh /mnt/identity.pub`.
   The generic `bootstrap-guest.sh` does not fit: Aurora has no `dnf` and its
   display manager is plasmalogin. The committed script already writes the
   plasmalogin drop-in and installs the common visibility autostart helper.
   Use the [verified guest preflight](vm-lab.md#visible-guest-preflight) before acceptance.
   This describes new provisioning; existing golden bytes remain unchanged.
5. Clean `systemctl poweroff` over SSH, `qemu-img check`, `qemu-img convert`
   to a new standalone image, `qemu-img compare` against the source, SHA-256,
   mode 0444, manifest.

### Automatic updates

Universal Blue ships `uupd.timer`, which starts `uupd.service` about two
minutes after boot; it runs `bootc upgrade` and pulls whatever the booted tag
currently points at (3.7 GB, about ten minutes of CPU and network on `f44`)
into the overlay. `sudo bootc status` blocks while it runs. Every overlay of the
Fedora 44 goldens does this, so a run on v3 that needs a quiet guest, or an
exact base, should stop it right after SSH answers:

```sh
sudo -n systemctl stop uupd.service; sudo -n systemctl disable --now uupd.timer
```

The Fedora 45 golden has the timer disabled and masked, so its overlays stay on
the base they were made from.

## Fedora 45 golden

`aurora-testing-45.20261022.1-852d34e885c1` is the Aurora guest on a Fedora 45
base. It was made on 2026-10-23 (UTC) from an overlay of the v3 golden, because
on 2026-10-22 the `stable` and `latest` Aurora tags still pointed at Fedora 44
(`44.20261022.1`) and only `ghcr.io/ublue-os/aurora:testing` carried Fedora 45.
Issue [#478](https://github.com/patrick-hudson/put.gd-desktop/issues/478) was
reproduced on a hand-rebased copy of this base, which is why the Fedora 45
guest is a required acceptance target. Provisioning receipts are on `f44` under
`~/work/put.gd/work/aurora-lab/prov-f45/` (`launch.log`, `bootc-switch.log`,
`verify.log`, `facts.json`, the scripts and `golden-sha256sums.txt`); the
golden directory holds `provisioning-verify.log`.

| Item | Value |
| --- | --- |
| Image | `ghcr.io/ublue-os/aurora:testing`, version `testing-45.20261022.1`, digest `sha256:969e4f6f164742b9f3f85e74b958faf88e357b65620e7c2f1cc28723948a9930`, built 2026-10-22T03:21:25Z. The `testing` tag is unsigned (`bootc status` shows no `containerPolicy` signature, unlike `stable`). |
| Guest | `VERSION_ID=45`, kernel `7.2.11-300.fc45`, `plasma-workspace-6.7.5-2.fc45`, `kwin-6.7.5-1.fc45`, `plasma-login-manager-6.7.5-1.fc45`, `qt6-qtbase-6.11.2-3.fc45`, `qt6-qtwayland-6.11.2-1.fc45`, glibc 2.44, flatpak 1.18.4 with the `flathub` system remote. |
| Session | plasmalogin autologin as `lab` (`Service=plasmalogin-autologin`, `Type=wayland`, `Desktop=KDE`, `Class=user`), `kwin_wayland` with Xwayland, `plasmashell`. |
| Lab settings | The v3 idle settings survived the rebase (`kscreenlockerrc` `Autolock=false`, `LockOnResume=false`; `powerdevilrc` no dimming, DPMS or auto-suspend for AC, Battery and LowBattery). `uupd.timer` disabled and masked. |
| Deployments | Single: the Fedora 44 rollback deployment was removed with `rpm-ostree cleanup -r` (5.7 GB freed) before the poweroff. |
| SSH | Host key unchanged (`SHA256:yeRsgHkd8AI0lIuNZ4H6YE2i4d1D7cxeFI86/uKoWiA`); `known_hosts` pins `[127.0.0.1]:22426` and `[127.0.0.1]:22427`. Only the builder bootstrap key `~/work/put.gd/work/lab-bootstrap-f44/aurora/identity` is authorized; the run-private key that `launch` appends was removed before the poweroff. |
| Golden | standalone qcow2, 23,965,663,232 bytes, SHA-256 `33fedf977f1e0cb7e34b944700f1959f4da5c708fa85dbbc9879077af1278556`, `qemu-img compare` identical to the source overlay, `qemu-img check` clean, mode 0444. |
| Copies | `f44` and `d69` (same directory name; the copy was streamed through the workstation on 2026-10-23, its SHA-256 and all sidecar hashes match, `qemu-img check` clean; receipt `work/verification/golden-transfer-1d422e725efb`). No complete S3 copy of this older disk. Patrick approved the independently rebuilt `aurora-testing-45.20261022.1-ec2-20261029-a1` from the same pinned Aurora OS for cloud restore; it has verified Fedora, Debian and S3 copies and is in `putgd-lab-aurora`. See [cloud golden and Boat snapshot](remote-golden-images.md#aurora-fedora-45-cloud-golden-and-boat-snapshot). |

### How it was made

From the scratch tree `~/work/put.gd-f45` (nightly `5ba9de0e`, `labroot.py`
pointing the lab roots at `~/work/put.gd`), run `aurora-f45g`
(id `13172c8d-b2c8-4d91-9453-8af058f17ca4`, SSH `22427`, VNC `5927`):

1. `lab.py launch --profile aurora-kde-wayland --golden <v3>/image.qcow2
   --known-hosts <pin for 22427>` (the golden's own `known_hosts` pins `22426`
   only; the pinned key is the same, so a copy with the port rewritten works).
2. `sudo -n systemctl stop uupd.service; sudo -n systemctl disable --now
   uupd.timer; sudo -n systemctl mask uupd.timer`. The updater had already been
   pulling the Fedora 44 `44.20261022.1` stable image for eight minutes.
3. `sudo -n bootc switch ghcr.io/ublue-os/aurora:testing` (256 layers,
   3.7 GB, 12 minutes; `Queued for next boot: … testing-45.20261022.1`), then
   `sudo -n systemctl reboot`; SSH answered 52 s later.
4. Verification (`verify.log`): `/etc/os-release`, `bootc status`, the
   `loginctl` session, the plasmalogin drop-in, the idle settings, the masked
   timer, the sudo rule, package versions, the host key fingerprint, and a
   `screenshot` of the autologged desktop (`witness-head0.ppm` in the run).
5. `sudo -n rpm-ostree cleanup -r` (drop the Fedora 44 rollback deployment),
   authorized_keys reduced to the bootstrap key, `~/putgd-lab` and any
   `putgd-appimage-*.desktop` removed, `sync`, `sudo -n systemctl poweroff`.
6. `qemu-img check`, `qemu-img convert -O qcow2` to the new directory,
   `qemu-img compare` (identical), `qemu-img check`, SHA-256, mode 0444,
   `known_hosts`, `identity.pub`, `bootstrap.sh`, `provisioning-verify.log`,
   `source.manifest.json` (the v3 manifest updated with the Fedora 45 facts
   and the rebase record).

The image is larger than v3 (24.0 GB against 15.4 GB) because the blocks the
pruned Fedora 44 deployment and the updater's partial pull occupied were not
discarded before the flatten; a future rebuild can `fstrim -av` in the guest
on a `discard=unmap` drive before the poweroff.

### Smoke round trip on the new golden

Run `aurora-f45s` from the same scratch tree, nightly AppImage
`putgd-26.1022.1019-nightly-x86_64.AppImage` (release
`v26.1022.1019-nightly`, SHA-256
`e5b7ad2caecaa7f5737bdfb5d2f0fd4e3c74ce5d98a8a68142caf4efd43525bc`, matches
`SHA256SUMS`), receipts under `~/work/put.gd/work/aurora-lab/prov-f45/smoke.log`
and the run directory:

| Item | Value |
| --- | --- |
| Run | `work/lab-runs/aurora-f45s`, id `4e7d4816-5dc1-40bc-b27a-400eeceb9658`, SSH `22427`, VNC `5927`, KVM, QEMU 10.2.2, golden SHA-256 `33fedf97…8556` recorded in `state.json` |
| Firmware | `/usr/share/edk2/ovmf/OVMF_CODE_4M.qcow2` read-only; private `vars.qcow2` copied from `OVMF_VARS_4M.qcow2` |
| `launch` | booted, SSH answered 3 min 18 s after the command (the hash of the 24 GB golden is included) |
| `install --appimage` | `installed`, guest SHA-256 `e5b7ad2c…25bc` = host file; `--version-json` `26.1022.1019-nightly` at commit `5ba9de0e`; bundled `usr/bin/putgd` `3920fcdf…5059` |
| `start` | resident pid 2246 running the bundled `putgd` (`3920fcdf…` = receipt), `source: kde-wayland` |
| `cli --method capabilities` | `source.adapter` `kde-wayland`; desktop, region and window capture `compositor-native` through `org.kde.KWin.ScreenShot2` v5, permission `restricted-desktop-entry`, entry matches the executable, not rejected; recording `recording_not_probed` (expected before the first prepare) |
| `screenshot` | `witness-head0.ppm`, SHA-256 `80769090c3bd1b5806a60445e76789c96bcdfb65f2f4cb6a251b7ff3cc70178f`, 1280x800: the dashboard open on Settings, version `26.1022.1019-nightly` ([picture](evidence/2026-10-23-aurora-f45-golden/02-smoke-dashboard-26.1022.1019.png)) |
| `collect` | kernel `7.2.11-300.fc45`, `plasma-workspace-6.7.5-2.fc45`, `qt6-qtbase-6.11.2-3.fc45`, `xdg-desktop-portal-1.22.1-2.fc45`, no `putgd` package (expected) |
| Retirement | `reset`, then the guarded `cleanup --evidence-manifest` with an explicit completion manifest removed only `disk.qcow2`; receipts under `prov-f45/evidence-aurora-f45s/` |

The autologged Fedora 45 desktop before any install is the provisioning run's
witness ([picture](evidence/2026-10-23-aurora-f45-golden/01-f45-desktop-autologin.png)).

## Running a disposable overlay

`tools/lab/lab.py` runs this guest through the `aurora-kde-wayland` profile
(`tools/lab/profiles/aurora-kde-wayland.yaml`: KDE, Wayland, `plasmashell`,
guest user `lab`, `virtio-vga`, 6144 MiB, 2 vCPUs, the `kde-wayland` fixture
hotkeys). Two profile keys differ from the package-based profiles:

- `"firmware": "uefi"`: `launch` adds the host's read-only `OVMF_CODE_4M` image
  and a private per-run copy of `OVMF_VARS_4M` as the two pflash drives. It
  looks for the pair under `/usr/share/edk2/ovmf/*.qcow2` (Fedora `edk2-ovmf`)
  and then `/usr/share/OVMF/*_4M.fd` (Debian/Ubuntu `ovmf`), refuses to launch
  when neither host pair exists, and records the code path, the template path,
  both SHA-256 values and the run's `vars.qcow2` (or `vars.fd`) under
  `firmware` in `state.json`. Profiles without the key keep their BIOS command.
- `"install": "appimage"`: `install --appimage FILE` replaces the apt path. It
  copies the file to the guest home (`/var/home/lab`), `chmod +x`, compares the
  guest's SHA-256 with the host file, extracts the bundled `usr/bin/putgd*`
  binaries (`--appimage-extract` into `~/putgd-lab/appimage`) and hashes them,
  runs `--version-json`, and writes `package.json` with `format: appimage`,
  `installedPath`, `version` and the `installedBinarySha256` map keyed like a
  package (`/usr/bin/putgd`). `verify_running_app` compares the resident's
  mounted executable against that map, as for a package. The install also
  writes `~/putgd-lab/bin/putgd-launcher` (runs the AppImage, so AppRun
  registers the KWin permission entry) and `~/putgd-lab/bin/putgd-cli` (the
  extracted CLI with the image's libraries); `guest_session` prepends that
  directory to `PATH` only for `appimage` profiles. `--deb` on this profile and
  `--appimage` on a package profile are rejected.

Run the commands on `f44` from a checkout that contains this runner. The lab
roots (`work/lab-runs`, `work/lab-images`, the lifecycle lock) are resolved
from the checkout, so a scratch tree needs the `overlay_cleanup.ROOT`
override described in [the remote-lab notes](vm-lab.md#retire-completed-overlays)
or the canonical checkout itself. Pick the golden with `--golden`: the Fedora
45 directory for acceptance, the v3 directory for the Fedora 44 base. The
Fedora 45 golden's `known_hosts` pins `[127.0.0.1]:22426` and `:22427`; the
Fedora 44 goldens' pin `22426` only, and the bootstrap directory's pins
`22425`, the port the hand-run `aurora-r1` guest used. The host key is the same
on all of them, so a copy with the port rewritten authenticates another port.
Check that the chosen SSH and VNC ports are free on the builder first
(`ss -ltn`); two Aurora runs on `f44` at once take `22426`/`5926` and
`22427`/`5927`.

```sh
set -eu
cd ~/work/put.gd
g=~/work/put.gd/work/lab-images/aurora-testing-45.20261022.1-852d34e885c1
r=~/work/put.gd/work/lab-runs/aurora-$(python3 -c 'import uuid; print(uuid.uuid4().hex[:8])')
test ! -e "$r"
python3 tools/diagnostics/check_socket_paths.py --socket "$r/qmp.sock"
python3 tools/lab/lab.py launch --profile aurora-kde-wayland \
  --golden "$g/image.qcow2" --run "$r" \
  --bootstrap-key ~/work/put.gd/work/lab-bootstrap-f44/aurora/identity \
  --known-hosts "$g/known_hosts" --ssh-port 22426 --vnc-display 26 --timeout 420
python3 tools/lab/lab.py install --run "$r" \
  --appimage ~/work/put.gd/work/aurora-lab/appimage-9042776/putgd-0.1.1-dev.419+g9042776eb70e-x86_64.AppImage
python3 tools/lab/lab.py start --run "$r"
python3 tools/lab/lab.py cli --run "$r" --method capabilities
python3 tools/lab/lab.py screenshot --run "$r"
```

`start` runs `putgd-launcher --background` in the autologged Plasma session,
waits for `putgd-cli capabilities` and checks the running `putgd` against the
install receipt. `cli`, `key`, `input` and `screenshot` then work as on any
profile. The `run` scenarios still need `putgd-desktop-fixture` in the guest;
the AppImage does not ship it and Aurora has no package to install it from, so
only smoke and CLI checks run on this guest until a fixture build for Fedora 44
userland is provided separately. The guest is busy for about a minute after
SSH answers while Plasma finishes its autologin; `start` waits for the CLI.

Expose the guest on the wall with a workstation tunnel to remote `5926`
following [remote VM displays](remote-vm-displays.md), with its own unit and
tile per run. `tools/lab/rfb.py` through that port provides screenshots, clicks
and typing. Typing through QEMU VNC drops Shift on punctuation (`&&` arrives
as `77`, `|` as `\`): use `;` and `tee` plus Ctrl+D.

Retire the overlay with the guarded cleanup after exporting evidence. A
smoke-only run has no passed scenario journal, so it needs the explicit
completion manifest described in [vm-lab.md](vm-lab.md#retire-completed-overlays):

```sh
python3 tools/lab/lab.py reset --run "$r"
python3 tools/lab/lab.py cleanup --run "$r" --evidence-manifest "$manifest" --dry-run
python3 tools/lab/lab.py cleanup --run "$r" --evidence-manifest "$manifest"
```

Cleanup removes only `disk.qcow2`; `vars.qcow2` (a qcow2 image without a
backing file, which the dependency inventory accepts), keys, logs and witnesses
stay with the run. The hand-run `aurora-r1` overlay has no `state.json` and
stays outside the guarded cleanup.

### Runner round trip, 2026-10-19

The first runner-driven round trip ran on `f44` from this branch (scratch tree
`~/work/put.gd-aurora` with the lab roots pointed at `~/work/put.gd`), while the
hand-run `aurora-r1` guest kept its ports. Private receipts, the QMP witness
and wall screenshots are in the workstation's
`work/aurora-lab/2026-10-19-aurora-runner-rn4f2a9c/` and on the builder under
`~/work/put.gd/work/aurora-lab/runner-rn4f2a9c/`.

| Item | Value |
| --- | --- |
| Run | `work/lab-runs/aurora-rn4f2a9c`, id `14d2ccc6-6f78-4b92-a0b1-71c5cca9fe89`, SSH `22426`, VNC `5926`, KVM, QEMU 10.2.2 |
| Golden | v3, SHA-256 `6a2fb407830f95228bb06f4d20d7cd0bd1aec8ed937ef59430e50eb608b62c2d` |
| Firmware | `/usr/share/edk2/ovmf/OVMF_CODE_4M.qcow2` (`cc2b32e9…`) read-only; private `vars.qcow2` copied from `OVMF_VARS_4M.qcow2` (`035317bb…`) |
| `install --appimage` | `putgd-0.1.1-dev.419+g9042776eb70e-x86_64.AppImage` from nightly CI run 39201599215 (`release-appimage`), SHA-256 `1be3fbdcd8502d7c3fe58c8faaeed95e7aeab4def6c1df6e60dbf07d1c8ebec7`, `--version-json` `0.1.1-dev.419+g9042776eb70e` at commit `9042776`; bundled `usr/bin/putgd` `9c300c04…` |
| `start` | resident pid 2270 running the bundled `putgd` (`9c300c04…` = receipt), `source: kde-wayland` |
| `cli --method capabilities` | `source.adapter` `kde-wayland`; desktop, region and window capture `compositor-native` through `org.kde.KWin.ScreenShot2` v5, permission entry matches the executable |
| `screenshot` | `witness-head0.ppm`, SHA-256 `fb9ed9d2a9efe14c8b11256aa54fa7c521fb69d71723d887a36b85b702649305`, 1280x800 |
| `collect` | kernel `7.1.8-200.fc44`, `plasma-workspace-6.7.4`, `qt6-qtbase-6.11.2`, no `putgd` package (expected) |
| Wall | workstation `5982` to remote `5926` (`putgd-vm-aurora-rn4f2a9c.service`), tile "Aurora 44 KDE VM aurora-rn4f2a9c (f44)", VNC connected |
| Retirement | `reset` stopped the owned QEMU; guarded `cleanup --evidence-manifest` (explicit completion manifest, 18 hashed files) completed at 06:12:58Z and removed only `disk.qcow2`; `vars.qcow2`, keys, witness and receipts remain; tunnel stopped and tile removed |

The first `start` on a fresh profile opens the dashboard on the Settings page
(no capture output is configured); the tray icon is present. The host
contracts (`python3 tools/ci/contracts.py lab`, all `tests/desktop` suites)
passed on `f44` from the same tree.

## AppImage results, 2026-10-19

Both AppImages were copied to `/var/home/lab` in an overlay of the v1 golden
(run `aurora-r1`, 3 vCPUs, 8 GiB), started with
`systemd-run --user … /var/home/lab/putgd.AppImage` inside the autologged
Plasma Wayland session, and driven through the bundled `putgd-cli` with
`LD_LIBRARY_PATH` and `QT_PLUGIN_PATH` pointing at the FUSE mount. The guest
`lab` user had enabled "Keep a local copy" in the dashboard, so captures were
local-only (no destination, no credentials).

| Check | nightly `e13fee9` (`0.1.1-dev.366`, before PR 81) | nightly `9042776` (`0.1.1-dev.419`, with PR 81) |
| --- | --- | --- |
| `--version-json` headless | pass | pass (not re-run) |
| FUSE mount and resident start | pass: `AppRun`, `putgd`, `putgd-ui`, `session.sock` | pass |
| Qt platform | `putgd` Wayland; `putgd-ui` on XWayland | both native Wayland (KWin decorations on the dashboard) |
| `capabilities` source | `kde-wayland`, KWin `ScreenShot2` v5, permission entry matches executable | same |
| Desktop capture (`action.invoke desktop`) | pass: 1280x800 PNG, 134,277 bytes, history `saved`, Plasma notification | not re-run |
| Region capture (`action.invoke region` + drag) | fail: `region_unavailable` (helper exits without a selection; the known pre-PR-81 defect) | pass: 460x320 PNG, 34,045 bytes from a 300,200 to 760,520 drag |
| Tray icon | present in the Plasma system tray | present |

The AppImage registers its `putgd-appimage-*.desktop` permission entry under
`~/.local/share/applications` and KWin accepts it on Plasma 6.7. The recording
feature reports `recording_helper_missing` in this build, which is expected for
the AppImage. Flatpak was not exercised; when the put.gd Flatpak exists, the same
overlay can install it from a local bundle with `flatpak install --user`, and
Flathub is already configured for its runtime.

One observed failure mode is not an app defect: a region capture requested
while the Plasma session was idle-locked (the default 5-minute lock in v1 and
v2) stayed in `acquiring` until `capture_timeout`. v3 disables the lock. On v1
or v2, unlock the session on the wall before capture tests.

## Follow-ups

- Provide `putgd-desktop-fixture` for this guest (a Fedora 44 userland build
  or a bundled copy) so the `run` scenarios, not only smoke/CLI checks, can
  execute on Aurora; add a Flatpak install path beside `--appimage`.
- Decide with Patrick whether the Fedora 44 goldens stay `f44`-only
  rebuildable evaluation images. Fedora 45 cloud restore now uses the separately
  verified EC2 golden; the older disk's byte-copy upload was cancelled
  ([#484](https://github.com/patrick-hudson/put.gd-desktop/issues/484)).
- Make `bootstrap-aurora-guest.sh` mask `uupd.timer`, and `fstrim` before the
  next flatten; rebuild the Aurora golden from the signed `stable` tag once it
  moves to Fedora 45, following [How it was made](#how-it-was-made)
  ([#483](https://github.com/patrick-hudson/put.gd-desktop/issues/483)).
- Exercise the Flatpak on this guest once it is implemented.
