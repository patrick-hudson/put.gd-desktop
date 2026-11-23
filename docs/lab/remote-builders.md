# Preferred build and graphical test servers

Patrick's five physical Linux servers are the preferred project build, VM and
acceptance hosts. Choose a host by required tools/profile and available
capacity; use workstation-local VMs when the servers are unavailable or cannot
run the required profile. Record the reason for each fallback. Work under
`~/work/<project>` and preserve existing checkouts, builds, desktops and user work.

| Host | SSH address | Existing physical desktop | Workstation VNC port | Workstation user service |
| --- | --- | --- | --- | --- |
| Fedora 44 Workstation | `patrick@192.168.68.70` | GNOME on Wayland | `127.0.0.1:5970` | `putgd-fedora-vnc.service` |
| Debian 13 | `patrick@192.168.68.69` | KDE Plasma on Wayland | `127.0.0.1:5979` | `putgd-debian-vnc.service` |
| Kubuntu 26.04.1 LTS | `patrick@192.168.68.72` | KDE Plasma 6.6.6 on Wayland (default since 2026-10-20; X11 session on request) | `127.0.0.1:5972` | `putgd-kubuntu-vnc.service` |
| Linux Mint 22.3 | `patrick@192.168.68.74` | Cinnamon 6.6.4 on X11 | `127.0.0.1:5974` | `putgd-mint-vnc.service` |
| Aurora Fedora 45 (`aurora-amd`) | `patrick@192.168.68.71` | Plasma 6.8.2 on Wayland (follows `aurora:testing`, so it moves) | `127.0.0.1:5976` | `putgd-aurora-vnc.service` |

The user authorizes full root access on these five machines through passwordless
sudo: install normal distro dependencies, configure build tools, compile and run
tests using `sudo -n COMMAND` (or `sudo -n -i`). `sudo -n id -u` must report `0`.
Do not apply this remote authorization to the workstation or unrelated machines.
If noninteractive sudo fails, report it rather than requesting or recording a
password. Keep distro Qt dynamically linked and use the project's pinned official
AWS SDK; see [build instructions](../release/build.md).

The [Linux acceptance matrix](../testing/acceptance-matrix.md) covers Mint, Ubuntu, Kubuntu,
Fedora and Debian, GNOME/KDE, and native X11/Wayland. Combine host desktops and
builder-hosted VMs as needed; physical-hardware release gates require the
appropriate physical machine. Provision missing profiles and record local fallbacks.

## Graphical acceptance on the physical desktops

The build servers themselves are authorized graphical acceptance targets. Build,
install and run the candidate directly in Fedora's GNOME/Wayland session,
Debian's or Aurora's Plasma/Wayland session, Kubuntu's selected Plasma session,
or Mint's Cinnamon/X11 session. Use the
corresponding physical-host tile on the workstation wall, preserve the user's
other windows/projects, and save screenshots or recordings of the actual app
behavior and failures.
Record the host OS, compositor/session version, exact source and running build,
scenario and result. SSH output or an offscreen test alone is insufficient.

These host-desktop checks do not require a VM or `/dev/kvm`. Select direct host
testing when its actual desktop/version matches the claim being tested; use a
VM for other required guest profiles. Preserve the release matrix's exact versions.

### GNOME rows run with a tray host

Ubuntu enables its AppIndicator extension by default, and a Fedora user who wants put.gd's
tray turns it on ([GNOME](../desktops/gnome.md)). So GNOME acceptance on f44 runs with a
tray host by default. f44 has Fedora's `gnome-shell-extension-appindicator` installed, and
`appindicatorsupport@rgcjonas.gmail.com` is in the desktop user's
`org.gnome.shell enabled-extensions`, next to `putgd@put.gd`. A fresh login therefore loads
it. Check it with `gnome-extensions info appindicatorsupport@rgcjonas.gmail.com`
(`Enabled: Yes`, `State: ACTIVE`).

You don't have to remember to check. `desktop_lock.py acquire f44` runs the same check
the nightly's Fedora GNOME guest runs: the extension must be enabled and active, and an
`org.kde.StatusNotifierWatcher` on the desktop session bus must have a registered host.
If either is missing, the lock is refused with one line that says what's wrong and how to
fix it. Usually the fix is `gnome-extensions enable appindicatorsupport@rgcjonas.gmail.com`
in the desktop session. If the extension stays `INACTIVE`, the repair touches the desktop,
so take the lock first with `--no-tray-host "repairing the tray host"`. Under that lock,
unlock the screen if it is locked (a locked screen also makes the extension `INACTIVE`),
check again, and only if it is still `INACTIVE`, re-login with
`sudo -n systemctl restart gdm`.

**Testing the no-tray path on purpose.** A case that covers GNOME without a tray, such
as quitting from the main window's menu or the prompt fallback when no tray can show it,
takes the lock with `--no-tray-host "<reason>"`. The lock then skips the tray check and
adds the reason to its purpose, so `status` shows why the check was skipped. Disable the
extension for the test with `gnome-extensions disable appindicatorsupport@rgcjonas.gmail.com`.
Before you release, turn it back on with
`gnome-extensions enable appindicatorsupport@rgcjonas.gmail.com` and confirm
`IsStatusNotifierHostRegistered` is true again, so the next session gets a tray. A row
that ran without a tray says so in its record.

## Pinned SSH access

Connect from the main workstation on the LAN. All five hosts accept
`/home/patrick/.ssh/awskey_110724`. Keep that private key on the workstation:
never copy it into a server, VM, source export, image or evidence packet. Use
the existing host pins; do not replace them or disable host-key verification.

```sh
# Fedora
ssh -i /home/patrick/.ssh/awskey_110724 -o IdentitiesOnly=yes \
  -o StrictHostKeyChecking=yes \
  -o UserKnownHostsFile=/home/patrick/work/put.gd/work/remote-builders/f44/known_hosts \
  patrick@192.168.68.70

# Debian
ssh -i /home/patrick/.ssh/awskey_110724 -o IdentitiesOnly=yes \
  -o StrictHostKeyChecking=yes \
  -o UserKnownHostsFile=/home/patrick/work/put.gd/work/remote-builders/d69/known_hosts \
  patrick@192.168.68.69

# Kubuntu
ssh -i /home/patrick/.ssh/awskey_110724 -o IdentitiesOnly=yes \
  -o StrictHostKeyChecking=yes \
  -o UserKnownHostsFile=/home/patrick/work/put.gd/work/remote-builders/k72/known_hosts \
  patrick@192.168.68.72

# Linux Mint
ssh -i /home/patrick/.ssh/awskey_110724 -o IdentitiesOnly=yes \
  -o StrictHostKeyChecking=yes \
  -o UserKnownHostsFile=/home/patrick/work/put.gd/work/remote-builders/m74/known_hosts \
  patrick@192.168.68.74

# Aurora Fedora 45
ssh -i /home/patrick/.ssh/awskey_110724 -o IdentitiesOnly=yes \
  -o StrictHostKeyChecking=yes \
  -o UserKnownHostsFile=/home/patrick/work/put.gd/work/remote-builders/aurora-amd/known_hosts \
  patrick@192.168.68.71
```

For unattended commands, also use `-o BatchMode=yes` and a bounded connection
timeout. A missing pin or mismatch requires verified identity recovery, not
trust-on-first-use. Local bootstrap notes and tunnel units live privately under
`work/remote-builders/f44/`, `work/remote-builders/d69/`,
`work/remote-builders/k72/`, `work/remote-builders/m74/` and
`work/remote-builders/aurora-amd/`. The `.71`
address previously belonged to a Windows builder; use the Aurora pin directory
for this host and preserve the historical Windows records.

## Aurora AMD Fedora 45 builder

`aurora-amd` (`192.168.68.71`) is a preferred build and native Plasma/Wayland
test host. Its 2026-10-29 bootstrap ran Aurora `testing-45.20261029.1` on an AMD
Ryzen 5 PRO 8500GE / Radeon 740M, with 16 GB RAM and a 512 GB NVMe drive.
The native session used Plasma/KWin 6.7.5 and distro Qt 6.11.2. On 2026-11-01 it
rebooted into `testing-45.20261101.2` (digest
`sha256:c3358112307abc4602b69fbc647ec46654a864344b541f4580a23a735e9497ca`): Plasma/KWin 6.8.2, Qt unchanged.

This host follows the `aurora:testing` tag with `uupd.timer` enabled. The timer
stages a new image daily, and the next reboot boots it, so **any reboot can change
the desktop version**. Check `rpm-ostree status` and `rpm-ostree db diff` before
relying on one. After a Plasma version change, Plasma opens a Welcome Center window
over the desktop once; close it by PID before desktop tests (its process name is
`plasma-welcome-`, so `pgrep -x plasma-welcome` misses it).

