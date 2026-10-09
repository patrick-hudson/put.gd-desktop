# Disposable graphical lab

The host interface is `python3 tools/lab/lab.py`. It uses QEMU, QMP, SSH and ordinary
Mint packages. It never shares the host clipboard. All control sockets are private;
VNC and SSH forwarding bind to 127.0.0.1. A QMP witness is independent of guest capture.
The user watches active machines at http://127.0.0.1:8765/. Keep each guest’s VNC
listener and graphical output active during interaction; use `run --keep` while
continuing a test session so subsequent actions remain visible on the display wall.
A locked or blanked session must be visibly restored before graphical acceptance.
Host protocol tests and offscreen fixture checks do not prove desktop acceptance.

Linux source-build guests also require the standard cursor development package: `libxcursor-dev` on Ubuntu/Mint or `libXcursor-devel` on Fedora. Install it in disposable development guests using their normal package manager. The desktop profile files describe runtime sessions; they do not contain development package lists. Provision a new golden disk for persistent package updates, preserving sealed bases and active overlays.

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

For the first installation (root owns executing this in the current work session):

```sh
mkdir -p work/lab-images work/lab-bootstrap/install
chmod 700 work/lab-bootstrap/install
qemu-img create -f qcow2 work/lab-images/cinnamon-x11.qcow2 32G
qemu-system-x86_64 -enable-kvm -machine q35 -cpu host -smp 2 -m 6144 \
  -device virtio-vga -device usb-ehci -device usb-tablet \
  -drive file=work/lab-images/cinnamon-x11.qcow2,format=qcow2,if=virtio \
  -cdrom work/lab-bootstrap/linuxmint-22.3-cinnamon-64bit.iso -boot order=d \
  -qmp unix:work/lab-bootstrap/install/qmp.sock,server=on,wait=off \
  -netdev user,id=net0,hostfwd=tcp:127.0.0.1:22220-:22 \
  -device virtio-net-pci,netdev=net0 -vnc 127.0.0.1:20 -display none \
  -daemonize -pidfile work/lab-bootstrap/install/qemu.pid
```

Connect a VNC viewer to `127.0.0.1:5920`. Install Mint normally onto the disposable
32 GiB disk, create user `lab`, and enable automatic login to Cinnamon. Use a
throwaway guest password entered in the installer, never in a command or repository.
Before booting the installed disk, generate the lab key on the host and prepare a
small FAT provisioning disk using normal `dosfstools`/`mtools` utilities:

```sh
ssh-keygen -q -t ed25519 -N '' -f work/lab-bootstrap/identity
truncate -s 16M work/lab-bootstrap/seed.img
mkfs.vfat -n PUTGDLAB work/lab-bootstrap/seed.img
mcopy -i work/lab-bootstrap/seed.img tools/lab/bootstrap-guest.sh ::bootstrap.sh
mcopy -i work/lab-bootstrap/seed.img work/lab-bootstrap/identity.pub ::identity.pub
```

Reboot with the ISO detached (`-boot order=c`, remove `-cdrom`) and add
`-drive file=work/lab-bootstrap/seed.img,format=raw,if=virtio` to the one-time QEMU
command. This disk contains the public key and script only. In the guest terminal:

```sh
sudo mount /dev/disk/by-label/PUTGDLAB /mnt
sudo sh /mnt/bootstrap.sh /mnt/identity.pub
```

Copy `tools/lab/bootstrap-guest.sh` and the public key into the disposable guest. The script verifies it is running inside QEMU, installs SSH with normal apt packages, creates the private authorized-key file, and grants only guest package installation to `lab`. The current .deb resolves Qt and other runtime dependencies through apt. No password or cloud credential is part of this script.

Verify the SSH host fingerprint
against the guest console before recording the host key. Keep the private key outside
evidence, configuration and golden disk. The reusable golden disk contains only the
public key and synthetic fixtures, no cloud credentials. Shut down the installed guest
cleanly before creating overlays and detach the provisioning disk. Record `sha256sum` of the final disk; the ISO checksum
does not authenticate the resulting disk.

The runner's `launch` generates a fresh SSH key per run and installs its public key
using the bootstrap key, pins the presented SSH host key to that run after bootstrap
verification, and then uses the fresh key. Pass the known-hosts file obtained during
golden-image provisioning; bootstrap does not silently trust new hosts.

## Package and run

Configure the app with `cmake --preset dev -DPUTGD_LAB_FIXTURE=ON`, build with
`cmake --build --preset dev`, then `cpack --config build/dev/CPackConfig.cmake -G DEB -B build/packages`.
The fixture is a separate development executable, never an implementation adapter.
The package contains the application, CLI, desktop entry, opt-in autostart template,
private official AWS runtime, dependency lock and licenses. Qt, its platform plugins,
QtKeychain and SQLite driver come from normal guest distro packages. Development installs use apt’s `--reinstall` and compare installed application binary
SHA256 values with the requested package, even when its version is unchanged. The installed
`putgd` binary resolves private SDK libraries with RPATH; `putgd-launcher` scopes that
runtime directory for desktop startup. U10 still owns complete release lifecycle,
checksums and final notice auditing.

The guest bootstrap grants the narrowly scoped `/usr/bin/apt-get` rule used by `install`; it never changes host privileges. Package dependency resolution uses distro repositories.

After the clean golden image shutdown:

