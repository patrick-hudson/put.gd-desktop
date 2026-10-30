# Disposable graphical lab

The host interface is `python3 tools/lab/lab.py`. It uses QEMU, QMP, SSH and ordinary
distro packages. It never shares the host clipboard. All control sockets are private;
VNC and SSH forwarding bind to 127.0.0.1. A QMP witness is independent of guest capture.
The user watches active machines at http://127.0.0.1:8765/. Keep each guest’s VNC
listener and graphical output active during interaction; use `run --keep` while
continuing a test session so subsequent actions remain visible on the display wall.
A locked or blanked session must be visibly restored before graphical acceptance.
Host protocol tests and offscreen fixture checks do not prove desktop acceptance.

For opt-in parallel package preparation on disposable cloud workers, use
[exact candidate builds on Boat](boat-candidates.md). Its artifact and snapshot
receipts are separate from the graphical acceptance evidence below.

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
it does not resume an existing run. `--run` must name a symlink-free direct child
of this checkout's canonical `work/lab-runs`; an outside or nested path is rejected
before allocating an overlay or bootstrapping anything. Use a short name such as
`work/lab-runs/c1`. If that still exceeds the 107-byte resolved socket limit,
use a checkout at a shorter real path and keep runs inside its canonical root;
a symlink or `/tmp` run outside that root does not provide a cleanup-safe shortcut. An existing-overlay resume must separately
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

New guest bootstrap installs the root-owned fixed supervisor at
`/usr/local/lib/putgd-lab/package_install.py`. Every DEB `install` also stages
its controller's helper into the fresh run overlay over that run's SSH
connection, so existing immutable goldens need no refresh. The controller checks
the uploaded SHA-256 values, uses the guest's existing passwordless `/usr/bin/apt-get`
rule with an `APT::Update::Pre-Invoke` hook to install the root-owned helper,
the root-owned scope launcher `/usr/local/lib/putgd-lab/package_scope.py` and
one sudo rule naming only those two files, then checks the installed SHA-256
values, ownership, permissions and sudo access before starting the supervisor. The staging update uses `/dev/null` as its source list, disables source parts
and list cleanup, and therefore neither fetches repositories nor deletes cached
indexes. Its hook intentionally exits unsuccessfully; that expected apt exit is
checked through the installed helper's postconditions. The normal update/install
runs only under the bounded supervisor. `helper.json` records
the verified hash and staging route; `staging.log` retains the hook result.
If another apt process holds `/var/lib/apt/lists/lock`, staging fails promptly
with the holder PID and process name reported by apt (for example, apt-daily
or Mint Update may be active). Wait for that process to finish before retrying;
the controller does not stop it. Failed staging also attempts to copy
`staging.log` into the host attempt directory, with a 10-second collection
timeout. A missing log or disconnected guest does not replace the original error.

Staging runs under a detached, unprivileged guest subreaper in a new Linux
session (`setsid`), in that job's private `~/putgd-lab/install-<id>` directory.
`staging-process.json` records the session ID, session leader PID and `/proc`
start ticks before any privileged staging work. With the system-scope
prerequisites below, the launcher runs the scope launcher through passwordless
sudo. Its fixed `systemd-run --system --scope --property=Delegate=yes`
arguments create a dedicated unit under
`/system.slice/putgd-staging-install-<id>.scope`. Inside it, `setpriv` drops
back to the sudo caller's UID, GID and supplementary groups, with a clean
environment, before the unprivileged wrapper runs. The system manager (PID 1)
owns the scope; it is independent of the user's login sessions and
`user@UID.service`. The identity then also records
`tracking: "cgroup-v2"`, its cgroup path/device/inode, the cgroup mount identity
and boot ID.

