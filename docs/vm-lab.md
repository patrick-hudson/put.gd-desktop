# Disposable graphical lab

The host interface is `python3 tools/lab/lab.py`. It uses QEMU, QMP, SSH and ordinary
distro packages. It never shares the host clipboard. All control sockets are private;
VNC and SSH forwarding bind to 127.0.0.1. A QMP witness is independent of guest capture.
The user watches active machines at http://127.0.0.1:8765/. Keep each guest’s VNC
listener and graphical output active during interaction; use `run --keep` while
continuing a test session so subsequent actions remain visible on the display wall.
A locked or blanked session must be visibly restored before graphical acceptance.
Host protocol tests and offscreen fixture checks do not prove desktop acceptance.

## Host selection

Plan host/guest placement with the [Linux acceptance matrix](acceptance-matrix.md).
Cover Mint, Ubuntu, Kubuntu, Fedora and Debian plus GNOME/KDE X11/Wayland;
provision missing VM profiles while using physical machines for physical release gates.

Use the [preferred Fedora/Debian/Kubuntu build servers](remote-builders.md) before local
workstation VMs. Select a host with the required capacity, KVM access and visible
guest display; record a local fallback reason if none can run the profile.
The runner executes on the actual QEMU host, via pinned SSH for remote hosts;
it does not currently provide a remote-host switch. Run its storage/process/port
inventory there, check resolved socket lengths there, and export receipts and
visual evidence to the workstation. Do not treat workstation capacity or process
absence as evidence about a remote host.

The existing Fedora GNOME and Debian Plasma display-wall tiles show physical
desktops. Add a distinct loopback-only SSH tunnel and tile for each remote guest
before graphical acceptance. Save screenshots or recordings of actions, results
and failures as well as the existing independent QMP/paste/artifact witnesses.
Host selection leaves the required guest OS/desktop versions
unchanged. Follow the remote-host runbook when copying goldens; never
boot or alter the source golden or reuse another run's writable overlay.

Linux source-build guests also require the standard cursor development package: `libxcursor-dev` on Ubuntu/Mint or `libXcursor-devel` on Fedora. Install it in disposable development guests using their normal package manager. The desktop profile files describe runtime sessions; they do not contain development package lists. Provision a new golden disk for persistent package updates, preserving sealed bases and active overlays.

## Base protection and inventory

Provisioned Linux golden disks under `work/lab-images/` are immutable inputs.
Never boot or modify them directly, or move, overwrite or delete a disk referenced
by overlays. Since 2026-10-17 the goldens live on the Fedora and Debian builders
and in S3, not on the workstation; see
[golden storage and restore](remote-golden-images.md). Tests use independent
disks under `work/lab-runs/` created through `tools/lab/lab.py`; provision a new
golden path for updates. Preserve each run's
recorded backing path and base identity.

Before provisioning or resuming, inventory existing run/base metadata and the
actual host-visible QEMU processes and reserved VNC/SSH ports. A sandbox's missing
PID does not prove that the host VM stopped. Verify the exact process identity
and owned overlay before stopping a VM; preserve its diagnostics. `reset` retains
the overlay. `launch` requires a new run directory and creates a new overlay;
it does not resume an existing run. An existing-overlay resume must separately
validate its recorded backing chain, host process/port absence and new QEMU identity,
then update the live run metadata without overwriting the prior receipt.

The current Linux runner records the golden SHA256 but does not enforce
Windows-style sealing or verify a sealed manifest before launch. Existing golden
files may still be writable. These workflow restrictions remain mandatory.