```sh
python3 tools/lab/lab.py profiles
python3 tools/lab/lab.py launch --profile cinnamon-x11 \
  --golden work/lab-images/cinnamon-x11.qcow2 --run work/lab-runs/cinnamon-001 \
  --bootstrap-key work/lab-bootstrap/identity \
  --known-hosts work/lab-bootstrap/known_hosts
python3 tools/lab/lab.py install --run work/lab-runs/cinnamon-001 \
  --deb build/packages/putgd_0.1.0_amd64.deb
python3 tools/lab/lab.py run --run work/lab-runs/cinnamon-001 --scenario region
```

`run` stops the VM on pass, mismatch, timeout or crash and preserves the private run
directory. Use `--keep` for interactive investigation. `reset` stops an owned VM and
retains its writable disk and diagnostics; a fresh run directory creates an independent
overlay. Repeat the recipe as `cinnamon-002` to prove two independent runs. Do not reuse
a running golden disk. The guest app uses isolated `~/putgd-lab/config` and `data`
settings/history directories. Evidence does not need a source checkout in the guest.

Other implemented `--scenario` values are `window`, `desktop`, `clipboard-text`,
`clipboard-image`, and `clipboard-files`. Selector checks are `region-cancel-escape`,
`region-cancel-right-click`, and `region-cancel-zero-area`: they require cancelled/no-result
completion, no artifacts, and an unchanged independent text clipboard source.
Use fresh overlays for scenario isolation.
QMP drives the real global hotkey while the independent fixture is focused. The fixture
paints fixed RGB regions, owns real native clipboard sources (Ctrl+Alt+Shift+T/I/F), and
records actual Ctrl+V MIME results. Region dimensions/interior pixels, full frame/desktop
geometry and known interior pixels, detached text, image and ordered file bytes are
compared independently. Wrong target, crop, bytes or paste fails and preserves a witness.
Host guest clipboard bridging is never enabled.

For publication, provision a keychain credential reference in the guest separately.
Supply a non-secret settings patch using `--settings work/lab-bootstrap/destination.json`.
It defines the isolated S3 test profile and action profile IDs. Only `keychain` reference
credentials are accepted by this lab route. The runner appends a unique run prefix to
that test destination, saves local copies, retrieves public URLs without authentication,
and compares actual public bytes with retained artifact SHA256 and newline URL paste.
It never changes bucket policy, retention or versioning. Do not use an ordinary bucket
profile or existing user objects as test fixtures. Credentials and payloads are absent
from golden disks and review packets; intentional synthetic evidence contains only the
lab's synthetic captures/text/files.

`--history` with `--settings` restarts the installed resident, inspects the new history
IDs, copies the stored link and pastes it into the independent fixture. Results retain
exact `delete.prepare` IDs, effects and acknowledgement. This preparation is read-only.
Use the production CLI interface to inspect before explicitly acknowledging deletion:

```sh
python3 tools/lab/lab.py cli --run work/lab-runs/cinnamon-001 --method capabilities
python3 tools/lab/lab.py cli --run work/lab-runs/cinnamon-001 --method history.detail \
  --params '{"id":"EXACT_NEW_SYNTHETIC_HISTORY_ID"}'
```

For deletion, review `delete.prepare` from the production CLI, then call `delete.confirm`
with its identical ordered IDs and acknowledgement. Retain returned operation ID and
poll `delete.inspect` for actual outcome; preserve the local archive and history record.
GUI deletion/search/details acceptance remains a separate graphical gate.

For manual scenarios use `key --run RUN ctrl shift 1`, `input --run RUN 'JSON_EVENTS'`,
`screenshot --run RUN --head 0`, `collect --run RUN`, and `reset --run RUN`.
Input uses QMP `input-send-event` objects. Query output capabilities are stored in
`qmp-features.json`; witnesses identify device `video0` and the requested head. `collect`
exports only whitelisted package/kernel versions and production sanitized diagnostics.
Private app/QEMU logs stay private; review them before including any diagnostic bundle.

## Evidence and gates

Each named run emits `results.json` and JUnit `results.xml`, independent paste JSON,
actual saved artifacts/PPM conversion, public GET bytes when configured, expected/actual
pixel and hash comparisons, QMP witness frames, package hash, versions, real fixture
session/platform/display layout and requested/actual acceleration. `state.json` contains
the base disk hash and run ID. Run directories include ephemeral private keys and must
never be uploaded whole: export only the explicit evidence files in `common.yaml`,
`artifact-*`, `public-*`, `history-paste.json`, package/version manifests and results.

Profiles are available for Cinnamon/X11, Ubuntu GNOME/Wayland, Kubuntu
Plasma/Wayland and the additional Fedora GNOME/Wayland smoke environment.
Mint, Ubuntu, Fedora and Kubuntu base disks have been installed and checked locally;
the full Wayland action matrix remains pending.
Native Wayland profiles require the installed U7/U8 helper, an actual Wayland
session and the native Qt platform; readiness rejects Xwayland. TCG is explicit and labelled functional-only. VM evidence does not certify the physical-machine performance budgets.
Multihead/scaling/rotation/removal, cancellation, shortcut conflicts, archive failures,
copy suppression, full GUI history/config conflict flows, normal GTK/file-manager
interop, physical Cinnamon and the complete release matrix remain acceptance gates.
A host protocol pass, ISO checksum, live desktop boot or installed package alone does
not satisfy them.

## Additional distro base disks

Use the ISO identities and authenticated checksum sources in `image-lock.json`.
Ubuntu's normal installer supports a NoCloud autoinstall seed; a manually completed
normal installer is also valid. Fedora Workstation's Live ISO requires its normal
graphical Anaconda install; do not assume a Kickstart seed is supported. Kubuntu
uses its normal graphical installer. Install each once into its own 32 GiB qcow2
base and create the `lab` user. Enable automatic login in that disposable guest
so a fresh boot reaches the native desktop.

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
