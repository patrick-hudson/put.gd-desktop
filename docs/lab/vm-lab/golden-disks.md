# VM lab: golden disks

Part of [Disposable graphical lab](../vm-lab.md).

## First Cinnamon golden disk

The OS is installed once. Subsequent tests boot independent copy-on-write overlays,
install the current development package, and drive the genuine desktop through QMP.
They do not repeat the installer or install compilers in the guest. QEMU uses its normal
`-daemonize`/pidfile lifecycle so completing a host command does not terminate a test VM.
The runner still owns timeout/crash cleanup through its private QMP socket.


Install host distro packages `qemu-system-x86 qemu-utils openssh-client` and a VNC
viewer for optional inspection. Check `/dev/kvm` is accessible. Administrator authentication, when needed,
is performed by the user. The authenticated ISO is
`work/lab-bootstrap/linuxmint-22.3-cinnamon-64bit.iso`; its signed SHA256 is in
`tools/lab/image-lock.json`. Recheck with `sha256sum` before booting.

The original Cinnamon base is already provisioned. The following is a template
for a **new, not-yet-provisioned** disk after inventory, not a command to boot or
replace that existing base. Choose a new disk and private control directory,
verify that neither path exists, and check the resolved socket length before
creating either. Confirm the chosen SSH/VNC ports are free on the actual host.
Run the host shell examples from the repository root; `set -eu` makes failed
preconditions stop the recipe.

```sh
set -eu
provision_id=$(python3 -c 'import uuid; print(uuid.uuid4().hex[:12])')
provision_disk="$PWD/work/lab-images/cinnamon-$provision_id.qcow2"
provision_control="$PWD/work/lab-bootstrap/p-$provision_id"
test ! -e "$provision_disk"
test ! -e "$provision_control"
python3 tools/diagnostics/check_socket_paths.py --socket "$provision_control/qmp.sock"
mkdir -p work/lab-images
mkdir -m 700 "$provision_control"
qemu-img create -f qcow2 "$provision_disk" 32G
qemu-system-x86_64 -enable-kvm -machine q35 -cpu host -smp 2 -m 6144 \
  -device virtio-vga -device usb-ehci -device usb-tablet \
  -drive "file=$provision_disk,format=qcow2,if=virtio" \
  -cdrom work/lab-bootstrap/linuxmint-22.3-cinnamon-64bit.iso -boot order=d \
  -qmp "unix:$provision_control/qmp.sock,server=on,wait=off" \
  -netdev user,id=net0,hostfwd=tcp:127.0.0.1:22220-:22 \
  -device virtio-net-pci,netdev=net0 -vnc 127.0.0.1:20 -display none \
  -daemonize -pidfile "$provision_control/qemu.pid"
```

Connect a VNC viewer to `127.0.0.1:5920`. Install Mint normally onto the disposable
32 GiB disk, create user `lab`, and enable automatic login to Cinnamon. Use a
throwaway guest password entered in the installer, never in a command or repository.
While this new disk is still being provisioned, prepare its SSH identity and a
small FAT seed using normal `dosfstools`/`mtools` utilities. Reuse an existing
verified bootstrap identity when appropriate; never overwrite one used by existing
runs. The commands below create fresh inputs only in the new control directory:

```sh
set -eu
ssh-keygen -q -t ed25519 -N '' -f "$provision_control/identity"
truncate -s 16M "$provision_control/seed.img"
mkfs.vfat -n PUTGDLAB "$provision_control/seed.img"
mcopy -i "$provision_control/seed.img" tools/lab/bootstrap-guest.sh ::bootstrap.sh
mcopy -i "$provision_control/seed.img" tools/lab/guest_idle.py ::guest_idle.py
mcopy -i "$provision_control/seed.img" tools/lab/package_install.py ::package_install.py
mcopy -i "$provision_control/seed.img" tools/lab/package_scope.py ::package_scope.py
mcopy -i "$provision_control/seed.img" "$provision_control/identity.pub" ::identity.pub
```

Shut down this new provisioning guest before restarting it. With the ISO
detached (`-boot order=c`, remove `-cdrom`), add
`-drive "file=$provision_control/seed.img,format=raw,if=virtio"` to the one-time
QEMU command for this same unprovisioned disk. The seed contains the public key, bootstrap script, visibility helper, package supervisor and scope launcher only. In the guest terminal:

```sh
sudo mount /dev/disk/by-label/PUTGDLAB /mnt
sudo sh /mnt/bootstrap.sh /mnt/identity.pub
```

Copy `tools/lab/bootstrap-guest.sh`, its companions `tools/lab/guest_idle.py`, `tools/lab/package_install.py` and `tools/lab/package_scope.py`, and the public key into the disposable guest, keeping all four files together. The script verifies it is running inside QEMU, installs SSH with normal apt packages, creates the private authorized-key file, and grants only guest package installation and the fixed scope launcher to `lab`. The current .deb resolves Qt and other runtime dependencies through apt. No password or cloud credential is part of this script.

Verify the SSH host fingerprint
against the guest console before recording the host key. Keep the private key outside
evidence, configuration and golden disk. The reusable golden disk contains only the
public key and synthetic fixtures, no cloud credentials. Shut down the installed guest
cleanly before creating overlays and detach the provisioning disk. Record `sha256sum` of the final disk; the ISO checksum
does not authenticate the resulting disk.

