# Aurora (Universal Blue) VM layer for AppImage and Flatpak testing

Aurora is Universal Blue's KDE Plasma desktop, an image-based (bootc) Fedora
Atomic variant: `ghcr.io/ublue-os/aurora:stable`. It installs nothing through a
package manager, ships Flathub as its only Flatpak remote, and represents the
user population that will run put.gd as an AppImage or a Flatpak rather than a
DEB or RPM. It is an evaluation guest, not a row in the
[release acceptance matrix](acceptance-matrix.md).

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

All three are standalone qcow2 files (virtual 48 GiB, about 15.4 GB allocated)
under `~/work/put.gd/work/lab-images/`, each with `source.manifest.json`,
`image-info.json`, the pinned guest `known_hosts` and the bootstrap inputs.
The [immutable-golden rules](vm-lab.md#base-protection-and-inventory) apply.
They exist only on `f44`; there is no Debian or S3 copy yet, so the
[two-copies rule](remote-golden-images.md) is not satisfied for them.

| Directory | SHA-256 | State |
| --- | --- | --- |
| `aurora-stable-44.20260908.1-f9c006a376e1` | `65cc3ae1cfb91dc5054ff2c4c50b82e8dddeed8ba53d6c29fd16282b1a0f1811` | First boot plus bootstrap. Its SDDM autologin drop-in is ignored (Aurora uses plasmalogin), so it boots to the greeter. |
| `aurora-stable-44.20260908.1-v2-af46889e2a54` | `b9ee08f09d8a4fbd4e57de04c8c1b6e9833400685492a1ab58a341d5e76dd7e7` | v1 plus `/etc/plasmalogin.conf.d/putgd-lab-autologin.conf`; autologin verified (`Service=plasmalogin-autologin`, Wayland user session). |
| `aurora-stable-44.20260908.1-v3-492c3f8945d1` | `6a2fb407830f95228bb06f4d20d7cd0bd1aec8ed937ef59430e50eb608b62c2d` | **Use this one.** v2 plus lab idle settings: no screen lock, dimming, display power-off or suspend. |

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
   run from Konsole as `sudo mount /dev/disk/by-label/PUTGDLAB /mnt; sudo sh /mnt/bootstrap.sh /mnt/identity.pub`.
   The generic `bootstrap-guest.sh` does not fit: Aurora has no `dnf` and its
   display manager is plasmalogin. The committed script already writes the
   plasmalogin drop-in and the idle settings that v2 and v3 added by hand.
5. Clean `systemctl poweroff` over SSH, `qemu-img check`, `qemu-img convert`
   to a new standalone image, `qemu-img compare` against the source, SHA-256,
   mode 0444, manifest.

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
or the canonical checkout itself. The golden's `known_hosts` pins
`[127.0.0.1]:22426`; the bootstrap directory's pins `[127.0.0.1]:22425`, the
port the hand-run `aurora-r1` guest used. Check that the chosen SSH and VNC
ports are free on the builder first.

```sh
set -eu
cd ~/work/put.gd
g=~/work/put.gd/work/lab-images/aurora-stable-44.20260908.1-v3-492c3f8945d1
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
- Copy the v3 golden to Debian and S3 to satisfy the two-copies rule, or decide
  with Patrick that it is a rebuildable evaluation image.
- Exercise the Flatpak on this guest once it is implemented.