The outer sudo stays in the SSH connection's `session-N.scope`, outside the
job's scope. Ending that login (`systemctl stop session-N.scope`,
`loginctl terminate-session` or `terminate-user`) sends its processes SIGTERM
and SIGHUP, and sudo relays them to its child (with a pty, to the child's
process group). A scope separates cgroups, not signals. So the wrapper's first
step inside the scope is to fork: only a waiter stays sudo's child and reports
the wrapper's exit status, while the wrapper continues in a new session in the
scope, reparented to PID 1. A relayed signal ends only sudo and the waiter; the
wrapper and everything it started keep running and write their terminal
receipts. The wrapper itself stays killable directly. Session-fallback jobs run
in the login's own scope and do not outlive a stopped login. Logind does not
stop a login that is already closing again on `terminate-user` (Mint 22.3,
`KillUserProcesses=no`); sudo then stays until that login is stopped.
`staging-result.json` is
written only after the staging shell and all its descendants have exited. A
lost SSH reply, timeout, or Ctrl-C therefore does not establish completion.
Failed staging reconciles the same job for up to 30 seconds, independently of
best-effort log collection. It records `staging-reconciliation.json` on the host
when available and releases `install-active` only after proving completion (or
fencing a launch that never started). A second Ctrl-C while writing the failure
receipt or reconciling staging prints recovery guidance even if the host failure
receipt could not be saved. Guidance checks the actual `install-active` state:
held, released, owned by a replacement job, or unknown if the guest cannot be
inspected. An interruption after release therefore does not claim the token is
still held. Held or unknown state includes the exact `recover-staging` command.
The cancellation fence also prevents a delayed SSH command from starting after
recovery. The controller never stops
staging by process name or kills a privileged apt process through its
unprivileged wrapper.

If the guest is unreachable, staging is still running after the bounded wait,
or the wrapper exited without a terminal receipt, the lock stays held and
retries are refused. Acquisition and both release paths serialize their token
checks and updates under the same guest `install-guard` flock (five-second
acquisition limit), so concurrent recovery cannot remove a replacement job.
Inspect `staging-process.json`, `staging-result.json`,
`staging-output.log` and `staging.log` for the reported job. When the same guest
is reachable and staging has finished, use the exact job ID from the error:

```sh
python3 tools/lab/lab.py recover-staging --run "$lab_run" \
  --job install-0123456789abcdef0123456789abcdef
```

This command first takes the host's `install.lock` flock in the run directory
without waiting. An `install` controller holds that lock from guest lock
acquisition until it returns. The kernel drops it when the controller exits for
any reason. While the controller is alive, recovery refuses. That includes the
window after successful staging while it writes `helper.json`, uploads
`package.deb` and launches the supervisor, when the guest shows no
supervisor progress yet. A second `install` on the same run is refused the same
way. The command then authenticates the owned running overlay and rejects a
changed lock token. If the package supervisor has started (`progress.json` or
`result.json` exists), it switches to the supervisor release below. Otherwise
it repeats the bounded guest lifetime check before releasing that token.

The root package supervisor is launched through the same system-scope mechanism,
in a separate `putgd-supervisor-install-<id>.scope`. Its wrapper records
`supervisor-process.json` before sudo; the root helper verifies that sudo kept
it in that exact scope before admission. Scoped launches pass `--require-scope`,
so missing or session-only supervisor metadata cannot downgrade that check.
Legacy callers and the explicit unavailable-manager fallback retain their
existing session behavior. It never migrates a root process into
a caller-selected cgroup. The normal install evidence includes that identity.
The root package supervisor also admits itself under `install-guard`. It
becomes a subreaper first, then starts only while its own job holds
`install-active`, and writes `progress.json` before releasing the guard. A
failure before admission leaves no progress or result, so the job stays
recoverable. After admission, the supervisor's own failures also end in
`result.json`.

Staging recovery checks the token and the absence of `progress.json` and
`result.json` twice. The first check happens before reconciling staging. The second happens
in the release, which holds `install-guard`. Each check is a separate
fail-closed shell statement. A delayed supervisor start therefore either
leaves `progress.json` before the release (recovery refuses and keeps the lock),
or finds the token gone or replaced. In that case it exits with
`Install supervisor refused to start` and writes no progress or result. The
supervisor never runs without the lock.