The runner's `launch` generates a fresh SSH key per run and installs its public key
using the bootstrap key, pins the presented SSH host key to that run after bootstrap
verification, and then uses the fresh key. Pass the known-hosts file obtained during
golden-image provisioning; bootstrap does not silently trust new hosts. The file
must contain the exact forwarded endpoint, such as `[127.0.0.1]:22220`, selected
by `--ssh-port` (port 22 uses `127.0.0.1` without brackets). The runner
checks for a matching entry before allocating a run. This catches a missing
endpoint; it cannot authenticate an unbooted guest or prove a matching key is
correct. SSH still verifies the presented key after boot.

For a different port, explicitly select the **same golden guest's previously
verified endpoint** and copy its pinned key to a new private file. Do not use an
unauthenticated `ssh-keyscan` result, or select another desktop's key because it
shares the same loopback address. For example, if this guest was authenticated
at port 22220 and its new run will use 22431:

```sh
umask 077
lab_hostkeys=$(mktemp -d /tmp/pgkeys-XXXXXXXX)
ssh-keygen -F '[127.0.0.1]:22220' -f work/lab-bootstrap/known_hosts > "$lab_hostkeys/pinned"
# Inspect the selected public-key fingerprint against the provisioning receipt.
ssh-keygen -lf "$lab_hostkeys/pinned"
awk '!/^#/ && NF >= 3 {if ($1 ~ /^@/) $2="[127.0.0.1]:22431"; else $1="[127.0.0.1]:22431"; print}' \
  "$lab_hostkeys/pinned" > "$lab_hostkeys/known_hosts"
```

Use `--ssh-port 22431 --known-hosts "$lab_hostkeys/known_hosts"` for launch.
Strict host-key verification still checks the guest's presented key. A missing
or changed key requires resolving the provisioning identity; disabling checking
is not a recovery step. Keep private run files under a unique directory when
running concurrent campaigns.

## Additional distro base disks

Use the ISO identities and authenticated checksum sources in `image-lock.json`.
Ubuntu's normal installer supports a NoCloud autoinstall seed; a manually completed
normal installer is also valid. Fedora Workstation's Live ISO requires its normal
graphical Anaconda install; do not assume a Kickstart seed is supported. Kubuntu
uses its normal graphical installer. Inventory existing bases/runs first, then
install each new baseline into a fresh, absent 32 GiB qcow2 path and create the
`lab` user. Existing provisioned paths are never installer targets. Enable
automatic login in that disposable guest
so a fresh boot reaches the native desktop.

Aurora (Universal Blue, Fedora Atomic KDE) is an image-based evaluation guest
for AppImage testing. It needs UEFI firmware, has no package
manager and uses plasmalogin instead of SDDM. The `aurora-kde-wayland` profile
sets `"firmware": "uefi"` (the runner adds the host's read-only
`OVMF_CODE_4M` image and a private per-run copy of `OVMF_VARS_4M`, found under
`/usr/share/edk2/ovmf` on Fedora or `/usr/share/OVMF` on Debian, and records
both in `state.json`) and `"install": "appimage"` (`install --appimage` instead
of `--deb`). Its goldens on `f44`, their provisioning and the runner commands
are in [Aurora VM](../aurora-vm.md). BIOS profiles have no `firmware` key and
their QEMU command is unchanged.

KDE neon (User for released Plasma, Unstable for Plasma git master) uses the
`neon-kde-wayland` profile (BIOS, `install: appimage`) and its own
`bootstrap-neon-guest.sh`, because neon uses plasmalogin and wraps `apt-get` in
`/usr/sbin`. Its goldens and provisioning are in [KDE neon VMs](../neon-vm.md).

Omarchy (Arch Linux and Hyprland) uses the `omarchy-hyprland` profile (UEFI,
`install: appimage`, session process `quickshell`). Its golden is installed by the
Omarchy ISO's own unattended installer on EC2, not by `bootstrap-guest.sh`, and
unlocks its LUKS root with a keyfile in the initramfs so overlays boot without a
prompt. The facts and recipe are in [Omarchy VM golden](../omarchy-vm.md).

`bootstrap-guest.sh` supports apt-based guests and Fedora's dnf. Run it inside
the guest with only the lab public key. It restores SSH file labels on SELinux
guests and enables their SSH firewall service when Firewalld is running. Its
package-manager sudo rule applies only to the guest. Verify the guest host-key
fingerprint against its QEMU console before pinning it.

After a fresh installed-disk boot, record OS/compositor versions, native session
type and successful automatic login. Power down cleanly, run `qemu-img check`,
and record the base SHA256. Install applications and development dependencies
only in disposable overlays. No AWS credentials belong in a base image.

Build the application package against each distro's own Qt/runtime libraries.
A Mint .deb is not a substitute for an Ubuntu 26.04 package: package names and
ABIs can differ. Use the documented SDK/CMake/CTest/CPack build in a snapshot;
never bypass package-manager dependency checks.

On Wayland, Qt's reported window origin is not a global screen coordinate. The
runner locates the complete synthetic four-color pattern in an independent
pre-action QMP witness; it rejects missing, obscured or ambiguous patterns.
Capture results are compared afterward and never determine the input coordinates.