The nightly Aurora row doesn't test this physical desktop. It boots a VM overlay of
the [current Aurora golden](aurora-vm.md#plasma-68-golden) on this host, so the two
versions can differ.

### Build commands

Development packages live in the official Fedora 45 Toolbx `putgd-build`.
Use the installed wrapper on Aurora:

```sh
~/.local/bin/putgd-build-env cmake --preset dev
~/.local/bin/putgd-build-env cmake --build --preset dev
~/.local/bin/putgd-build-env ctest --preset dev
```

With no arguments, `putgd-build-env` opens a shell in that container. Before a
`cmake` or `ctest` command it runs `desktop_lock.py guard` from
`~/.local/libexec/putgd-builder/` and refuses while another session holds the
host's [quiet lease](#quiet-leases-for-timing-runs). The wrapper
selects `/var/home/patrick/work/put.gd`, the canonical path to the prepared
checkout, clears the live graphical session variables for CTest and checks the
test socket paths under a new short private `TMPDIR`. For an isolated checkout,
enter the same container and configure from that checkout's canonical path with
its matching pinned SDK prefix. Exact source/build identity rules still apply.
The `/home/patrick` alias resolves to `/var/home/patrick`; configuring through the
alias caused build-tree S3 and Proton helper lookup to miss their exact directory
match during bootstrap.

The prepared environment has Fedora GCC 16.2.1, CMake, Ninja, Qt 6.11.2 development
packages, QtKeychain 0.17.0, an official AWS SDK prefix and the bundled
Proton runtime. Its SDK must be the release in `cmake/dependency-lock.json`:
`cmake --preset dev` stops when it is not, because `cmake/Dependencies.cmake`
asks for that version `EXACT`. If it differs, or the prefix's origin is unknown,
provision a new prefix in a fresh isolated checkout with
`tools/ci/provision-linux-sdk.sh`, which clones and verifies the locked source
into unused SDK source and build paths. That checkout has no Proton runtime;
for Proton work, provision one there and configure with `-DPUTGD_BUNDLE_PROTON=ON`
as [the build guide](../release/build.md) describes. Preserve the prepared checkout
and its prefix. Qt stays dynamically linked to distro libraries. Fedora's
`openssl3-devel` / `openssl3-libs` 3.5.8 compatibility packages support this SDK;
the OpenSSL 4 dependency update is tracked in
[#646](https://github.com/patrick-hudson/put.gd-desktop/issues/646). The container
also has the GStreamer media plugins and Cisco OpenH264 needed by recording
contracts. Native `openssl3-libs` was added persistently with `rpm-ostree` and
applied live. On 2026-10-29, for VM guests, `qemu-system-x86-core`, `qemu-img`,
`qemu-device-display-virtio-vga` and `dpkg` were layered the same way, after a
`--dry-run` review: 24 packages in all, with no desktop, compositor, Mesa,
PipeWire or portal package, and the base image unchanged.
QEMU runs on the host, not in the Toolbx. The Toolbx doesn't share `/tmp`, where
the Proton account lock lives. `lab.py install` needs `dpkg-deb`.

The host is image-based: use DNF inside Toolbx for development packages and
review `rpm-ostree` transactions for native runtime/package additions. Native
GUI checks use the physical Plasma/Wayland session and its wall tile. Run the
capacity/KVM/display preflight and provision the required guest before VM work;
the bootstrap evidence does not establish VM or release-matrix acceptance.

### Proton, credentials and setup evidence

Patrick signed in independently on this builder. Its existing lab session uses
reference `putgd/proton/lab-aurora-amd` and the native KWallet Secret Service
collection `putgd lab`. Use its installed host adapter:

```sh
~/.local/bin/putgd-proton-lab status
~/.local/bin/putgd-proton-lab check --session ID --owner NAME
```

The adapter in `~/.local/libexec/putgd-proton/aurora_lab.py` selects canonical
profile and broker paths under `/var/home/patrick/putgd-lab/` and adds the native
KDE session variables for GUI work. Its registry is
`~/putgd-lab/locks/proton-registry.json`. The weekly
`putgd-proton-keepalive.timer` is enabled. Take `aurora-amd`'s Proton lock for
session use and its desktop lock for GUI work, using the same commands as the
other builders. KWallet 6.31's Secret Service split requires the prepared
collection; the legacy bootstrap compatibility fix is tracked in
[#647](https://github.com/patrick-hudson/put.gd-desktop/issues/647).

The authorized test credential file is `~/Documents/creds.txt`, owned by
`patrick`, mode `0600`, outside the source checkout. Read it only in memory for
authorized live tests; keep its contents out of logs, diagnostics and exports.

The bootstrap built clean source `9dac325035a28f0f7b00281b4e759ddc3f337b2f`
(`26.1029.2263-dev+g9dac325035a2`) with **113/113 CTest suites passing**. These are
offscreen/Xvfb contracts. A fresh native lab resident then passed the Proton
account/folder check against a new isolated folder; its own Destinations window
was saved as a cropped visual. This proves the native Proton setup only.
Private records, failed attempts and exact identities remain on Aurora in
`~/work/bootstrap-aurora-20261029/` and on the workstation in
`work/remote-builders/aurora-amd/bootstrap/`. The installed lab app's current
prefix is recorded in the registry; use a matching current build for later
acceptance. Display scaling is 100%, and sleep/suspend/hibernate targets remain
masked. The wall and both stats dashboards already include this host.

## Visual results are required

All five physical desktops appear at [the display wall](http://127.0.0.1:8765/).
Tiles start view-only; select **Enable KB + Mouse** to interact, then return to
view-only when finished. VNC has no password and must remain restricted to
loopback, reached through authenticated SSH tunnels. Never expose it to the LAN.
The services in the inventory table run on the workstation. Inspect/recover the
existing service before starting a duplicate tunnel or taking its port.

Each tile's VNC server runs in the host's desktop session as user `patrick`
(original hosts inspected on 2026-10-19, Aurora added on 2026-10-29; the units
live on the hosts):

| Host | Server | Unit | Listens |
| --- | --- | --- | --- |
| f44 GNOME Wayland | `w0vncserver` from Fedora's `tigervnc-wayland-server` 1.16.2 | user `putgd-gnome-vnc.service` (`WantedBy=graphical-session.target`) | `127.0.0.1:5901` |
| d69 Plasma Wayland | `/usr/local/bin/w0vncserver`, TigerVNC `v1.16.2` (`b555312d`) built from source in `~/work/bootstrap` (CMake Release, `ENABLE_WAYLAND=ON`, no viewer) | user `w0vncserver.service` (Wayland + KDE only, `WantedBy=plasma-workspace-wayland.target`) plus the `localhost.conf` drop-in that binds it to loopback; `wall-vnc-watchdog.service` (since 2026-10-20, #164) restarts it when its stream stops | `127.0.0.1:5900` |
| k72 Plasma X11 | `X0tigervnc` (Ubuntu's `tigervnc-scraping-server` 1.15) on the X display | user `patrick-vnc.service`, started by the `patrick-vnc` autostart entry only in an X11 session | `127.0.0.1:5900` |
| m74 Cinnamon X11 (added 2026-10-20) | `X0tigervnc` (Mint's `tigervnc-scraping-server`) on the physical `:0` display | user `putgd-native-vnc.service` | `127.0.0.1:5900` |
| k72 Plasma Wayland (added 2026-10-20, #87; default session) | `/usr/local/bin/w0vncserver`, TigerVNC `v1.16.2` (`b555312d`) built in `~/work/bootstrap-w0vnc` like d69's | user `w0vncserver.service` (Wayland + KDE only, `WantedBy=plasma-workspace-wayland.target`), run under `systemd-inhibit --what=idle:sleep --mode=block` by the `10-inhibit-sleep.conf` drop-in; `wall-vnc-watchdog.service` restarts it when its stream stops | `127.0.0.1:5900` |
| aurora-amd Plasma Wayland | `/usr/local/bin/w0vncserver`, from Fedora 45 TigerVNC 1.16.2 | user `app-org.tigervnc.w0vncserver.service` under an idle/sleep inhibitor; `aurora-wall-watchdog.service` restarts it if the stream stalls | `127.0.0.1:5900` |

Every lab desktop must stay awake: the wall VNC unit (or a sibling) holds a
`systemd-inhibit --what=idle:sleep --mode=block` inhibitor. On 2026-10-20 k72's new
Plasma Wayland session had none, idle-suspended after about 15 minutes, and
dropped off the network ("No route to host"). Keep `putgd` out of the inhibitor's
`--who` label, because `desktop_lock.py` flags any process matching `putgd` as a
leftover. That suspend never resumed: k72 came back only after a power cycle at
02:08Z.

A Wayland tile can also freeze while its server keeps running, so the wall shows
the same frame, with a stuck panel clock, until someone notices.
`w0vncserver` 1.16.2 reads KWin's PipeWire screencast stream and doesn't notice
when that stream stops: a disconnect is only logged at debug level, the portal's
`Closed` signal isn't handled and frames flagged corrupted are dropped silently.
On k72 on 2026-10-20 the i915 GPU hung in `kwin_wayland` at 02:16:07Z
(`GPU HANG: ecode 9:1:85dffffb`, "Resetting rcs0 for preemption time out").
KWin reset its GL context and the stream feeding the tile stopped. The Skylake
HD 530 had hung the same way three times under Xorg (2026-10-17 07:35Z and
08:13Z, 2026-10-19 02:32Z), so expect it again; the hang is #89. Check for it with
`journalctl -b -k | grep 'GPU HANG'` and
`journalctl -b | grep 'graphics reset attributable'`.
To tell a frozen tile from a quiet desktop, compare two RFB grabs a minute
apart: the panel clock changes every minute.

k72 and d69 run `tools/lab/wall_vnc_watchdog.py` beside their servers as the user unit
`tools/lab/wall-vnc-watchdog.service` (script copied to
`~/.local/bin/wall-vnc-watchdog`; Wayland + KDE only and
`WantedBy=plasma-workspace-wayland.target`, like `w0vncserver.service`). It
holds one shared, view-only RFB connection to `127.0.0.1:5900` and asks for an
incremental update at most every 10 seconds. It restarts `w0vncserver.service`
when no update has arrived for 5 minutes, or within about 10 seconds of the
journal reporting a GPU hang or KWin GL reset. Restarts are rate-limited to one
per 5 minutes, doubling up to 30 minutes while frames don't come back.
`systemctl --user restart` keeps the `10-inhibit-sleep.conf` wrapper, so the
inhibitor survives. The new server opens a new portal session from the restore
token, so no prompt appears. Its log is `journalctl --user -u wall-vnc-watchdog`.
A full-screen static window that hides the clock for more than 5 minutes causes
one harmless restart. Both triggers were tested on k72 on 2026-10-20: a labelled
synthetic `GPU HANG: ` journal line, and a `SIGSTOP`ped server. Each restart
brought live frames back. On d69 a labelled synthetic KWin reset line restarted
the server after the unit's 5-minute start grace, and frames came back
([receipt](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-20-recording-closeout.md#164-wall-vnc-watchdog-on-d69)).
d69's `patrick` user isn't in `adm`, so the kernel's `GPU HANG` line is
invisible to the unit there. KWin's own "graphics reset attributable" line is in
the user journal, and the unit matches it. On k72 the user is in `adm` and
the unit sees both lines.
A reset that the new stream survives (updates still arriving 30 s after the
line) no longer forces a second restart. On k72 at 06:05Z a second hang, logged
3 s after a restart, did that at 06:10:30Z.
Checking the watchdog and the HDMI-A-2 mirror after a k72 re-login and a power
cycle is still #164, pending Patrick's OK and k72's return from its hardware
check.

All use `-SecurityTypes=None`, so they must stay on loopback. The servers keep
their portal restore token under `~/.local/state/tigervnc`
(`-RememberDisplayChoice=Always`); the first start in a new session asks for the
desktop's screen-sharing consent once. On k72 the two servers share port 5900,
one per session type, so the wall tile works in either login. Since 2026-10-20
k72's default session is Plasma Wayland (`Session=plasma.desktop` in SDDM's
autologin, `/etc/sddm.conf`). Switch it to `plasmax11.desktop` only when a check
needs KDE/X11, then switch it back. Each switch restarts `sddm` and ends the
desktop session, so check first that no other session holds work on k72.

k72 has two outputs. HDMI-A-1 is forced on with the headless EDID
(`drm.edid_firmware=HDMI-A-1:edid/patrick-headless.bin`). Since the 2026-10-20
power cycle, Patrick's own monitor is also on HDMI-A-2, a hotplug-detected
output with the same EDID. Extended, the two outputs made a 3342x1080 workspace,
and the tile streamed both of them. At Patrick's direction, HDMI-A-2 now mirrors
HDMI-A-1 (`kscreen-doctor output.HDMI-A-2.mirror.HDMI-A-1`), so the tile is one
1920x1080 desktop. KWin stores the mirror in `~/.config/kwinoutputconfig.json`
as the two-output setup's `replicationSource`, so it applies whenever both
outputs are connected. It hasn't been tested across a power cycle yet. The
previous file is at `~/putgd-lab/k72wall/kwinoutputconfig.json.before-mirror`.
Because Patrick's screen shows the same desktop, a capture may leave k72 only
after you've checked on the host that your own test window fills the captured
area. Check frame liveness by comparing hashes on k72; don't copy frames off the
host.

Every lab desktop carries the host user's own windows, so a recording or frame
may leave a builder only once the lab's own window is shown to fill it. The
tools for that are `tools/lab/cover_player.py` and `tools/lab/record_run.py`
(#165):

- `cover_player.py play --output NAME [--pattern | --media MARKER.mkv]` opens a
  magenta window full screen on that output, with a scrolling pattern or the
  looped A/V marker at 1280x720 in its middle. It prints `covered` only after
  GTK sees the window full screen on that output at its size. Without
  `--output` it refuses on a desktop with more than one distinct output
  (mirrored outputs count once). `cover_player.py outputs` lists them. Once a
  second the player checks whether the process that started it is still
  running, and closes when it has exited, so a driver that dies can't leave
  the desktop covered. `--detached` turns that check off; whoever starts a
  detached player must stop it by its PID.
- `cover_player.py precheck --port 5900` (5901 on f44) reads one frame from the
  host's loopback VNC tile, in memory, and fails unless the output is at least
  98% cover colour. Run it before recording anything.
- `cover_player.py check-frame FRAME [--crop OUT]` deletes a saved frame unopened
  when it is under half cover colour, and `--crop` keeps only the covered
  output.
- `fluidity.py fixture --output NAME --ticks FILE` shows a moving picture full
  screen whose every frame carries its frame number in a machine-readable
  strip, at the display's frame clock; `fluidity.py analyze CLIP --fps N`
  reads those numbers back out of a recording (distinct source frames per
  second, repeat/skip pairs, longest stall) and `fluidity.py ticks FILE` reports
  the fixture's own frame intervals. `fluidity_run.py record|select` runs both
  per build on the host, with private directories, `--runtime-dir` beside a
  running resident, `--kwin-entry` for a build tree's screencast entry and
  `--real-data-home` for AppImages; `select` drags through the uinput agent
  (`--input-socket`) or xdotool and must release the button it pressed (#6).
  `record --env KEY=VALUE` is repeatable and sets the resident/helper environment:
  for example `--env LIBVA_DRIVER_NAME=none` forces software x264, and
  `--env PUTGD_RECORD_ADAPTIVE_RATE=off` disables adaptive rate for A/B runs.
  Keep secrets out of these arguments. Each recording result in `results.json`
  includes `machineBusy`, the machine-wide CPU busy share (0–1) sampled from
  `/proc/stat` across that recording, alongside the recorder rate-window logs.
- `fake_outputs.py add DP-1 DP-3` forces spare DRM connectors on with the
  booted monitor's EDID (runtime `drm.edid_firmware` plus a writable firmware
  path, no reboot), so a builder with one monitor gets a three-output desktop
  for selector and recording checks; `remove` restores them. Run it under the
  desktop's lock and remove the outputs before releasing it.
- `record_run.py` runs a whole check on the host: cover, precheck, each build's
  resident with private config/data/cache/state, `putgd-cli` recordings
  (monitor, region or window; any format; optional pause), `--motion`,
  `--av-sync` split at the pause, one gated frame per copy (`--frames`), and
  the kernel's GPU hangs for the run. A region must straddle the video's edge
  (for example `--rect 0,0,960,540`) to pass the frame gate. `--vnc-port 0`
  skips the precheck, in disposable guests only. See the script's docstring for
  the options.
- `virtual_mic.py` gives a host or guest a simulated microphone, because a real
  one is never recorded in the lab: `create` loads a null sink and remaps its
  monitor into a real input (`module-null-sink`, `module-remap-source`; a bare
  monitor would not do, put.gd never offers one), `tone` plays into it,
  `unplug` unloads the input while a recording runs, and `remove` unloads
  every module it loaded by its exact id and restores the previous default
  source and sink by name. `--system-sink` adds a private default output so
  system audio can be checked without sound from the speakers. It keeps a state
  file, so `remove` after a failed run still cleans up. `create` checks the
  microphone before it hands it over (a tone into the sink must come out of the
  source, read with `parec`; a silent source is made again, up to six times) and
  loads a loopback into a third, silent sink that keeps the source in use for
  the run: on PipeWire 1.0.5 (Mint 22, Ubuntu 24.04) a remapped source left
  without a reader sometimes passes pure silence to every later reader. `unplug`
  and `remove` unload that loopback before the source, so it can never be moved
  to a real input. `create --unplugged` makes the microphone's sink alone and
  `plug [--default]` loads the source later, the way a microphone is plugged in
  (`--default`: it becomes the default input). `unplug-output [--fallback]`
  removes the private output system audio is recorded from: with `--fallback`
  another private output is made the default first; without it the command
  refuses while a real output exists, because what plays into the removed one
  would be moved there and heard.
- `record_run.py --mic virtual|muted|default|NAME` records with a microphone:
  `virtual` creates the simulated one, plays the `--av-marker` clip's bursts
  into it and removes it afterwards; `muted` does the same with the bar's
  toggle off (the clip must hold none of it); `default` asks for the system
  default, which on the builders is a monitor and must be refused.
  `--system on|off|tone` adds system audio (`tone`: a quiet 440 Hz on the
  private output), `--unplug-at S` removes the microphone S seconds in, and
  `--marker-motion` keeps a bar moving so the motion gate has something to
  see. `--av-sync` then measures the microphone's bursts against the flashes
  (`putgd-record --av-sync CLIP --tone 1000 --tone 440` also reports each
  tone's level). Use `--lead 6`: the first pairs after Record can sit a frame
  or two off while the audio branch settles. `--system leak` plays the tone on
  a private output whose monitor is the default input, without recording
  system audio: the clip must hold none of it, also after `--unplug-at`.
  A round lists `problems`, and the run exits 1, when its clip is more than
  1.5 s shorter than the time recorded, lacks the simulated microphone's bursts,
  holds them with the toggle off, holds the unrecorded output's tone, says the
  microphone was lost when nothing removed it, or does not say so after
  `--unplug-at`. `summarize_run.py RESULTS.json` prints a run on one screen.
  `--mic late` prepares with no microphone to open and, while the bar is at
  Ready, plugs the simulated one in as the default input, switches it on with
  `recording.audio`, pulls it and plugs it in again: `recording.inspect` must
  follow each step within eight seconds (`readyFollow` in the results, a frame
  of the desktop per step with `--frames`), and the clip recorded afterwards
  must hold the bursts. `--output-gone-at S` (with `--system tone`) removes the
  recorded output S seconds in: with `--output-fallback` another private output
  takes over; without it nothing is left, which on a machine with a sound card
  needs `--cards-off` (every card's profile off for the run, written to
  `RUN/cards.json` first and put back with the defaults afterwards, so no lab
  sound can reach a real output). `--mic-default` makes the simulated microphone
  the default input for the run, to show that a moved system-audio stream never
  lands on it. The recording must go on and hold the whole time recorded;
  `afterOutputGone.recordedFrom` names the source the recorder reads afterwards.
  `--delayed-capture desktop:3` (or `monitor`, and 5 or 10) checks a delayed
  capture and a recording in one resident session: `action.invoke` with
  `delaySeconds` before `recording.prepare` and again after the recording is
  cancelled. Each capture must end `completed` no sooner than its delay, saved
  to the run's private archive and at least half cover colour
  (`delayedCaptures` in the results); with `--frames` a frame of each countdown
  is kept and `countdownPixels` counts what the pill changed at the top centre.
  A run with `--unplug-at` and more than one recording (formats, rounds,
  builds) plugs the simulated microphone in again before each later one. A run
  with `--output-gone-at` is one recording (`--rounds 1`, one format, one
  build): the private output is not made again, so anything more is refused
  before the run starts.
  `--hide-bar` hides put.gd's recording bar at Ready (`recording.overlay
  {visible: false}`). A whole-monitor recording on a one-monitor desktop has
  the bar's Stop handle in the clip (`overlay.overlayPlacement` `handle` in the
  round's entry), and with `--frames` the frame gate can take it for something
  that is not the cover and delete the clips; a clip deleted that way is listed
  under `problems`, with this option named.
  On Hyprland a `--delayed-capture` run holds `misc:on_focus_under_fullscreen`
  at 0 as a window run does: the review window opens before the second
  capture and would otherwise take the cover's full-screen state.
- `record_run.py --click S,X,Y[,BUTTON]` (repeatable) presses a button at a
  desktop pixel S seconds into each recording, as real input through the
  desktop's VNC tile (`--click-vnc HOST:PORT` in a guest: QEMU's VNC on the VM
  host is `10.0.2.2:59NN` from inside), with `clickHighlights` on. The clip is
  then measured with `putgd-record --click-check CLIP FROM_MS,TO_MS,X,Y`, which
  reports what changes near a place between two times against the first frame
  of that span: first the pointer arriving there, then the ring appearing over
  the resting pointer. The ring's delay after the press is counted from the
  pointer's arrival in the same clip, so the recording's own start does not
  enter. Add `--system tone` on a builder: the marker's sound then plays into a
  private output instead of the speakers. A window run (`--target window`)
  records the cover: on Wayland the focused window; on X11 the cover's window
  found by its process id (a cover started from a shell does not get the focus
  on Cinnamon); on Hyprland with `misc:on_focus_under_fullscreen` held at 0 for
  the run and put back afterwards, because at 1 (Omarchy's value) put.gd's
  controls take the cover's full-screen state when they open, the cover is
  tiled out of sight and the presses land outside the recorded window.
- `recording_bar_shots.py` saves frames of the desktop with the recording bar's
  microphone states on screen (toggle off; on, with a level on the meter; no
  microphone) and, with `settings`, Settings, or with `support`, Settings with
  Desktop support details open (through `ui.diagnostics`), for PR pictures.

Two traps when you drive a private resident in a desktop session (#214, #198):

- A private config without a settings revision makes the resident open
  Settings by itself at start, `--background` or not. Take any baseline frame
  before the resident starts (or give the config a revision first, with one
  `settings.patch`, and restart it); a baseline taken after start already holds
  the window, and a step that leaves its colours unchanged then looks missing.
- A private `XDG_RUNTIME_DIR` hides the session's PipeWire socket, so
  `putgd-record` falls back to the portal and its picker on Plasma. Export
  `PIPEWIRE_RUNTIME_DIR=/run/user/$(id -u)` next to it. A lab build of
  `putgd-record` also needs an application entry with
  `X-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1,org_kde_kwin_fake_input` (the second
  is what a scrolling capture's wheel needs through Plasma 6.7) and its `Exec` path
  (remove it afterwards and run `kbuildsycoca6`). On f44 the full-screen cover
  stayed behind the session's windows (0% cover); crop strictly inside your own
  window there and check every crop edge is its background token.

Keep graphical tests visible there throughout the run. Capture screenshots or
short recordings of the initial desktop, decisive actions, results, failures and
completion. Save the visual files with the host, session/compositor/version, exact
source commit/tree, installed and running build identities, scenario parameters
and pass/fail result. Preserve failed attempts. Exclude revealed credentials and
protected login values from screenshots, recordings, logs and review material.
Follow [evidence retention](../testing/evidence-retention.md).

A host tile does not automatically display its VMs. Before remote VM acceptance,
bind the guest's VNC listener to remote loopback, allocate a distinct free
workstation port and forward it over pinned SSH. Register a separate guest tile
in the existing wall configuration, preserving all host tiles. The current wall
reads its `QEMU_GRID_REMOTES` JSON file, or
`tools/qemu-grid/remote-displays.json` by default: entries have exactly `name` and
`port`, with the port naming a workstation-loopback tunnel. Verify the live guest
visually; a listed tile or an RFB handshake alone does not establish visibility.

SSH builds/logs and offscreen tests do not establish graphical acceptance. Test
the actual GNOME or Plasma session for physical-host claims. Required release
profiles retain their exact OS/desktop versions; Fedora GNOME or Debian Plasma
does not waive a different version in that matrix. For VM scenarios,
retain the existing independent QMP, saved-artifact and native-paste witnesses.

## Sharing the lab between concurrent sessions

Several agent sessions, Claude or Codex, often use the lab at the same time.
These rules apply to every session, whether or not it has a coordinator. They
come from 2026-10-19, when the f44 desktop sat idle for about 50 minutes: its
lock holder was waiting for another session's go-ahead, and that session was
waiting for the lock, over a probe process left behind by a finished test.

### Shared and exclusive resources

- **The physical desktops are exclusive.** f44 (Fedora 44 GNOME Wayland, `.70`),
  d69 (Debian 13 Plasma Wayland, `.69`), k72 (Kubuntu 26.04 Plasma Wayland, `.72`),
  m74 (Linux Mint 22.3 Cinnamon X11, `.74`) and aurora-amd (Aurora Fedora 45
  Plasma Wayland, `.71`) each serve one session at a time, under a lock from
  `tools/lab/desktop_lock.py`.
  You need the lock for anything that runs in the desktop session, draws on it,
  sends it input, captures or records it, or logs it out. That includes installing
  a package, extension or desktop entry the session loads.
- **SSH builds, tests and VMs on the builders are shared.** Run builds with
  `nice -n 10` and a bounded `-j`. Check `uptime` before you start another heavy
  job on a host whose load is already above its core count; k72 and m74 have four cores.
- **Timing runs hold the host's quiet lease.** Recording, capture-latency,
  memory and other timing runs take `desktop_lock.py acquire HOST --resource
  quiet` when they are ready to start, and release it the moment the timing
  ends. While another session holds it, don't start new heavy SSH jobs on that
  host (builds, full ctest, replays, benchmarks). The guarded lab helpers refuse
  to; for anything else, use `desktop_lock.py guard`. Jobs already running may finish. See [Quiet leases for timing
  runs](#quiet-leases-for-timing-runs).
- **Don't change the desktop under test.** Never install, upgrade or downgrade
  compositor or desktop packages on these hosts: mutter, gnome-shell, kwin,
  plasma, xorg, mesa, GPU drivers, pipewire or xdg-desktop-portal*. Read every
  dnf or apt transaction before you confirm it, so a tool install doesn't pull
  them in. Use a nested or containerised setup instead.
- **The workstation is Patrick's.** No builds, full ctest, packaging,
  VMs or encoding runs there. Git, editing, reading and a single
  short test are fine.

The automatic nightly desktop check runs from d69's timer at 12:30 UTC on all
five builders, using the holder name `nightly-desktops`, and posts to pinned
[issue #1003](https://github.com/patrick-hudson/put.gd-desktop/issues/1003).
Release manual desktop leases by 12:15 UTC so the check can acquire them.

#### What the nightly check runs

Each row boots a fresh guest of its golden, installs the published package and runs
these cells, each judged by an independent check, not the app's own report. The one exception is the Tray menu cell
on trays without AT-SPI, where the rows come from put.gd's record of what it served the tray host and the screen is
read only to confirm them. Before it
installs anything, the row holds the apt guests' own package updaters (`apt-daily`,
`apt-daily-upgrade` and Mint Update's automation) in the overlay, as root through the
goldens' passwordless `apt-get` (its Pre-Invoke hook, with no sources, no package cache
and no lists lock). It stops and masks their timers, then waits up to 20 minutes for the
updater services to drain: a running or queued updater is left to finish, because
stopping one could kill dpkg or leave `unattended-upgrade` running. It then masks each
loaded service only if it is `inactive` or `failed` with no queued job and a cgroup
that is absent or reports `populated 0`, and records every unit in `package-updates.json`. The row goes on only if the guest reports every listed unit as
absent (`not-found`) or `masked`, as `inactive` or `failed`, and with no queued job and a cgroup that is absent or reports `populated 0`; any other state, or no report for a unit, stops it there. Fedora installs no updates by
itself and Aurora's `uupd` is held by its tools step. Ubuntu's upgrade timer fires between 06:00 and 07:00 guest time, the
hour the check runs: on 2026-11-20 it upgraded `openssh-server` mid-row in the Kubuntu
guest and sshd's restart cost the row its guest.

| Cell | Green when |
| --- | --- |
| Idle memory | The idle resident is under the 40 MiB budget and maps no forbidden library |
| Region, Auto-copy | Region pixels equal the frozen pattern; the link is on the clipboard with no copy click |
| Window / desktop | Hotkey window and desktop captures equal the pattern |
| Recording → upload | A moving clip beats a frozen one, then uploads |
| Edit → upload | Four mouse marks land on the drawn image coordinates |
| Link → browser | Firefox shows the uploaded image from History's copied link |
| History delete | The deletion from History completes and the object then answers 404 |
| Upload clipboard | Image pixels, file bytes and text bytes from the clipboard upload unchanged |
| Copy image | History's Copy image gives the original pixels |
| Survives exit | The auto-copied link is still on the clipboard after Quit and a restart |
| Right-click upload | The file manager's upload entry uploads exactly the selected file |
| Window recording | A recording of the moving pattern window, by its identity (`recording.windows`, or its X id on X11); the resident must freeze exactly that identity. The exported clip lasts 4 to 30 s, its frames differ and it is the window's size (one pixel less allowed for encoder alignment) |
| Tray menu | A real right-click on put.gd's tray icon opens its menu; Capture region from it uploads at the dragged size and its link is auto-copied. Where the tray exports the icon to AT-SPI (GNOME with the AppIndicator extension, Plasma), the menu's named rows there must be the agreed rows (#1532) in order, with no other named row. Where it doesn't (Cinnamon, Budgie, Omarchy), the icon is found by its pixels (below). put.gd's own `diagnostics.trayMenu` must then show the host asking for the menu after the click and list exactly the agreed rows in order as the visible rows it serves (red otherwise); tesseract must read each agreed label in order in the popup that opened next to the icon, and no other text there (a shortcut on a line of its own, or a mark of fewer than three letters or digits, is not counted); the click lands on the drawn Capture region row and put.gd must record that row as the one the host reported clicked |
| Copy text / redact | Copy text of a drawn two-line pattern puts those two lines on the clipboard (compared with whitespace collapsed and blank lines ignored); Quick redact (`name`, `list`) turns the login name and a listed word solid in the saved copy, and two decoy words stay unchanged |
| Live S3 (live rows) | A public upload, its CloudFront link in Firefox and its deletion; then the destination switched to expiring links (#1366): the link is signed for at most a day and records its expiry, the signed GET returns the file, the same URL unsigned gets 403, and the deletion leaves the origin without the object |
| Live R2 (live rows) | Three uploads to a Cloudflare R2 destination with the bucket-scoped key: public links on `https://cf.ul.gy` and on the bucket's r2.dev address, then a presigned link (signed for at most a day, refused unsigned). Each GET returns the file; each delete from History leaves the origin without the object. A public GET after a delete is recorded, not judged, as either host may serve a cached copy |
| Live Proton (live rows) | An upload; the recipient's download matches the saved bytes; after revoking, the link no longer resolves. A row with `"proton": false` leaves it out (—) until a Proton lend is proven on its golden |

➖ marks a feature the [acceptance matrix](../testing/acceptance-matrix.md) says that
desktop does not support (`UNSUPPORTED` in `nightly_desktops.py`, with the matrix's
words; empty today); it is never red and opens no issue. Use it only for something no user
of that desktop can do. What a user can set up is set up in the guest instead: stock Fedora
GNOME has no tray host, so the Fedora GNOME row installs Fedora's AppIndicator extension
(`gnome-shell-extension-appindicator`) before the witness reboot (the transaction is in
`tray-host.json`), enables it after the reboot, as a Fedora user does for put.gd's tray, and
waits for a registered StatusNotifier host before put.gd starts (`tray-host-ready.json`).
⚪ means not run or no valid result: setup or infrastructure failures, checks that could not
decide, and cells the harness cannot drive on a desktop, each with the reason.

On trays without AT-SPI the Tray menu cell finds put.gd's icon by its pixels. The template is
what the running resident publishes at that moment: its `IconPixmap` frames, which must read
the same twice a moment apart with no upload or completion showing, plus its `IconName` PNG
on hosts that read names, taken with the second read. The cell compares the glyph's shape with each drawn shape on the
profile's panel strip, at the size it was drawn, so the season's glyph, the tint and a host's
recolouring don't matter. No shape scoring at least 0.8, or a second shape within 0.12 of the
best, is ⚪ with the scores, and so is a second shape at 0.8 or above however far behind. A popup
that can't be placed or doesn't settle, or that shows text that is no agreed row, is ⚪ too;
those checks come before the served rows are compared, so a wrong served menu is red only once
the popup was placed, settled and showed nothing else. A popup missing an agreed label is ⚪,
checked after the served rows. Omarchy keeps tray items in a drawer that opens on hover: the
cell adds put.gd's item to the tray's `pinned` list in `~/.config/omarchy/shell.json` (what the
tray's Pin does) for its duration. It registers the cleanup before writing, and the cleanup
writes the earlier value back and reads it back from disk; if that fails, the run's cleanup
fails. Omarchy's menu card shows about twelve rows, so the cell scrolls it
to read the rest, scrolls back and reads the top again before clicking Capture region. Scrolled
down, the card cuts the row at its top edge; that one line is set aside, and noted, only when the
line below it is an agreed row the first view also read. The pointer is moved off the panel
before the icon is matched and kept off the rows while the menu is read. Cinnamon's host
sometimes asks put.gd for the menu and draws none, then takes the next click as closing it, so
the cell right-clicks up to three times, each only while no popup shows.

A builder runs one or two rows, one after the other, sharing its one guest slot and
ports: m74 runs Mint Cinnamon (DEB) then Hyprland on Omarchy (AppImage); d69 runs
Ubuntu GNOME (DEB) then Fedora 45 Budgie (RPM); k72 Kubuntu (DEB), aurora-amd Aurora
(AppImage) and f44 Fedora GNOME (RPM) run one each. All seven rows run Live S3, Live R2 and
Live Proton once d69's and m74's configs set `"live": true` and `"proton": true` for the Budgie
and Hyprland rows.

labwc reports no window geometry, so the Budgie row places windows by their own pixels,
and three labwc 0.20 behaviours need care:

- **Dialogs.** labwc captures a modal dialog on its own as 1×1, and draws it into its
  parent's capture at that capture's origin. The Budgie guest gets labwc window rules
  for the dialogs the cells click (`window_rules` in the `fedora-budgie-wayland`
  profile: Delete from destination and Revoke share link). Each rule moves the dialog to a
  fixed position with no server-side decoration, so its content starts exactly there.
  The driver takes the dialog's own pixels from the top-left corner of the captures of
  its process's other windows. Exactly one such corner must match the outputs at the
  rule's rectangle, both across all sampled cells and across the cells with at least four
  colours.
- **The labwc bridge.** Budgie's labwc bridge reads `rc.xml` once, when it starts, and
  writes its own copy back on every change. put.gd's shortcuts reach it as gsettings,
  so rules written while it runs are lost. The guest setup stops the bridge, writes
  the rules, starts it again from its own environment, and checks that the bridge's first
  write still holds them (`labwc-rules.json` in the row's evidence). budgie-session
  does not restart the bridge on its own.
- **Stale captures.** grim's capture of the outputs can show a region's earlier pixels
  after a window redrew it, while the display shows the new ones. A window's own
  capture can lag the same way. This happened with History's details pane after its
  link row laid out, and it left History clicks unverified. A pointer hit-test on a
  toplevel (not a popup) of a single-output guest therefore compares the window's own
  capture with the guest's display, read over VNC, inside the target's rectangle. A
  target that reaches past the display, or a crop too large for the probe request,
  keeps grim's capture of the outputs. Within one check the window is captured up to
  three times against that one display read. The display is read again for each later
  check: after a screen clearing, and for up to two retries of an unverified result. When a
  check starts by activating the target, it reports unverified, so the next check reads the
  display again.

Firefox's blank kiosk page has no pixels to place it by. A window with no distinctive pixels
is placed only when it is the active toplevel and its capture matches exactly one output
of its size. On labwc, capturing History puts its own details pane back at the top, and the
driver captures History to place it. That keeps the pane's Delete from destination… button
below the window. When the button isn't in view, the S3 and R2 cleanup deletes through the
exact row's own menu instead, the same path the History delete cell uses, and goes through
the same confirmation. It then clears the History search for the next pass. When an
accessibility walk runs past its time budget, the driver makes up to three attempts at the
geometry read. On
Budgie, History reads skip any application whose executable isn't put.gd's without
contacting it, because the lab's Firefox stops answering accessibility calls after showing
a CloudFront page. Live cleanup wheels History's pane below its preview: a pointer resting
on the preview shows a tooltip over the point.

On Omarchy, quickshell's clipboard plugin (`omarchy.clipboard`) keeps a clipboard
history on disk: `~/.local/state/omarchy/clipboard-history.json`, copied images under
`clipboard-images/`, and a temporary copy of each text. Omarchy can record links
put.gd copies automatically, signed ones included. The Hyprland guest turns the plugin off with
`omarchy-plugin-disable` before the cells run, and records its check in
`omarchy-clipboard.json`. Each live cell repeats that check after confirming the desktop lease and
the guest slot. The plugin must be disabled in `shell.json`, no `capture.sh` watcher may
run, and a random canary copied to the clipboard must change none of the known clipboard
files. Those files are checked both in the shell's default state folder and in its
`XDG_STATE_HOME` when set. The guest also gets `qtkeychain-qt6` as a new package,
because the lab's credential bridge links it. After a successful request to dismiss
quickshell's notification cards, screen clearing polls until quickshell reports none. It stops
earlier if the count can't be read or the deadline passes. Hyprland ends the kiosk's fullscreen when History opens over it for a revoke, so
the revoked-link check asks Firefox for fullscreen back before it navigates.

Each builder reads its own
`~/.config/putgd-nightly/config.json`, so a row added to a builder goes in that
builder's copy and in d69's. The cells added on 2026-11-20 add about nine minutes to a
live row (Live R2 alone takes about five) and three to a local-only one; a builder with
two rows takes about an hour.

### Desktop locks

Each builder keeps its own lock in `~/putgd-lab/locks/desktop.lock`, a file of
`key: value` lines (`owner`, `session`, `host`, `purpose`, `acquired`,
`expires`, `heartbeat` and, when set, `blocked`) with UTC times. Locks live on the
builders, not in a workstation folder, for these reasons:

- Every session that can use a desktop already reaches its builder over pinned
  SSH, whatever its app, worktree or session family. A workstation folder only
  works for sessions that can see and write it. App notes folders belong to one
  session family and disappear with it, worktrees don't share `work/`, and a
  sandboxed Codex session may not write outside its checkout.
- The builder's own clock decides expiry and staleness, so clock skew between
  machines doesn't matter.
- The cleanup check has to run on the builder anyway, and anyone at the host can
  read the lock with `cat`.
- The builder's local filesystem gives a real atomic acquire. The tool serialises
  every change with `flock` and creates the lock with link(2), which never
  replaces an existing file.

The cost is that a lock needs its host to be reachable, but an unreachable host's
desktop can't be used anyway. The tool sends itself to the builder over the
pinned SSH options above (`ssh ... python3 -`), using the `known_hosts` pins in
`work/remote-builders/<host>/`. It refuses to run without a pin.

```sh
python3 tools/lab/desktop_lock.py status [HOST ...] [--check]
python3 tools/lab/desktop_lock.py acquire f44 --owner NAME --session ID --purpose TEXT --minutes 30 [--no-tray-host REASON]
python3 tools/lab/desktop_lock.py heartbeat f44 --session ID [--blocked REASON]
python3 tools/lab/desktop_lock.py extend f44 --session ID --minutes 30
python3 tools/lab/desktop_lock.py release f44 --session ID [--verified-clean] [--not-mine ID ...] [--removed-ok REASON]
python3 tools/lab/desktop_lock.py break f44 --session ID --reason TEXT
python3 tools/lab/desktop_lock.py held [HOST] [--session ID]
python3 tools/lab/desktop_lock.py run f44 --owner NAME --session ID --purpose TEXT --minutes 30 [--resource quiet] [--release] -- CMD ...
```

Use `run` for work that needs a lock: desktop work (the default `desktop` lock)
and timing runs (`--resource quiet`). Shared SSH builds, tests and VMs need no
lock. `run` is the safe form of "acquire, then work": it takes the lock, or
confirms that your session already holds it, and only then runs CMD on the
builder over the same pinned BatchMode SSH, with `PUTGD_LAB_SESSION` set to your
session. CMD's words reach the builder as you gave them; put a shell expression
in `sh -c '...'`. If the lock isn't yours it exits 1 without running CMD. While
CMD runs it renews the lock every 5 minutes, or every third of the lease if
that is shorter. A refused renewal stops CMD's SSH and exits 1, and so does one
that can't reach the host when the lease would lapse before the next try;
anything CMD started on the host may still be running. Otherwise it exits with CMD's exit code and
keeps the lock, so you can run the next step the same way.
`--release` gives the lock back after CMD as `release --verified-clean` would,
and is still refused if something new is left on the host.

Don't write `acquire ...; CMD` or acquire in one step and work in another. On
2026-11-22 both forms ran lab work on builders that other sessions held: a
refused acquire followed by `;` installed a different candidate deb over the
holder's on k72.

`held` exits 0 only while your session (`--session`, default
`$PUTGD_LAB_SESSION`) holds a current lock, neither expired nor stale. Without
HOST it checks the builder's own lock directory. Scripts check it before each
step. The lab helpers that install packages or drive a builder's desktop
(`physical.py`, `record_run.py`, `fluidity_run.py`, `recording_bar_shots.py`)
refuse to start unless your session holds that builder's desktop lock. Under
`run` this is automatic. `physical.py` runs on the workstation and checks
through its own SSH transport, so give it `lock_session` in its config or set
`PUTGD_LAB_SESSION`. A host with no lab lock directory, such as a VM guest, is
not checked.

- `--session` is how other sessions reach you. Use your Claude Code `local_...`
  id; a Codex or terminal session uses a unique name such as `codex-<branch>`
  and says in `--purpose` where its notes are.
- Acquire before you start anything on the desktop, not while you prepare, and
  chain the work to it with `run`. If
  you have a coordinator, wait until its schedule lists you next. `--minutes` is
  at most 45. Run `extend` before the lock expires; it moves the expiry to N
  minutes from now.
- Heartbeat at least every 10 minutes while you hold the lock. A lock is
  **stale** once its heartbeat is more than 15 minutes old, and **expired** once
  `expires` has passed. If something else blocks you, run
  `heartbeat --blocked REASON` and tell your coordinator, or release the lock.
  The next heartbeat clears the reason.
- If the desktop is locked, don't poll the holder and don't wait idle. Tell your
  coordinator, do other work and try again later. Between session families, the
  lock goes to whoever takes it first.
- On a GNOME host (f44), `acquire` also checks the tray host and refuses without it
  ([GNOME rows run with a tray host](#gnome-rows-run-with-a-tray-host)).
- `acquire` lists what's already on the host, such as an earlier holder's
  processes. Those aren't yours: leave them and tell your coordinator. `status`
  shows who released or broke the lock last.

### Quiet leases for timing runs

The `desktop` lock means "I'm driving this desktop session". The quiet lease
(`--resource quiet`, `~/putgd-lab/locks/quiet.lock`) means "don't load this
host's CPU". They are independent: a timing run on a desktop holds both, a desktop
acceptance run usually holds only `desktop`, and a headless benchmark over SSH
holds only `quiet`.

There are no clock windows and no private marker files. On 2026-11-22 a booked
"aurora is quiet 02:30-02:50Z" window failed both ways: a build that didn't know
about it started inside it and spoiled a timing pair, and a run whose packages were
ready at 01:53Z slept on an idle host until its window opened. A lease is held only
while the work runs:

```sh
python3 tools/lab/desktop_lock.py acquire aurora-amd --resource quiet --owner NAME --session ID --purpose TEXT --minutes 30
python3 tools/lab/desktop_lock.py heartbeat aurora-amd --resource quiet --session ID
python3 tools/lab/desktop_lock.py release aurora-amd --resource quiet --session ID
python3 tools/lab/desktop_lock.py guard aurora-amd [--session ID]
```

- Take the lease when the timing work is ready to start, never ahead of it, and
  release it the moment the timing ends. Heartbeat, extend, status and break work
  as for the desktop lock. Release needs no `--verified-clean`: there is nothing
  on screen to check.
- Acquire records the host's load averages and CPU count in the lease. When the
  1-minute load is above the CPU count it says so: find the heavy work before you
  time anything, and keep the recorded load with your results.
- When another session holds the lease, do other work and retry later. Don't
  wait idle for it. The tool's refusal says so too.
- These entry points check the lease themselves and exit without starting while
  another session holds it: `rr_loop.py`, `race_loop.py`,
  `scroll_tracker_score.py`, `fluidity_run.py` and `record_run.py` (through
  `desktop_lock.refuse_while_quiet()`), and Aurora's `putgd-build-env` before
  `cmake` and `ctest`. A held, blocked or stale lease stops them, and so does an
  unreadable lease written in the last 15 minutes. A free or expired lease
  doesn't.
- The [weekly stress lane](#weekly-stress-lane-on-k72) holds k72's lease
  itself for its whole run. When the host is idle, it acquires the lease
  (owner `putgd-stress-weekly`, session `putgd-stress-weekly-PID`) before it
  fetches. It renews the lease every 5 minutes and releases it when the run
  ends, also after a failed build or a stop. If another session holds the
  lease at the start or takes it while the lane waits for an idle host, the
  lane skips the week and leaves that lease alone. `PUTGD_LAB_SESSION` doesn't
  let it through. If the lease stops being the lane's, or a renewal fails and the
  lease would lapse before the next try, the lane stops its run, children
  included, before it lets the lease go.
- Nothing else checks the lease. This includes `stress_suite.py`, `ctest` or
  `cmake` run directly, and any other heavy command. For those, put
  `desktop_lock.py guard HOST && ` in front of the command. `guard` exits 1 while
  another session holds the lease. Run on a builder without `HOST`, it reads that
  builder's own lock directory.
- Your own lease doesn't stop your own run in the guarded helpers: set
  `PUTGD_LAB_SESSION` to the lease's session (or pass `guard --session ID`).
- An expired lease no longer stops heavy work, but its file stays until someone
  removes it, so no other timing run can acquire the lease. Clear it as in
  [Stale locks](#stale-locks), with `--resource quiet`: message the holder, then
  `break HOST --resource quiet --session ID --reason TEXT`.

### Release only after a full cleanup check

Processes and agents started over SSH aren't part of the desktop session. A GNOME
or Plasma logout doesn't stop them, and neither does ending your agent session.
Before you release:

1. Stop everything you started on the host: residents, any leftover `flatpak ps`
   instances, input agents (`sudo -n systemctl stop putgd-lab-input`), pattern windows and
   recordings, plus the workstation-side SSH forwards. Kill only the exact PIDs
   you started.
2. Remove anything temporary you added: desktop entries (then run
   `kbuildsycoca6 --noincremental`), user extension copies (then log out and in
   again, and check that the packaged extension loaded), packages you added (put back
   the acquire-time version of any package you replaced), and
   `~/.local/share/putgd`, `~/.cache/putgd` or `~/.config/putgd` if your runs
   created them.
3. Run `release HOST --session ID`. On the host it inventories `flatpak ps`
   instances and runs
   `pgrep -u patrick -af` for `putgd`, `host_input`, `videotestsrc`, `cover_player` and lab test
   `Xvfb` servers (`desktop_lock.py` `PROCESS_PATTERN` has the exact pattern), and checks for user
   putgd desktop entries, extension copies and data, and an installed `putgd`
   package. It lists anything that appeared since you acquired the lock and
   doesn't release yet. Once nothing of yours is left, run it again with
   `--verified-clean`; the lock is released only if the check passes. Items that
   were there at acquire are reported but don't block. A matching process counts
   if it runs in the desktop session (`DISPLAY` or `WAYLAND_DISPLAY` is set), or is
   an input agent, test pattern or running `cover_player.py`. When the tool can't read a
   process's environment (a confined `putgd-ocr` is non-dumpable), it reads the first readable
   environment among its current parent, that one's parent, and so on, up to eight levels. The
   process is shared SSH work only when that environment has `SSH_CONNECTION` or `SSH_CLIENT`
   and no display variable: another session's SSH benchmark helper doesn't block, while the
   editor's helper does (#1938). The check sees only the current parents, so a helper adopted by
   a subreaper is judged by that subreaper; put.gd configures its helper to receive `SIGKILL`
   when the thread that launched it exits. It also counts when no
   readable environment turns up before PID 1 or within eight levels, or when a parent can't be
   looked up or exits during the check. An `Xvfb` whose auth file is `/tmp/pg*/xvfb-run.*/Xauthority`
   (xvfb-run inside a lab tool's private `/tmp/pg*` TMPDIR) blocks release once
   its parent is PID 1 or `systemd`, that is, once the test that started it has
   gone (#1925). xvfb-run's default `/tmp/xvfb-run.*/Xauthority` never matches.
   Every other matching process, including a lab `Xvfb` whose parent still runs,
   is shared SSH work, such as another session's build or memory collector: it
   appears only in the summary and doesn't block release. If a new item belongs to another session (its
   notes or its owner say so), name it with `--not-mine ID`; the receipt records
   it. Never use `--not-mine` for your own items.
   Leave the putgd package as acquire found it: the same version, or none.
   `acquire`, `release` and `status --check` say `no putgd package installed` when
   there is none (with `config files of deb <ver> only` if a removal left them). A
   changed version counts as new, and a package that was there at acquire and is
   gone blocks the release. Reinstall it, or give `--removed-ok REASON` when the
   removal is intended; the receipt records the reason under `removedOk`.
4. Tell the next session in the queue and your coordinator.

Each release and break writes a JSON receipt to `~/putgd-lab/locks/history/` on
the host, with the old lock and the cleanup check.

### Never wait on a peer while holding a lock

- Don't hold a lock while you wait for another session's reply. If something on
  your desktop belongs to another session, send that session and your
  coordinator a notice. Name the exact PIDs, what you'll do (for example, log
  out) and when, at least 10 minutes ahead. If nobody objects by then, go ahead:
  silence means go.
- Processes left by a session that holds no lock on that host are leftovers, and
  your logout may end them. Never pattern-kill. Kill only the exact PIDs you
  started; other sessions' PIDs need the coordinator's OK.
- Nothing on a builder is Patrick's. Applications left open in a desktop session
  (browsers, terminals, btop), a pending system update and the installed package
  state are lab state. A logout, display-server restart, session switch or
  package change on a builder needs no approval from him; say in your report
  what you did. The check before one is for other sessions' work, as above, and
  the package rules below still hold (read every transaction; never change the
  desktop under test). His workstation is the one machine that is his, and it
  stays hands-off.
- A relayed approval with a condition, such as "Yes, coordinate first", means
  "notify the affected sessions with a deadline", not "wait for their reply".
  If your own permission check blocks a relayed approval, ask Patrick in your
  own session; don't work around it.

### Stale locks

`status` flags expired and stale locks. Message the holder (its session is in the
lock) and give it 10 minutes to answer. If it doesn't, check the host with
`status HOST --check`, then run `break HOST --session ID --reason TEXT`. The
tool refuses to break a live lock, including a blocked one that still sends
heartbeats. Breaking kills nothing. It writes a receipt with the old lock and
whatever appeared on the host during that lock, so the next holder knows what
was left behind. The coordinator breaks locks in its session family; a session
without a coordinator breaks one itself.

### Stay reachable

Messages reach an agent only between tool calls. Don't sit in one foreground wait
(sleep, an until loop or a long blocking command) for more than about two
minutes. Put long builds, waits and recordings in background tasks or monitors,
answer messages promptly and keep heartbeating.

### One coordinator per session family

A session that starts two or more sessions that will use the desktops is their
coordinator:

- It owns the schedule (who gets which desktop next) and keeps it in its notes
  folder. Sessions ask it for desktop time instead of negotiating with each
  other.
- It puts these rules in every brief, and relays Patrick's decisions with his
  words quoted. It also writes them to a decision log that its sessions re-read
  after a context compaction and before they open or finalize a pull request.
- It runs a watchdog (`desktop_lock.py status` on a timer, in the background)
  that flags expired, stale and blocked locks and heavy work on the workstation,
  and it acts on the alerts itself.
- When a session releases a desktop, it checks the host with `status HOST
  --check` and hands the desktop to the next session in the queue.

`tests/desktop/test_desktop_lock.py` covers expiry, heartbeat staleness, atomic
acquire, the release check and break rules, the quiet lease and one guarded
helper; it runs in CI's `lab` contracts.

### A nightly guest kept for live cleanup

A live S3, R2 or Proton cell publishes a real object. Before it publishes, it writes
`live-cleanup-pending.json` in the run directory
(`work/lab-runs/n<8 hex>/`), naming the destination, the prefix, the object key
and the run id. It removes the file once the app's delete is confirmed. If the
file is still there, or a receipt in the row says `unresolved_live_cleanup`,
the nightly keeps the guest: it still holds the History row and credential
reference that can delete the object. The slot stays in
`~/putgd-lab/locks/tier1-guest.lock.d/`, and every later nightly on that builder
stops at `guest retained: live remote cleanup unresolved` until a person
resolves it. The guest stays kept until you finish these steps.

The nightly also keeps a guest for other cleanup errors, and the same commands
recover it. Never retire the overlay, release the lease or remove the slot's
files by hand. Two cases change the steps:

- **No live publication pending.** The run directory has no marker and no
  receipt in the row says `unresolved_live_cleanup`; for example, a fixture
  cleanup only lost its SSH connection to the guest. Skip steps 1 and 2, do
  steps 3 and 4, and in step 5 add `--no-live-publication` and only the
  `--guest-only-error` values you checked. The guest needs no pause and no
  destination check. The command refuses if a marker or a receipt flag shows
  a publication after all, then follow every step. Its record names no
  publication, never removes a marker and never clears a live-cleanup flag. If
  no error needs recording either, go straight to step 6.
- **QEMU already exited.** The worker's transient unit can kill the guest's
  QEMU when it stops. A VM that is gone can't publish anything, but it can't be
  paused either. `--check-live-cleanup` then runs the same full host process
  check that retirement runs. It counts the guest as stopped only when no QEMU
  or other process uses the run's overlay. It records that time and prints
  `"guestExited": true` next to `"guestPaused": true`, and the 120 s wait runs
  from then. A pause recorded while QEMU was still running doesn't count once
  QEMU has exited, and an exit stops counting if anything writes to the overlay
  again (a QEMU started on it). In either case run `--check-live-cleanup` again
  and repeat the destination check.

Run the commands below on the builder that holds the slot, from its nightly
checkout, while no nightly worker is running there. They refuse a slot whose
worker is still alive. Only the manual nightly trigger (`--now`) runs on the d69
controller.

An upload can still finish after its worker died, either from an orphaned
recipe or Proton lend on the builder or from put.gd inside the guest. So no
destination check counts until `--check-live-cleanup` has run (step 2). It
refuses while a recipe or lend for the run is live. Otherwise it pauses the
guest's VM, or records that its QEMU has exited, so nothing in it can publish.
An upload that had already reached the destination can still commit just
after that, so the command returns only once 120 s have passed since the
recorded pause or exit. For a live
publication, `--resolve-live-cleanup` refuses unless the VM is still paused,
or its QEMU still gone, and that time has passed. Resuming the VM or starting
a QEMU on the overlay, for any reason, voids every check made before: run
`--check-live-cleanup` again and repeat the destination checks before step 5.
If a command refuses because a recipe or lend is live, wait until it stops,
then start again from step 1.

1. **Read the marker.** `cat` the run directory's
   `live-cleanup-pending.json`. Note `destination`, `prefix` and `objectKey`.
   The slot's `retained.json` (or `owner.json`) names the run and the row's
   evidence directory. If the marker is already gone, find the three values in
   the attempt's evidence or the provider console. The prefix is the folder
   before the file name, with its trailing `/`:
   `acceptance/tier1-live/<32 hex>/` for S3, `putgd-tier1-<32 hex>/` for
   Proton. If you can't pin them down, keep the guest and ask your
   coordinator.
2. **Pause the guest, then make sure the object is gone.** First run
   `python3 tools/lab/nightly_desktops.py --check-live-cleanup` (add
   `--destination`, `--prefix` and `--object-key` when the marker is gone),
   for either provider. It must print `"guestPaused": true` (and
   `"guestExited": true` when QEMU had already exited). Check the
   destination only after it returns. If the object is still there, delete it in the provider's own
   console, matching the exact key or folder from step 1, and run the command
   again. For S3 only, you can instead use **Delete from destination** or
   **Retry deletion** in the kept guest's put.gd, on the row whose key matches
   exactly, if put.gd is still running there. Resume the VM for that, then
   run `--check-live-cleanup` again: a check counts only while the VM stays
   paused, or its QEMU stays gone. A guest whose QEMU has exited can't run
   put.gd, so use the provider's console. put.gd can't delete a Proton file,
   so use the Proton web app.

   ```sh
   python3 - work/lab-runs/RUN_NAME <<'EOF'
   import sys
   from pathlib import Path
   from tools.lab import lab
   with lab.QMP(Path(sys.argv[1]) / 'qmp.sock') as qmp:
       qmp.execute('cont')
   EOF
   ```

   - **S3:** after pausing, the same command reads key set 2 in memory and
     changes nothing on the destination. It makes an
     authenticated GET of the exact key and lists the prefix. If the bucket
     has ever had versioning, it also lists the prefix's object versions, and
     it fails when it can't read the versioning state. `objectKeyAbsent`,
     `prefixEmpty` and `versionsGone` must all be `true` (exit 0). This check
     decides. `https://put.gd/<objectKey>` can keep serving a cached copy for
     a while after the object is deleted, so a response there doesn't keep the
     guest. If `versionsGone` is `false`, turn on **Show versions** in the S3
     console, permanently delete the remaining versions of the exact test key
     (and anything else under the prefix), and run the check again.
   - **Proton:** sign in to the put.gd Proton test account at
     `drive.proton.me`. The cell's folder is the prefix without its trailing
     slash (`putgd-tier1-<32 hex>`). Check all three places every time:
     **My files** must not have it, **Shared** (shared by me) must have no link
     for it or anything in it, and **Trash** must hold neither the folder nor
     any of its files (the file can be in Trash on its own). Revoke any link
     that is left, and permanently delete only that folder or its files.
   - **R2** (marker `destination: cloudflare-r2`): the command only pauses the
     guest; it does not read R2. Check the R2 test bucket at its origin with the
     bucket-scoped key, in memory as the cell does: the exact `objectKey` must be
     gone and the prefix empty. Delete the exact key there if it is still present.
     The public addresses (`https://cf.ul.gy` and the bucket's r2.dev address) can
     keep serving a cached copy after the delete, so they don't decide.
3. **Read the row's receipts.** For each `*/receipt.json` in the evidence
   directory, read `environment.cleanup_errors` and
   `environment.guest_local_cleanup`. Recovery retires the guest only when
   every cleanup error is guest-local. Check every other error, one by one,
   including `private session root cleanup refused`, which a failed live
   cleanup often leaves because the cell kept its fixture files on purpose.
   Look on the guest while it runs. When its QEMU has exited, use the
   receipt, the attempt's log and host state instead. An error counts as
   guest-only when it is about the guest's own files, processes or keyring
   entries, and nothing on the host or a remote service. The same text in
   another receipt needs its own check. Errors you can't explain from that
   evidence keep the guest: ask your coordinator. If you resumed the VM to look
   inside the guest and the row has a live publication, go back to step 2
   afterwards.
4. **Record attempts killed before their terminal proof.** An attempt
   directory with `receipt.json` but no `terminal.json` makes retirement
   refuse the whole row. Record each one:

   ```sh
   python3 tools/lab/nightly_desktops.py --unaudited-attempt ATTEMPT_DIR_NAME
   ```

   This writes `unaudited-attempt.json` next to its receipt, with the receipt's
   hash. Retirement then leaves that attempt out of `attachments.tgz`, keeps it
   in place, marks the completion `failed`, lists it under
   `unauditedAttempts` and hashes both files into `completion-manifest.json`.
   It never counts as a result. The command refuses an attempt that has any
   `terminal.json`: a terminal proof that fails its checks keeps the guest.
5. **Record the resolution.**

   ```sh
   python3 tools/lab/nightly_desktops.py --resolve-live-cleanup RUN_NAME
   ```

   `RUN_NAME` is the run directory name you checked, such as `n1234abcd`. The command refuses when the slot
   now holds another run, or while a lend or recipe for the run is still live
   (it may still be uploading). Add `--guest-only-error 'EXACT TEXT'` once for each error you checked in
   step 3. If the marker is already gone, add `--destination`, `--prefix` and
   `--object-key` with the values you checked in step 2. For a row with no
   live publication pending, add `--no-live-publication` instead and give no
   publication; the printed record must show `"publication": null`. Without
   that flag the command always records a publication as confirmed gone (the
   marker's, or the one you gave), so never run it without the flag for a row
   whose destination you didn't check. The command writes
   `live-cleanup-resolution.json` in the evidence directory. For a live
   publication it then removes the marker. The record names the run, its lab
   run id, the publication (or none) and the hash of every receipt in the row.
   The command refuses a marker for another run, values that differ from the
   marker and an error that no receipt holds.
6. **Recover the slot.**

   ```sh
   python3 tools/lab/nightly_desktops.py --recover-retained RUN_NAME
   ```

   This runs the recovery the next nightly would run, now: it retires the
   guest, releases the desktop lease (and a Proton lease whose lend is
   returned) and removes the slot. Run it alone, with no other option. The next nightly, or one started from d69
   with `--now`, does the same if you skip this step. Recovery uses the record
   only for receipts whose hash still matches. For those receipts it clears the
   live-cleanup gate (only when the record names a publication confirmed gone)
   and the errors you named, and nothing else. A receipt that changed or
   appeared after the record goes through the ordinary checks, so its own
   live-cleanup flag still keeps the guest. The other checks still apply too:
   the Proton lend receipts, the overlay owner, the exact QEMU process and the
   release check of the desktop lease. The command refuses another run and a
   live recipe or lend. It also refuses a desktop lease unless the lease is free, or is
   the retained session's own `nightly-desktops` lease and has expired or gone
   stale; a kept lease turns stale 15 minutes after its last heartbeat. A
   held Proton lease must be the retained session's own `nightly-desktops`
   lease with a matching lend receipt. When that lend isn't returned and the run
   directory still exists, recovery first runs `proton_lab.py recover` for it
   (bounded to 120 s) and refuses unless that returns the lend. If an earlier
   recovery reached guarded cleanup (the run has `overlay-cleanup.json`, or its
   `disk.qcow2` is already gone) and then stopped or had its lease release
   refused, a retry finishes or
   re-checks that cleanup against the row's existing `completion-manifest.json`
   instead of retiring twice. When the disk is gone, that check needs an
   `overlay-cleanup.json` that matches the manifest; without one the slot stays
   kept, so ask your coordinator. Its evidence goes to
   `operator-recovery/<time>-<hex>/stale-recovery/` in the row's evidence
   directory. A refusal prints its reason. An unexpected failure after that
   directory exists leaves `stale-recovery-error.json` in
   `operator-recovery/<time>-<hex>/`; one before it (an unreadable slot file)
   only prints. Only a command that exits 0 and prints its summary has removed
   the slot. Recovery, by this command or the nightly, writes
   `~/putgd-lab/locks/history/recovered/<time>-<run>-<hex>.json`. That record
   holds the slot's owner, which slot files were there, the reason the slot
   was kept and what cleared each gate. The records stay with the row's
   evidence.

## Weekly stress lane on k72

k72 runs the weekly builder stress lane (see [ci.md](../release/ci.md#weekly-run-on-k72)).
It's headless and takes no desktop lock, but it holds k72's
[quiet lease](#quiet-leases-for-timing-runs) while it runs. These are the user units for
`patrick`, with lingering enabled so they fire while nobody is logged in:

| Unit | Does |
| --- | --- |
| `putgd-stress-weekly.timer` | starts the service on Sundays at 03:00 local time (`Persistent=false`) |
| `putgd-stress-weekly.service` | `python3 -I ~/.local/libexec/putgd-stress/stress_weekly.py` (`Type=oneshot`, 4 h hang guard) |

It uses these paths on k72:

| Path | What |
| --- | --- |
| `~/.local/libexec/putgd-stress/` | the installed `stress_weekly.py` and `desktop_lock.py` |
| `~/work/put.gd-stress` | its own tree, cloned and updated by the wrapper; don't build in it |
| `~/work/flake-stress/weekly-DATE/` | each week's results, eight weeks kept |
| `~/.local/state/putgd-stress/` | `weekly.lock` (held while a run is active) and the last build size |
| `~/.config/putgd-stress/github-token` | the tracker token, mode 600, installed by Patrick only |

**Installing or updating.** Run the one-shot installer on k72 from a checkout of
the version you want, then delete the checkout:

```bash
src=$(mktemp -d /tmp/pgsw-install.XXXXXX)
git clone -q --depth 1 -b nightly https://github.com/patrick-hudson/put.gd-desktop.git "$src/put.gd"
sh "$src/put.gd/tools/lab/install-stress-weekly.sh"
rm -rf -- "$src"
```

The installer copies the wrapper and the units, runs `daemon-reload`, enables
the timer, enables lingering and prints the next run. Re-run it whenever
`tools/ci/stress_weekly.py` or the units change. The weekly tree updates
itself, but the wrapper does not.

**The GitHub token.** Patrick creates and installs the token himself. Never
create, copy, print or paste it in a session. It's a fine-grained token for
the `patrick-hudson/put.gd-desktop` repository only, with the Issues
permission set to read and write:

```bash
install -m 600 /dev/null ~/.config/putgd-stress/github-token
${EDITOR:-nano} ~/.config/putgd-stress/github-token
```

The wrapper reads the token only from a regular file owned by `patrick` with
mode 600. It passes the token only to the `flake_ledger.py` child as
`GH_TOKEN`. When the file is missing or loose, the week's `weekly.log` says
why, and the results stay local.

**The tests never see the desktop.** Plasma imports its session (`DISPLAY=:0`,
`WAYLAND_DISPLAY`, `XAUTHORITY`, `XDG_SESSION_TYPE=wayland` and more) into the
systemd user manager. On the first run on 2026-10-21, the tests inherited that
session and reached the live k72 desktop instead of their own Xvfb, and five
native and X11 tests timed out in every iteration. The unit now unsets those
variables (`UnsetEnvironment=`). The wrapper also gives every child an
allowlisted environment, like an SSH login: `HOME`, `USER`, `PATH`, the locale,
`XDG_RUNTIME_DIR` and the user bus. Any other user unit that runs tests on a
lab desktop needs the same treatment.

**Working beside it.**
- While a run is active, `pgrep -x ctest` and
  `flock -n ~/.local/state/putgd-stress/weekly.lock true` (which fails) show
  it. From 03:00 on Sundays it takes about 20 minutes after an incremental
  build, and more after a fresh clone (the first full build took 10 minutes).
  Don't start another full suite on k72 then.
- While a run builds or tests, `desktop_lock.py status k72 --resource quiet`
  shows the lane's lease. A timing run that finds it held does other work and
  retries later.
- To keep the run away from a timing run, hold the k72 quiet lease
  (`--resource quiet`) while the timing runs: the lane skips that week. A desktop lock whose purpose says quiet,
  A/V or performance work also makes it skip.
- `stress_weekly.py --dry-run` runs the checks and prints the plan without
  fetching, building or writing anything.
- To stop a run, use `systemctl --user stop putgd-stress-weekly.service`. It
  stops only that service's processes.

## Proton sessions on the builders

Each builder holds one put.gd Proton Drive session for the test account
phudson2@gmail.com, signed in once by Patrick: the original four on 2026-10-23
and Aurora on 2026-10-29. Use it for live Proton
tests on the builder or in its VMs. The tools, commands and recovery steps are in
[Proton Drive: lab sessions](../user/proton-drive.md#lab-sessions-on-the-builders); the
evidence is in [the 2026-10-23 lab sessions](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-23-proton-lab-sessions/README.md).

| Builder | Keychain reference | Default keyring | Lab app in `~/putgd-lab/proton/` | VMs that can borrow it |
| --- | --- | --- | --- | --- |
| f44 | `putgd/proton/lab-f44` | gnome-keyring `putgd lab` | extracted AppImage | Cinnamon, GNOME and KDE goldens |
| d69 | `putgd/proton/lab-d69` | gnome-keyring `putgd lab` | extracted debian13 .deb (#518) | Cinnamon, GNOME and KDE goldens |
| k72 | `putgd/proton/lab-k72` | KWallet `putgdlab` | extracted ubuntu26.04 .deb (#518) | Cinnamon and GNOME goldens (VT-x enabled 2026-10-30, [#659](https://github.com/patrick-hudson/put.gd-desktop/issues/659)) |
| m74 | `putgd/proton/lab-m74` | gnome-keyring `putgd lab` | extracted AppImage | Cinnamon and GNOME goldens |
| aurora-amd | `putgd/proton/lab-aurora-amd` | KWallet `putgd lab` | installed native source build; use `putgd-proton-lab` | Cinnamon and GNOME goldens |

Each builder's registry (`~/putgd-lab/locks/proton-registry.json`) lists the goldens it
lends to under `guests`, with each one's path, SHA-256, bootstrap key and host-key
pins, recorded by `proton_lab.py register-guests`. Every builder passed a
lend proof on 2026-10-29 and 2026-10-30 ([evidence](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-29-proton-guest-bridge/README.md)).

- **The Proton lock.** Take `desktop_lock.py acquire HOST --resource proton` before
  using a builder's session in any way: its lab app, a check, or lending it to a VM.
  It lives in `~/putgd-lab/locks/proton.lock`, next to the desktop lock, and is
  independent of it. One holder at a time: every refresh rotates the token, and two
  users of one session can sign it out for both. `status --resource proton` lists it.
- **Desktop work.** Opening the lab profile's app on the desktop (`proton_lab.py open`,
  for example for a new sign-in) also needs the desktop lock. Restore the browser
  handler afterwards: d69 and k72 send https to the ChatGPT app, so sign-ins there
  need Firefox set temporarily, with `mimeapps.list` backed up and restored byte for byte.
- **Keyrings.** The blank-password lab keyring is the default on every builder.
  Reboot/re-login persistence under autologin was proven on the original four:
  a reboot on k72, display-manager re-logins elsewhere. Aurora's initial
  verification covers a new resident reading the persisted session; its setup
  record is in the [Aurora section](#aurora-amd-fedora-45-builder).
- **VM guest keys and pins.** These are needed for the Proton-lending goldens:
  - On d69, the Ubuntu-based goldens accept `lab-bootstrap-d69/rec-ws/identity`. The
    plain `identity` times out at boot.
  - On f44, `lab-bootstrap-f44/known_hosts` pins port 22220 to the Cinnamon golden's
    host key. The GNOME golden needs `lab-bootstrap-f44/known_hosts-gnome`, the same key
    d69 pins for that golden, remapped to 22220.
  - KDE uses `known_hosts-kde` (port 22234 on d69, 22434 on f44).
  - k72, m74 and aurora-amd have `lab-bootstrap-k72/`, `lab-bootstrap-m74/` and
    `lab-bootstrap-aurora-amd/`, copied from f44's: `identity`, `known_hosts` (Cinnamon golden on port 22220) and
    `known_hosts-gnome` (GNOME golden on port 22220). The workstation keeps a
    copy of the pins and public key under `work/lab-bootstrap-<host>/`; the
    private key is the workstation's `work/lab-bootstrap/identity`.
  - On aurora-amd, run `proton_lab.py` through its adapter, and give `lab.py` the
    canonical `/var/home/patrick/...` paths. Guarded overlay cleanup refuses
    the `/home` alias as a symlink component.
  - The Mint and Cinnamon golden takes the ubuntu24.04 .deb; the GNOME and KDE goldens
    are Ubuntu and Kubuntu 26.04 and take the ubuntu26.04 .deb.
- **Lost session.** Patrick signs in again on that builder only. Nobody else signs in,
  signs out or copies a session.

## Remote VM operating procedures

Use [remote VM display tunnels](remote-vm-displays.md) to add each builder-hosted
QEMU guest to the existing wall. Use [golden image discovery and copying](remote-golden-images.md)
for current golden locations, restores, verified transfers and the approved shared guest login.

## VM preflight and golden-image copies

Before provisioning or launching a guest, inspect the actual remote host's CPU
virtualization support, accessible `/dev/kvm`, QEMU tools, RAM, storage/inodes,
running VMs, backing chains and reserved SSH/VNC ports. Retain per-host
[storage preflight](lab-storage.md) receipts. Provision missing distro packages
with the authorized noninteractive sudo. Do not count TCG as KVM acceptance or
start a hidden guest because display setup is incomplete. If the host cannot
meet the profile, record the blocked gate and use a suitable local VM fallback.

Golden-image copying is authorized. The workstation originals were retired on
2026-10-17, after Patrick approved moving them to verified Fedora, Debian and S3
copies. Those copies are now the immutable sources. Copying one is not a request
to move or retire it. For each transfer:

1. Inventory the source and its complete backing chain. Never copy a running
   guest's writable disk as a golden. Prefer the existing standalone provisioned
   goldens in the [golden inventory](remote-golden-images.md#current-inventory).
   If flattening is needed, produce a new standalone image and verify it; never
   rebase or alter the original chain.
2. Record the source image SHA-256, virtual size and sidecar/provisioning identity.
   Check remote capacity for the copy, staging and future overlays before transfer.
3. Copy to a fresh private directory under
   `~/work/put.gd/work/lab-images/` on the selected host, using a temporary filename
   and sparse transfer where supported. Never overwrite an existing golden or a
   disk referenced by overlays. Do not copy workstation private keys or secrets.
4. Verify the destination checksum, `qemu-img info` backing-file state and virtual
   size, and `qemu-img check`. Retain source/destination paths and verification
   receipts before promoting the complete image to its new immutable golden path.
   Never rewrite historic run metadata to point at the copy.
5. Create fresh disposable overlays under that host's
   `~/work/put.gd/work/lab-runs/` through the existing runner. Never boot either
   golden directly. Bootstrap separate guest identities on the host/run through
   the documented visible console; do not distribute the workstation key.
6. Run the lab controller on the actual QEMU host using the same committed source
   and required native dependencies. Preflight all resolved Unix socket paths
   there (107 UTF-8 bytes maximum), and keep guest displays on the workstation
   wall. The runner currently has no remote-host command-line switch.
7. Export source/build identities, functional artifacts, visual evidence,
   failures and completion/retirement receipts to the workstation before guarded
   cleanup of completed disposable overlays. Preserve both golden copies and
   existing projects. Follow [VM lifecycle](vm-lab.md) and
   [retention rules](../testing/evidence-retention.md).

The [historical base lifecycle](../history/windows-vm-lab.md) records the stricter
disk/UEFI/TPM/identity bundle rules for retired experiments; preserve those artifacts.

The 2026-10-16 access inventory and the 2026-10-17 Kubuntu preflight are in [the builder records](../history/remote-builders-records.md). Recheck capabilities and versions for every campaign; see [VM preflight](#vm-preflight-and-golden-image-copies).

## Linux Mint host preflight, 2026-10-20

Patrick added m74 (`192.168.68.74`) on 2026-10-20 as a build, VM and graphical
acceptance host, the lab's physical Mint Cinnamon desktop. Its host key
(ED25519 `SHA256:MlmnM8ur0e3UvkVJ4HHFr55bVK8rA3FzJ4uYzWKwks8`) was accepted at
Patrick's explicit direction and pinned in `work/remote-builders/m74/known_hosts`.
Pinned SSH and `sudo -n id -u` passed. It runs Linux Mint 22.3 with Cinnamon 6.6.4
on X11 (kernel 6.14.0-37), with LightDM autologin into Cinnamon. The machine has an
Intel i5-6500T (four logical CPUs, Intel HD 530 graphics, `/dev/dri/renderD128`),
31 GiB RAM and about 207 GiB free. `/dev/kvm` is group `kvm` and patrick is in
`kvm` and `libvirt`; `qemu-system-x86_64` is installed. It runs headless with an
EDID firmware override (`drm.edid_firmware=HDMI-A-1:edid/putgd-headless-1080p.bin`,
1920x1080), so the physical `:0` session keeps a monitor. The wall tile is
`X0tigervnc` on `:0` (user `putgd-native-vnc.service`, `127.0.0.1:5900`), reached
through the workstation's `putgd-mint-vnc.service` on `127.0.0.1:5974`. A separate
`Xtigervnc :1` virtual desktop (`remote-vnc.service`, `127.0.0.1:5901`) is not the
physical session and isn't acceptance evidence for physical Cinnamon. This
establishes access, KVM and display, not guest boot or acceptance readiness.


### m74 notifications belong to Cinnamon (#312), 2026-10-21

The `:1` virtual desktop runs Xfce under its own bus (`~/.vnc/remote-xstartup`:
`exec dbus-run-session -- startxfce4`), but Xfce's autostart entry
(`/etc/xdg/autostart/xfce4-notifyd.desktop`) runs
`systemctl --user start xfce4-notifyd.service`. That starts the unit with the user
manager's environment, which means the shared `/run/user/1000/bus` with `DISPLAY=:0`. After
m74's reboot at 02:55Z it claimed `org.freedesktop.Notifications` there two
seconds before Cinnamon. put.gd's notifications on m74 were then drawn by
xfce4-notifyd, and a click on the body didn't run the default action.

The fix masks the user unit: `systemctl --user mask xfce4-notifyd.service`
(`~/.config/systemd/user/xfce4-notifyd.service` → `/dev/null`). On the shared bus,
activation through that unit now fails, so only Cinnamon claims the name. In the
`:1` session the autostart falls back to `exec xfce4-notifyd` on its own bus.
Applied at 16:17Z, followed by `systemctl --user stop xfce4-notifyd.service`, which
passed the name to the already-queued Cinnamon without a logout:

```
before  shared bus   ('Xfce Notify Daemon', 'Xfce', '0.9.4', '1.2')   owner: xfce4-notifyd (the unit)
        :1 bus       ('Xfce Notify Daemon', 'Xfce', '0.9.4', '1.2')   owner: xfce4-notifyd (the :1 session's)
after   shared bus   ('cinnamon', 'GNOME', '6.6.9', '1.2')            owner: cinnamon --replace
        :1 bus       ('Xfce Notify Daemon', 'Xfce', '0.9.4', '1.2')   owner: xfce4-notifyd (unchanged)
```

To check: `gdbus call --session -d org.freedesktop.Notifications -o /org/freedesktop/Notifications -m org.freedesktop.Notifications.GetServerInformation`
with `DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus`. Rollback:
`systemctl --user unmask xfce4-notifyd.service`. With the mask in place, a put.gd
update notification's default action (a click on the body) opened its link again
on m74 at 16:53Z. Evidence:
[2026-10-21-test-lab-reliability](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-21-test-lab-reliability/README.md).