A controller that dies after admission leaves `install-active` held by a
supervisor that keeps running and finishes on its own. `recover-staging`, with
or without `--abandoned`, releases that job only when all of these hold: the
supervisor's terminal `result.json` exists (a root-owned, single-link regular
file with `ok`, `aborted` and `remainingPids`); its wrapper's `staging.lock`
flock is free; its recorded `putgd-supervisor-install-<id>.scope` reports
`populated 0` or is authenticated gone (same rules as below); and the wrapper's
leader is dead with an empty session. The release re-checks the token and
`result.json` under `install-guard`, then writes `supervisor-recovery.json` on
the host. `progress.json` without a terminal result, a populated or unreadable
cgroup, and a session-tracked or identity-less supervisor (no scope can prove
its root descendants exited) keep the lock. A failed install (`ok: false`) is
released with a warning; inspect `result.json` and dpkg state before the next
install. Each refusal names its reason (the guest's stderr stays hidden).

Recovery that reconciles a started staging requires a verifiable session leader
identity and no live process in the recorded session. That covers ordinary
recovery after a recorded start, and every `--abandoned` recovery. The scan
enumerates `/proc/<pid>/stat` session IDs (field 6), including root-owned
staging descendants. An empty session must be empty on two consecutive passes,
because a member that forks and exits during a single pass can be missed.
For cgroup-tracked jobs these checks are additional guards: recovery also
requires the recorded cgroup's `cgroup.events` to report `populated 0`. This
kernel flag is atomic and includes processes in delegated child cgroups,
including descendants that called `setsid()`. Reading `cgroup.procs` is not an
emptiness test. If systemd has already removed the empty scope, recovery may
instead prove its directory gone on the same boot and cgroup mount, together
with all the original identity/session checks. A missing or unreadable
`cgroup.events` in an existing directory, a replaced cgroup inode, changed
boot/mount, invalid metadata or any uncertain inspection retains the lock.
Missing identity for `--abandoned`, unreadable process information or
incomplete enumeration (including restricted procfs mounts) refuses recovery.

An abrupt controller death or an interrupted receipt write can leave no host
`staging-failed.json`. Once the controller has exited, recovery works from the
guest's state under the same ownership, token, supervisor and lifetime guards.
If the guest has `staging-process.json`, recovery reconciles that identity.
If staging never recorded an identity, the controller died before the
wrapper took `staging.lock` and started its script. The wrapper may not have
launched yet, or may be waiting for that lock. Ordinary recovery then fences
a delayed wrapper launch under `staging.lock`, reports `staging-not-started`
and releases the token. `--abandoned` refuses in that case, because it
requires a recorded identity. The command saves `staging-recovery.json` in the
host job directory and preserves the install-time
`staging-reconciliation.json`. It refuses a running or uncertain job. It does
not resume the failed install; run `install` again after recovery succeeds.

If the wrapper died without writing `staging-result.json`, ordinary recovery
continues to refuse. Inspect the recorded PID/start ticks, logs and descendants
before explicitly abandoning that attempt:

```sh
python3 tools/lab/lab.py recover-staging --run "$lab_run" \
  --job install-0123456789abcdef0123456789abcdef --abandoned
```

`--abandoned` requires every recovery guard above, an unheld guest
`staging.lock` flock, a valid recorded session leader PID that is dead or has
different `/proc` start ticks, and a complete scan proving no live process has
the recorded session ID. Cgroup-tracked jobs additionally require atomic
cgroup emptiness or the authenticated gone-scope proof above. Killing the wrapper can free its flock while privileged
children still run. Children that remain in the staging session keep recovery
blocked until they exit. Zombie or exited processes do not count as live.

The launcher probes the cgroup v2 mount, `systemd-run`, `systemctl`, `sudo`,
the scope launcher (root-owned, not group- or world-writable), system-manager
bus access and passwordless sudo authority by actually executing
`sudo -n -- /usr/local/lib/putgd-lab/package_scope.py --probe` (a successful
sudo policy listing alone is insufficient). The probe checks `systemd-run`,
`setpriv` and `env` as root and creates no unit. The lab user is never granted
`systemd-run` itself: with caller-chosen arguments it is root-equivalent. The
launcher accepts only `staging` or `supervisor` and an `install-<32 hex>` job
ID; the unit name, every `systemd-run` property, the `setpriv` drop to the sudo
caller and the clean environment are fixed. Only words after those reach the
unprivileged command.

New guest bootstrap installs `setpriv` through `util-linux`, installs the
launcher and grants it beside apt and the supervisor. Existing goldens and
snapshots are never changed: every DEB `install` stages the launcher and its
rule into that run's own overlay, with the overlay's existing apt authority, as
it does the supervisor. A fresh overlay's first staging therefore uses the
session fallback (`fallbackReason`: the scope launcher is not installed); its
supervisor and every later job in that overlay use system scopes. The launcher
requires **systemd 254 or newer**, checks the version before
using `--expand-environment=no`, and falls back clearly on an older or
unverifiable version. The user manager and user bus are not prerequisites;
no linger setting is changed. Each failed prerequisite has its own
`fallbackReason`. If a prerequisite is unavailable, the job keeps the existing
session scan and `tracking: "session"`. The guest prints the fallback and the
controller reports it at install time for staging and supervisor phases.
`staging-tracking.json` and `supervisor-tracking.json` retain the selection.
A scope-creation or scope-entry failure after selection fails the launch;
it never retries the command outside the scope. The original user-manager
mechanism was rejected after B1 reproduced watchdog loss when the last login
ended. Its earlier SSH/bus checks did not prove logout survival. Corrected
Mint 22.3 and Ubuntu 26.04 logout/controller-SIGKILL evidence uses the new
system-manager mechanism: both the graphical login and any closing SSH login
were ended, `Linger=no` was retained, and `user@1000.service` was observed
inactive while the root helper survived and then wrote `result.json`. See the
[B1 evidence](evidence/2026-10-30-staging-cgroup-b1/README.md). That run
ended the closing login with `kill-session --signal=KILL`, which sudo cannot
relay. Independent validation then showed a plain stop of that login killing
the supervisor wrapper on both guests, and `terminate-user` killing it on
Ubuntu (sudo-rs), through sudo's relay. The
[login-end evidence](evidence/2026-10-30-staging-cgroup-f1/README.md) repeats
plain `systemctl stop session-N.scope` and `loginctl terminate-user` during
both staging and the supervisor on Mint 22.3 and Ubuntu 26.04, with the real
`install` controller killed, then releases each finished job through
`recover-staging`. Other guest profiles use the same runtime checks;
Aurora/rpm-ostree's AppImage route does not run the APT path.

Cgroup tracking covers normal inherited descendants, including `setsid()`.
A privileged hook can deliberately leave the scope by writing `cgroup.procs`,
starting a new unit with `systemd-run`, or asking `systemctl start` to launch a
service elsewhere. The required session scan remains as a second guard for
out-of-cgroup processes that retain the recorded session ID. Leaving needs no
root, either: while the lab user's manager runs, any of the job's processes
running as that user can start `systemd-run --user --scope` and move into
`user@1000.service`, which its own user manager owns. The lab user's own
`sudo` grant for `package_scope.py` likewise lets a job process start a
lab-UID process in a new system scope that outlives logins. The same session
scan applies to both. A process that both leaves the cgroup and changes
session (root or not) can escape both checks.

Recovery trusts only the job's own scope: the recorded unit must be
`putgd-staging-<job>.scope` or `putgd-supervisor-<job>.scope` under
`/system.slice`. Any other unit or path, including a legacy user-manager
scope, reads as unknown and keeps `install-active`.
Tracking is a lifetime fence for the staged command tree, not a security
boundary against a deliberately privileged migrator.

**Only session-tracked jobs retain the following limits.** A live wrapper waits,
as subreaper, for every descendant, including ones that called `setsid()`.
After the wrapper is killed, a descendant that left the session with `setsid()`
is invisible to the scan. The `/proc` scan is also not atomic: a process that
keeps forking and exiting could evade both passes. The in-tree staging hook
starts no such daemon, but a guest's own APT Pre-Invoke hook could. Session-only
reports retain both warnings in `limit`; inspect the guest process list when
its hooks can daemonize. Cgroup-tracked reports have no session-scan `limit`.
Recovery never signals or freezes processes.

A held flock, live matching leader,
live session member, populated cgroup, malformed identity or uncertain inspection retains
`install-active`; so does a missing identity under `--abandoned`. Recovery
writes `staging-recovery.json` only after the guarded release succeeds. Older receipts without a session ID cannot prove abandonment;
preserve the run for investigation.

On guests without Python 3.9's `os.pidfd_open`, or where the kernel returns
`ENOSYS` for that API, leader identity checks fall back to `/proc` start ticks
and process state. `pidfd_open` `EINVAL` means the leader exited but its PID is
still in use as the ID of a session with live members. Recovery counts the
leader as dead and refuses, with those member PIDs from the session scan in the
report and the error. Other inspection failures remain unknown and cannot
authorize abandonment. The command never signals a process. It fences delayed starts,
records a `staging-abandoned` report in the host's `staging-recovery.json`, and
clears only the matching install token. The missing terminal receipt remains
missing; recovery proves process quiescence rather than a successful helper
installation. These rules apply only to disposable overlays; immutable goldens
are never changed.
See the [Cinnamon cancellation proof](evidence/2026-10-30-lab-staging-748/README.md)
for the earlier exact identities, refused retries, later successful install and cleanup.
The [cgroup recovery regression](evidence/2026-10-30-staging-cgroup-780/README.md)
records the #780 guest identities, setsid/SIGKILL proof, fallback unit cases and
resource retirement.

This uses authority already granted to apt in the disposable guest. It never
changes the host, golden, bootstrap key or `authorized_keys`, and needs no guest
password or console bootstrap rerun. Keep the golden immutable; provision a new
golden for lasting bootstrap changes. Package dependency resolution uses distro
repositories.

The VM apt path rejects RPM inputs; `physical.py` retains its existing dnf/RPM
install path. DEB `install` has a 900-second apt budget and a further 600-second
recovery window.
Profiles can set positive integer `install_timeout` and
`install_recovery_timeout` values; CLI `--install-timeout` and
`--install-recovery-timeout` override them. The guest supervisor survives an SSH or
controller disconnect, waits for only its own descendants after the apt budget
expires, and never starts a replacement apt command during recovery. It verifies
`dpkg -s putgd` and a clean `dpkg --audit` before recording success in
`result.json`. The controller then verifies the installed binary hashes in the
guest before recording the package receipt. Recovery can succeed after the
initial timeout if the owned install finishes; that timeout remains recorded in
the receipt.

Each attempt retains `apt.log`, PID/start identities, `progress.json`, and
`result.json` (including dpkg state) under `~/putgd-lab/install-<id>` in the guest
and copies completed evidence into the host run's matching directory. If the
recovery window expires, the supervisor freezes and kills only descendants pinned
by pidfds, then reports any remaining PIDs and package state. It never kills by
name or retries apt. The guest `~/putgd-lab/install-active` lock blocks retries
through binary verification after successful package-manager completion. A failure
before staging is launched, confirmed staging completion before the supervisor
starts, or a later collection/verification failure after a successful
package-manager report releases only that job's lock. Unresolved or
failed package-manager attempts retain it. A refused retry reports the held job
ID, summarized result/progress state, UTC start time from the job marker, and
guest `result.json` path without creating another host evidence directory.
The complete report, including owned process identities, stays in the evidence file.
If the supervisor produces no progress within 15 seconds, installation fails early
and points to `supervisor.log` (which may itself be missing); the lock stays held
because a lost launch reply does not prove no process started.
For those attempts, inspect the recorded
job/result, reconcile the exact owned process identities and dpkg recovery state, and acknowledge recovery by removing only that
job's lock before another install. Do not remove a lock while its supervisor or
package descendants are still running; uninterruptible IO may require retaining
the stopped run for investigation.

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
python3 tools/diagnostics/check_socket_paths.py --socket "$lab_run/qmp.sock" "$lab_run/vnc.sock"
python3 tools/lab/lab.py profiles
python3 tools/lab/lab.py launch --profile cinnamon-x11 \
  --golden work/lab-images/cinnamon-x11.qcow2 --run "$lab_run" \
  --bootstrap-key work/lab-bootstrap/identity \
  --known-hosts work/lab-bootstrap/known_hosts
python3 tools/lab/lab.py install --run "$lab_run" --deb "$lab_deb"
python3 tools/lab/lab.py run --run "$lab_run" --scenario region --keep
```

`install` takes `--deb` for package profiles or `--appimage` for profiles with
`"install": "appimage"`; it rejects the other kind. `start --run RUN` starts the
installed resident in the native session with the same `putgd-launcher
--background` command the scenarios use, waits for the production CLI to
answer, and checks the running executable against the install receipt; use it
for smoke and `cli` checks on guests without the lab fixture.

Session commands run put.gd with private `XDG_CONFIG_HOME=~/putgd-lab/config`
and `XDG_DATA_HOME=~/putgd-lab/data`. For AppImage profiles,
`~/putgd-lab/data/applications` is a link to the session's own applications
directory (its `XDG_DATA_HOME`, else `~/.local/share`). AppRun writes the KWin
permission entries and the window-icon entries there, and the compositor reads
only the session's directory
([#523](evidence/2026-10-23-appimage-icon-523/README.md)). put.gd's own data
stays private. An `applications/` directory that already holds files is left
as it is.

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

## Multi-head guests

A guest can have up to four virtual monitors. Set `display_heads` (1-4) in a profile, or pass
`--heads N` to `launch`. `--layout` (or a profile `head_layout` list) gives each head's size and
place in head order, as `WxH+X+Y` entries that may use negative origins and gaps:

```sh
python3 tools/lab/lab.py launch --profile cinnamon-x11 --golden GOLDEN --run "$lab_run" \
  --bootstrap-key KEY --known-hosts KNOWN_HOSTS --vnc-display 60 \
  --layout 1920x1080+0+0,1280x1024-1280+0
python3 tools/lab/lab.py layout --run "$lab_run"
```

- **QEMU:** the profile's `virtio-vga` (or `virtio-gpu-pci`) device gets `max_outputs=N`
  and id `video0`. Each head has its own loopback VNC listener on consecutive displays:
  `--vnc-display 60` with three heads listens on 5960, 5961 and 5962. A single-head launch
  keeps its exact previous command line.
- **Connected heads:** virtio-gpu reports only head 0 as connected until a display client
  sends size information for a head. Right after QEMU starts, `launch` sends each head an RFB
  SetDesktopSize request (the layout's size, or 1280x800), so the guest boots with N connected
  heads and that size as each head's preferred mode. Without it a second output stays off, as
  #108 found, and `xrandr --setmonitor` splits of one output are not a substitute.
- **Run metadata:** `state.json` and the `launch` result list every head's port under
  `heads`, plus `head_layout` and the QEMU answer to each size request.
- **Layout and verification:** `layout --run RUN [--layout SPEC]` re-sends the head sizes,
  arranges the outputs in the native session and verifies them. It uses `xrandr` on X11,
  `kscreen-doctor` on Plasma and Mutter's DisplayConfig D-Bus API on GNOME, with head N on
  connector `Virtual-(N+1)` and head 0 as the primary output. It passes only when the guest
  reports exactly N enabled outputs in the requested arrangement, each X11 RandR monitor is
  its own output, and each head's QMP `screendump` has the head's size. Results go to
  `layout.json` in the run directory.
- **Origins:** X11, Mutter and KWin all place the desktop's top-left corner at 0,0, so a
  negative origin becomes the same arrangement shifted, with head 0 still primary (for
  example a monitor left of the primary). Mutter also rejects gaps between monitors; X11
  and KWin keep them.
- **Input per head:** each head has its own `usb-tablet` bound to that head's console, with
  serial `putgd-headN`, so a VNC client on head N sends head-relative coordinates. On X11,
  `layout` maps each tablet to its output with `xinput map-to-output`. `input --run RUN
  --head N 'JSON_EVENTS'` sends QMP input through head N's tablet. QMP input without
  `--head` still goes to the unbound tablet, which spans the whole desktop.
  Wayland guests can't map them yet: KWin and Mutter scale every QEMU tablet over the whole
  desktop, KWin ignores `outputName` and `calibrationMatrix` for them (and stores one setting
  for all three identically named tablets), and a udev calibration rule needs guest root.
  There, pointer input on a multi-head guest lands at the head-relative position scaled to
  the whole desktop; keyboard input is unaffected.
- **Screenshots:** `screenshot --run RUN --head N` saves head N's frame.

## Retire completed overlays

Run lifecycle commands on the actual QEMU host. A sandbox with a filtered process
view cannot certify that a VM stopped. Linux launch/reset, Windows lifecycle
operations and overlay cleanup share a host lock. Cleanup also checks actual
process/open-file identities and QEMU backing chains, identified by their image
headers rather than filename extensions. It holds a native QEMU image lock
through final verification and unlink, preventing a competing writer from
opening the disk. The process inventory must also rule out ordinary read-only
file descriptors and mappings, including those held by root or another user.
If a process is unreadable, cleanup retries the same read-only inventory with
`sudo -n -- /usr/bin/python3 -I -B`; it does not run the lab controller, import
checkout modules at elevation or delete files as root. The elevated program is
the scan function's source, captured once when cleanup is imported. A regular
user can't read `/proc/1/fd`, so in practice every host takes this path: boat's
`user` runner and all five builders already allow `/usr/bin/python3` through
`sudo -n` without a prompt. No sudo rule is installed or changed. The direct
scan alone is enough only where every process is readable, such as cleanup run
as root.
Unavailable sudo, unreadable elevated inventory, changed process identities,
`hidepid` and nested PID namespaces refuse cleanup; a native image lock alone
cannot establish that an opaque process has no read-only descriptor.
Incomplete image inventories also refuse cleanup. It refuses running,
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


### Retire a pre-existing run outside the canonical root

The cleanup guard deliberately refuses an old run outside this checkout's
`work/lab-runs`. Do not bypass the guard, use a symlink, or delete its disk by
hand. Complete the following on the actual QEMU host, after its campaign and
investigation finish:

1. Export and checksum its evidence, original `state.json`, exact QEMU identity,
   backing chain and old absolute run path. Stop the exact owned VM with
   `lab.py reset --run OLD_RUN`, then verify that process has exited and no
   process holds the overlay. Inventory backing chains and confirm no other
   overlay references a disk inside the run before relocating it. Preserve all
   golden disks and their paths.
2. Choose an unused, short, symlink-free direct child of this checkout's
   `work/lab-runs`. Move the stopped run directory there as a complete bundle.
   Preserve the original state as an evidence file; update only the active
   `state.json`'s `overlay` field to the new absolute `disk.qcow2` path. Preserve
   the run ID, golden path/hash and historical results. Record both paths and
   the unchanged overlay/backing identities in relocation evidence. Never
   resume this retired run or rewrite historical acceptance records.
3. Export and verify that relocation evidence. Create the explicit completion
   manifest described above using the **new** absolute run path and original
   run ID, and include the original state and relocation record as hashed
   members. Run canonical `cleanup --run NEW_RUN --evidence-manifest MANIFEST
   --dry-run`, then the same command without `--dry-run`. For a golden in
   another checkout, supply its owned canonical `--images-root .../work/lab-images`.
   Retain the cleanup receipt alongside the evidence.

The guard still checks ownership, actual processes, all backing dependencies,
image locks and completion evidence after relocation. A stopped VM alone is
insufficient authorization to retire its disk.


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
| `tools/lab/edit_acceptance.py RUN` | Graphical Edit & upload: hotkey, region-selected source, every tool, undo/redo, crop, save copy, submit, cancel, PNG sanitization and screenshots. It drives the editor at its opening Fit zoom: the scale comes from `editor.view` and `lab.locate_scaled_fixture_paint` finds the scaled image from its quadrant boundaries. On GNOME, restart the guest session after installing a new package so gnome-shell loads its put.gd extension (an older one lacks the edit shortcut). |
| `tools/lab/edit_memory.py RUN [SAMPLER]` | Whole-family RSS+swap (`family_sampler.py` in the guest) for desktop, region and 24 MP file cases. Gate: incremental peak at most 3R + 96 MiB, and the resident returns within 8 MiB of baseline once the fixture takes the clipboard. |
| `tools/lab/mcp_editor_e2e.py EVIDENCE` | Official MCP client through `putgd-mcp` and the target's production `putgd-cli` (a small wrapper running it over pinned SSH): all editor tools, preview image, save copy, submit and cancel. |
| `tools/lab/guest_reboot.py RUN` | Reboot through the session manager when a session-loaded integration such as the GNOME extension must reload; verifies the boot time changed. |

On the `fedora-gnome-wayland` golden the session manager ignores that reboot
request and the `lab` account may run only `dnf` through `sudo`. After
installing the RPM in a run's overlay, `sync` in the guest and reset the
disposable VM through its QMP socket (`system_reset`); GDM auto-logs `lab`
in and the new Shell loads `putgd@put.gd` (seen ACTIVE on 2026-10-18), so the
capture scenarios bind their shortcuts without a new golden. Never reset a
golden; only a run's own overlay.

`tools/lab/physical.py HOSTCFG.json install DEB|RPM|SCENARIO|script DRIVER` runs the
same runner and drivers on a physical desktop. `SCENARIO` is any `lab.py run
--scenario` name, such as `region` or `window`. It swaps the runner's shell,
copy and screenshot helpers for pinned SSH, `scp` and the host's loopback VNC
tile (`tools/lab/rfb.py`, security type None only), reads input from the
forwarded agent above, and labels results `graphical-physical`. Keep the host
config and its run directory under `work/` or the scratchpad; they name only
key and known-hosts paths, never secrets. A package built without
`PUTGD_LAB_FIXTURE` has no `putgd-desktop-fixture`; set the host config's
optional `fixture_bin` to the host directory holding a lab-built one.

Window capture includes the window frame where the desktop provides one. On
Plasma/Wayland, KWin draws that frame server-side and native Qt reports zero
frame margins, so the runner asks KWin's `queryWindowInfo` picker for the
frame whenever the fixture reports a KDE Wayland session. It decides from that
observed session, not the profile name, so physical and VM profiles get the
same check.

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

Aurora (Universal Blue, Fedora Atomic KDE) is an image-based evaluation guest
for AppImage and Flatpak testing. It needs UEFI firmware, has no package
manager and uses plasmalogin instead of SDDM. The `aurora-kde-wayland` profile
sets `"firmware": "uefi"` (the runner adds the host's read-only
`OVMF_CODE_4M` image and a private per-run copy of `OVMF_VARS_4M`, found under
`/usr/share/edk2/ovmf` on Fedora or `/usr/share/OVMF` on Debian, and records
both in `state.json`) and `"install": "appimage"` (`install --appimage` instead
of `--deb`). Its goldens on `f44`, their provisioning and the runner commands
are in [Aurora VM](aurora-vm.md). BIOS profiles have no `firmware` key and
their QEMU command is unchanged.

KDE neon (User for released Plasma, Unstable for Plasma git master) uses the
`neon-kde-wayland` profile (BIOS, `install: appimage`) and its own
`bootstrap-neon-guest.sh`, because neon uses plasmalogin and wraps `apt-get` in
`/usr/sbin`. Its goldens and provisioning are in [KDE neon VMs](neon-vm.md).

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


## Visible guest preflight

[Code PR #584](https://github.com/patrick-hudson/put.gd-desktop/pull/584) adds the
common disposable-guest visibility preflight. `lab.py launch` applies it after
authenticated SSH and before returning; `start` also applies it. Graphical
scenarios use check-only preflights both before and after the workload, so policy
drift after setup is refused rather than silently repaired. Custom
acceptance drivers must use the same boundary checks:

```sh
python3 tools/lab/lab.py preflight --run work/lab-runs/RUN --apply --stage before-campaign
python3 tools/lab/lab.py preflight --run work/lab-runs/RUN --stage before-idle
# Run the unchanged measurement driver, with the visible wall maintained.
python3 tools/lab/lab.py preflight --run work/lab-runs/RUN --stage after-idle
```

The host authenticates the owned running QEMU process, disposable overlay and
forwarded SSH endpoint before sending the helper. The guest rejects non-QEMU/KVM
machines and derives configuration homes from the actual desktop process. Do not
read desktop policy through `guest_session`'s product-specific XDG homes.

GNOME and Cinnamon disable idle activation, locking, dimming and inactive sleep
on both power supplies. Plasma verifies lock and all AC/Battery/LowBattery
power profiles and reloads the live services. A user service holds a verified
logind idle/sleep inhibitor; initial acquisition may take up to five seconds.
Initial GNOME/Plasma service readiness is bounded to 30 seconds; Cinnamon
ScreenSaver is demand-activated and must return inactive from `GetActive`.
Initial complete-output readiness is bounded to 15 seconds per head, retaining
every blank attempt. Check-only boundaries neither repair drift nor wait for
missing services, inhibitor or visible frames.
X11 additionally disables and reads back screen saving/DPMS. Native session type,
active seat, unlocked state, inactive screensaver, configured outputs, complete
QMP frames and VNC extents must agree. A uniform RGB color is not a visible
desktop witness, even when its component channel values differ. These checks do not replace a geometry
campaign's independent all-head fresh-motion oracle or human visual review.

New provisioning seeds for generic, Aurora and neon guests must contain
`guest_idle.py` beside `bootstrap.sh`. Bootstrap installs the helper and a desktop
autostart entry for future disposable logins. The normal launch/start checks still
verify the effective live policy before acceptance. Existing immutable goldens
are never edited to retrofit it: apply the helper in the fresh owned overlay.
For an older golden, install the companion helper with `--install-autostart
--user lab` inside its fresh owned overlay before any planned later login;
`launch` applies live policy but does not itself install that autostart file.
An existing unrelated helper/unit is a collision and is preserved; inspect it
instead of overwriting it. The user inhibitor ends when the disposable guest is
retired through normal guarded cleanup.

Any failed policy, active screensaver, locked session, missing output or blank
witness blocks acceptance. Once the run directory is verified as owned,
non-symlinked and not writable by other users, early ownership/readiness/profile
failures also retain `guest-preflight-*.json`. Unsafe receipt directories are
refused without writing or contacting a guest. Available visuals are preserved. Restore a locked or blanked guest through its visible console before
retrying. Preserve the failed attempt. Never apply these settings to a physical
user desktop or treat an SSH-only check as complete graphical evidence.

A failed launch attempts a bounded guest `sync` before stopping its owned QEMU
process, retaining `failure-shutdown.json`. The generated run public-key append
is newline-separated and synced before preflight, so a failed initial launch
does not rely on guest page-cache persistence. This does not authorize stopping
an unrelated guest or altering existing keys.
