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

`tools/lab/lab.py` cannot launch this guest yet: it has no UEFI/pflash option
and no `aurora` profile, and Aurora's `install` path (apt) does not apply.
Until a profile exists, run QEMU by hand on `f44` from a fresh run directory:

```sh
set -eu
g=~/work/put.gd/work/lab-images/aurora-stable-44.20260908.1-v3-492c3f8945d1
r=~/work/put.gd/work/lab-runs/aurora-$(python3 -c 'import uuid; print(uuid.uuid4().hex[:8])')
test ! -e "$r"; mkdir -m 700 -p "$r"; cd "$r"
qemu-img create -q -f qcow2 -b "$g/image.qcow2" -F qcow2 disk.qcow2
cp /usr/share/edk2/ovmf/OVMF_VARS_4M.qcow2 vars.qcow2
cp "$g/known_hosts" known_hosts
qemu-system-x86_64 -machine q35,accel=kvm -cpu host -smp 3 -m 8192 \
  -drive if=pflash,format=qcow2,readonly=on,file=/usr/share/edk2/ovmf/OVMF_CODE_4M.qcow2 \
  -drive if=pflash,format=qcow2,file="$r/vars.qcow2" \
  -device virtio-vga,id=video0 -device usb-ehci -device usb-tablet \
  -drive file="$r/disk.qcow2",format=qcow2,if=virtio \
  -netdev user,id=net0,hostfwd=tcp:127.0.0.1:22425-:22 -device virtio-net-pci,netdev=net0 \
  -qmp "unix:$r/qmp.sock,server=on,wait=off" -vnc 127.0.0.1:25 -display none \
  -daemonize -pidfile "$r/qemu.pid"
```

A fresh `OVMF_VARS_4M.qcow2` boots the installed disk through the removable
`EFI/BOOT` path; the provisioning copy `aurora-lab/prov-a1/vars.qcow2` also
works. Check that `5925` and `22425` are free on the builder first, and pick
other numbers (and matching `known_hosts` endpoints) for a second guest. SSH as
`lab@127.0.0.1 -p 22425` with the bootstrap key and the golden's `known_hosts`.
Expose the guest on the wall with a workstation tunnel to remote `5925`
following [remote VM displays](remote-vm-displays.md); the 2026-10-19 run used
workstation port `5981` (`putgd-vm-aurora-a1.service`) and the tile
"Aurora stable VM prov-a1 (f44)". `tools/lab/rfb.py` through that port
provides screenshots, clicks and typing. Typing through QEMU VNC drops Shift on
punctuation (`&&` arrives as `77`, `|` as `\`): use `;` and `tee` plus Ctrl+D.

Retire an overlay only after exporting evidence; the guarded `lab.py cleanup`
does not know these runs, so remove `disk.qcow2` by hand after confirming the
QEMU process is gone and nothing else references it.

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

- Add a `tools/lab/lab.py` UEFI option and an `aurora-kde-wayland` profile so
  `launch`, `screenshot`, `key`, `input` and guarded `cleanup` work; `install`
  needs an AppImage/Flatpak path instead of apt.
- Copy the v3 golden to Debian and S3 to satisfy the two-copies rule, or decide
  with Patrick that it is a rebuildable evaluation image.
- Exercise the Flatpak on this guest once it is implemented.