The provisioned GNOME/KDE goldens are the standalone images
`gnome-provisioned-20261016` and `kde-provisioned-20261016`. Their host paths
and hashes are in the [golden inventory](remote-golden-images.md#current-inventory).
Use these for new provisioned runs. A workstation fallback first restores the
one it needs to `work/lab-images/<name>.qcow2`. Their private sidecar manifests
record the source chain, image SHA256, integrity check and successful guest-visible comparison. The migration receipts
live under `work/housekeeping/standalone-goldens-20261016/`. Historical run
metadata retains the old paths and hashes; do not rewrite it to name the new
goldens. Never resume a retired run directory.

To flatten another chain, create a **new** golden path with `qemu-img convert`
and no backing-file option. Before retiring any old image, require successful
`qemu-img check` and `qemu-img compare OLD NEW`, equal virtual sizes, no backing
file in the new image, and a saved SHA256/manifest. Inventory actual host
processes, open files and every surviving lab backing chain. Remove dependent
leaf images before their unreferenced parents; preserve other goldens and all
host evidence. Do not use an unsafe rebase, modify an existing golden in place,
or delete a parent merely because its directory is named `lab-runs`.

## Protected login inputs

New remote lab guests use the approved `lab` / `putgd-lab` default; follow
[golden discovery, transfer and login provisioning](remote-golden-images.md).
Historical source goldens retain their existing logins until an independent
candidate is provisioned, visibly verified and saved at a new golden path.
The inventory below locates legacy inputs; never edit an immutable source in place.

Agents restore existing locked sessions. First inspect the run's profile and
bootstrap metadata, then locate the protected local input for that installation.
The three existing lab installations use these local files:

| Native profile | Existing protected input |
| --- | --- |
| Cinnamon/X11 | `work/lab-bootstrap/install/password` |
| Ubuntu GNOME/Wayland | `work/lab-bootstrap/ubuntu-install/password` |
| Kubuntu KDE/Wayland | `work/lab-bootstrap/kubuntu-install/password` |

These paths identify local inputs; they contain no login value in this document.
Verify with `lstat` that the selected file is regular, not a symlink, owned by the
current user and mode `0600`. Read its value only in memory after verifying the
visible console's focused, masked password field. Send keystrokes through that
VM's QMP/VNC console, keeping its VNC listener and display wall active. Never print,
hash, screenshot or put the value in arguments, configuration, logs or review
material. Never use `credentials.txt` for desktop login. Visually verify the
unlocked native desktop before accepting a graphical result. Ask the user only
if the protected input is unavailable or the console rejects it.

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
mcopy -i "$provision_control/seed.img" "$provision_control/identity.pub" ::identity.pub
```

Shut down this new provisioning guest before restarting it. With the ISO
detached (`-boot order=c`, remove `-cdrom`), add
`-drive "file=$provision_control/seed.img,format=raw,if=virtio"` to the one-time
QEMU command for this same unprovisioned disk. The seed contains the public key and
script only. In the guest terminal:

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

## Package and run

Configure the app with `cmake --preset dev -DPUTGD_LAB_FIXTURE=ON` and build with
`cmake --build --preset dev`. Package into a new output directory so earlier
versions cannot be mistaken for the current build:

```sh
set -eu
package_output=$(mktemp -d "$PWD/build/packages-lab-XXXXXXXX")
cpack --config build/dev/CPackConfig.cmake -G DEB -B "$package_output"
set -- "$package_output"/*.deb
test "$#" -eq 1
test -f "$1"
lab_deb=$1
sha256sum "$lab_deb"
```

Record this produced package path/hash and its build manifest; use `"$lab_deb"`
for installation. Preserve CPack's result and do not select a package after a
failed command. Build identity and installed/running binary hashes must match the
candidate under test, regardless of the filename or version label.

The fixture is a separate development executable, never an implementation adapter.
The package contains the application, CLI, desktop entry, opt-in autostart template,
private official AWS runtime, dependency lock and licenses. Qt, its platform plugins,
QtKeychain and SQLite driver come from normal guest distro packages. Development installs use apt’s `--reinstall` and compare installed application binary
SHA256 values with the requested package, even when its version is unchanged. The installed
`putgd` binary resolves private SDK libraries with RPATH; `putgd-launcher` scopes that
runtime directory for desktop startup. U10 still owns complete release lifecycle,
checksums and final notice auditing.

The guest bootstrap grants the narrowly scoped `/usr/bin/apt-get` rule used by `install`; it never changes host privileges. Package dependency resolution uses distro repositories.

After inventory and clean golden image shutdown, choose a new run path and
verify that it does not exist. On a builder, replace the `--golden` path with
that host's inventory path. On the workstation,
[restore `cinnamon-x11`](remote-golden-images.md#restore-to-the-workstation)
first. The example consumes the existing verified base; it never boots that
base directly. Verify the profile's reserved SSH/VNC ports
are free on the actual host before launching:

```sh
set -eu
lab_run_id=$(python3 -c 'import uuid; print(uuid.uuid4().hex[:12])')
lab_run="$PWD/work/lab-runs/cinnamon-$lab_run_id"
test ! -e "$lab_run"
python3 tools/diagnostics/check_socket_paths.py --socket "$lab_run/qmp.sock"
python3 tools/lab/lab.py profiles
python3 tools/lab/lab.py launch --profile cinnamon-x11 \
  --golden work/lab-images/cinnamon-x11.qcow2 --run "$lab_run" \
  --bootstrap-key work/lab-bootstrap/identity \
  --known-hosts work/lab-bootstrap/known_hosts
python3 tools/lab/lab.py install --run "$lab_run" --deb "$lab_deb"
python3 tools/lab/lab.py run --run "$lab_run" --scenario region --keep
```

`run` stops the VM on pass, mismatch, timeout or crash and preserves the private run
directory. After a successful scenario exports its hashed evidence bundle and
finished journal event, it automatically retires the stopped, unreferenced overlay.
Failed runs and incomplete evidence collections retain their disks. A later pass
in the same guest also retains the disk if an earlier scenario failed, lacked
complete collection, or did not finish; retire it
with explicit reviewed completion evidence after investigation. Use `--keep`
for interactive investigation or a campaign that needs the guest again. `reset`
only stops an owned VM and retains its writable disk and diagnostics; a fresh run
directory creates an independent overlay. Repeat with another new run path to prove two independent runs. Do not reuse
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

## Retire completed overlays

Run lifecycle commands on the actual QEMU host. A sandbox with a filtered process
view cannot certify that a VM stopped. Linux launch/reset, Windows lifecycle
operations and overlay cleanup share a host lock. Cleanup also checks actual
process/open-file identities and QEMU backing chains, identified by their image
headers rather than filename extensions. It holds a native QEMU image lock
through final verification and unlink, preventing a competing writer from
opening the disk. Unrelated nondumpable applications may hide their memory maps;
the held image lock handles that normal host restriction, while hidden VM
processes and incomplete image inventories still refuse cleanup. It refuses running,
referenced, symlinked or mismatched disks, changed evidence and unrecorded
completion. It only removes `disk.qcow2`; screenshots, logs, parameters, results,
keys, firmware/TPM state and historical metadata remain in place. The separate
`overlay-cleanup.json` records the exact disk identity and evidence digest, with
a durable prepared/completed transition and safe retry after interruption.

For a passed, fully exported standard scenario retained with `--keep`, first stop
the VM, then preview and apply cleanup:

```sh
python3 tools/lab/lab.py reset --run "$lab_run"
python3 tools/lab/lab.py cleanup --run "$lab_run" --dry-run
python3 tools/lab/lab.py cleanup --run "$lab_run"
```

Legacy journals without explicit collection-completeness records require an
explicit completion manifest after review. An automatic cleanup refusal is recorded in `overlay-cleanup-status.json`; it
does not turn a passing scenario into a test failure or silently erase its disk.
Resolve the reported ownership/dependency/evidence problem and retry explicitly.

Custom campaigns and reviewed failures need an explicit completion manifest.
Export all required failure context, source/package identities, parameters,
results and witnesses first. Keep them private. In that evidence directory save
`completion.json` containing `run` (absolute run directory), `runId` (Linux state
`id` or Windows state `run_id`), `completed: true` and `outcome: "passed"` or
`"failed"`. A separate manifest has `schemaVersion: 1`,
`kind: "vm-overlay-cleanup-evidence"`, those same four fields, and a `files`
object mapping relative evidence paths, including `completion.json`, to their
SHA256 hashes. Members must be owned regular files within the manifest directory,
with no symlink components or parent traversal. This records that investigation
is complete; it never changes a failed acceptance result to a pass.

```sh
python3 tools/lab/lab.py cleanup --run "$lab_run" \
  --evidence-manifest "$completion_manifest" --dry-run
python3 tools/lab/lab.py cleanup --run "$lab_run" \
  --evidence-manifest "$completion_manifest"
```

Windows custom runs use `python3 tools/lab/windows.py cleanup --vm RUN
--evidence-manifest MANIFEST` after shutdown, with the same preview and evidence
rules. Do not place evidence manifests or private run files in release exports.
Retirement removes the old guest filesystem permanently; future tests use a
fresh run directory cloned from the preserved golden.


## Physical-desktop input agent

Physical build-server desktops have no QMP. `tools/lab/host_input.py` creates
kernel uinput devices, a keyboard and an absolute pointer modelled on QEMU's
usb-tablet, and accepts the runner's QMP subset (`qmp_capabilities`,
`input-send-event` with qcode keys, left/right/middle buttons and 0–32767
absolute axes) on an owner-only Unix socket. Events pass through libinput into
the compositor or X server like hardware input, so global shortcuts, region
selectors and editor windows receive real input. It refuses every other command,
including `screendump`; capture frames from the host's loopback VNC tile.

Start it with authorized `sudo -n` only to open `/dev/uinput`; it drops to the
desktop user before listening, and no udev rule or persistent permission
changes. Forward the socket over the pinned SSH host key, then point a private
run directory's `qmp.sock` at it so `lab.key`, `lab.pointer` and
`lab.input_events` work unchanged:

```sh
sudo -n systemd-run --unit=putgd-lab-input --collect \
  /usr/bin/python3 tools/lab/host_input.py serve \
  --socket /run/user/1000/putgd-lab-input.sock --user patrick
ssh -NT ... -o StreamLocalBindUnlink=yes \
  -L "$run/qmp.sock:/run/user/1000/putgd-lab-input.sock" patrick@HOST
```

Stop the unit after the campaign; its devices disappear with it. On Kubuntu
26.04 Plasma/X11 the devices appear in `xinput`, and a runner pointer move to
(960, 540) on the 1920x1080 desktop read back exactly from `XQueryPointer`.
Wayland sessions still need their own verification before use as evidence.
`tests/desktop/test_host_input.py` covers the accepted protocol and rejections.

## Editor acceptance drivers

These drivers run against a run directory that has passed `lab.py run --keep`
with the region scenario, so the resident and the clipboard fixture are live:

| Driver | Covers |
| --- | --- |
| `tools/lab/edit_acceptance.py RUN` | Graphical Edit & upload: hotkey, region-selected source, every tool, undo/redo, crop, save copy, submit, cancel, PNG sanitization and screenshots. |
| `tools/lab/edit_memory.py RUN [SAMPLER]` | Whole-family RSS+swap (`family_sampler.py` in the guest) for desktop, region and 24 MP file cases. Gate: incremental peak at most 3R + 96 MiB, and the resident returns within 8 MiB of baseline once the fixture takes the clipboard. |
| `tools/lab/mcp_editor_e2e.py EVIDENCE` | Official MCP client through `putgd-mcp` and the target's production `putgd-cli` (a small wrapper running it over pinned SSH): all editor tools, preview image, save copy, submit and cancel. |
| `tools/lab/guest_reboot.py RUN` | Reboot through the session manager when a session-loaded integration such as the GNOME extension must reload; verifies the boot time changed. |

`tools/lab/physical.py HOSTCFG.json install DEB|RPM|region|script DRIVER` runs the
same runner and drivers on a physical desktop. It swaps the runner's shell,
copy and screenshot helpers for pinned SSH, `scp` and the host's loopback VNC
tile (`tools/lab/rfb.py`, security type None only), reads input from the
forwarded agent above, and labels results `graphical-physical`. Keep the host
config and its run directory under `work/` or the scratchpad; they name only
key and known-hosts paths, never secrets.

On Fedora, `install` takes the RPM: it removes any installed `putgd` with dnf
first, because dnf will not reinstall or downgrade a same-version development
build. Verification extracts the RPM with `rpm2cpio`. A first install of the
GNOME extension loads only after a fresh login. Physical runs also need:

- Close put.gd windows left by earlier steps with `ui.quit`. On GNOME, a leftover
  Settings window covered the fixture.
- Keep the workstation wall's tile for the host view-only. With input enabled,
  moving the mouse over it injects pointer events into the run.

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
uses its normal graphical installer. Inventory existing bases/runs first, then
install each new baseline into a fresh, absent 32 GiB qcow2 path and create the
`lab` user. Existing provisioned paths are never installer targets. Enable
automatic login in that disposable guest
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
