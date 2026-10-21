# Preferred build and graphical test servers

Patrick's four physical Linux servers are the preferred project build, VM and
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

The user authorizes full root access on these four machines through passwordless
sudo: install normal distro dependencies, configure build tools, compile and run
tests using `sudo -n COMMAND` (or `sudo -n -i`). `sudo -n id -u` must report `0`.
Do not apply this remote authorization to the workstation or unrelated machines.
If noninteractive sudo fails, report it rather than requesting or recording a
password. Keep distro Qt dynamically linked and use the project's pinned official
AWS SDK; see [build instructions](build.md).

The [Linux acceptance matrix](acceptance-matrix.md) covers Mint, Ubuntu, Kubuntu,
Fedora and Debian, GNOME/KDE, and native X11/Wayland. Combine host desktops and
builder-hosted VMs as needed; physical-hardware release gates require the
appropriate physical machine. Provision missing profiles and record local fallbacks.

## Graphical acceptance on the physical desktops

The build servers themselves are authorized graphical acceptance targets. Build,
install and run the candidate directly in Fedora's existing GNOME/Wayland session,
Debian's Plasma/Wayland session or Kubuntu's Plasma/X11 session. Use the
corresponding physical-host tile on the workstation wall, preserve the user's
other windows/projects, and save screenshots or recordings of the actual app
behavior and failures.
Record the host OS, compositor/session version, exact source and running build,
scenario and result. SSH output or an offscreen test alone is insufficient.

These host-desktop checks do not require a VM or `/dev/kvm`. Select direct host
testing when its actual desktop/version matches the claim being tested; use a
VM for other required guest profiles. Preserve the release matrix's exact versions.

## Pinned SSH access

Connect from the main workstation on the LAN. All four hosts accept
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
```

For unattended commands, also use `-o BatchMode=yes` and a bounded connection
timeout. A missing pin or mismatch requires verified identity recovery, not
trust-on-first-use. Local bootstrap notes and tunnel units live privately under
`work/remote-builders/f44/`, `work/remote-builders/d69/`, `work/remote-builders/k72/` and
`work/remote-builders/m74/`.

## Visual results are required

All three physical desktops appear at [the display wall](http://127.0.0.1:8765/).
Tiles start view-only; select **Enable KB + Mouse** to interact, then return to
view-only when finished. VNC has no password and must remain restricted to
loopback, reached through authenticated SSH tunnels. Never expose it to the LAN.
The services in the inventory table run on the workstation. Inspect/recover the
existing service before starting a duplicate tunnel or taking its port.

Each tile's VNC server runs in the host's desktop session as user `patrick`
(found on the hosts on 2026-10-19; none of it is in this repository):

| Host | Server | Unit | Listens |
| --- | --- | --- | --- |
| f44 GNOME Wayland | `w0vncserver` from Fedora's `tigervnc-wayland-server` 1.16.2 | user `putgd-gnome-vnc.service` (`WantedBy=graphical-session.target`) | `127.0.0.1:5901` |
| d69 Plasma Wayland | `/usr/local/bin/w0vncserver`, TigerVNC `v1.16.2` (`b555312d`) built from source in `~/work/bootstrap` (CMake Release, `ENABLE_WAYLAND=ON`, no viewer) | user `w0vncserver.service` (Wayland + KDE only, `WantedBy=plasma-workspace-wayland.target`) plus the `localhost.conf` drop-in that binds it to loopback; `wall-vnc-watchdog.service` (since 2026-10-20, #164) restarts it when its stream stops | `127.0.0.1:5900` |
| k72 Plasma X11 | `X0tigervnc` (Ubuntu's `tigervnc-scraping-server` 1.15) on the X display | user `patrick-vnc.service`, started by the `patrick-vnc` autostart entry only in an X11 session | `127.0.0.1:5900` |
| m74 Cinnamon X11 (added 2026-10-20) | `X0tigervnc` (Mint's `tigervnc-scraping-server`) on the physical `:0` display | user `putgd-native-vnc.service` | `127.0.0.1:5900` |
| k72 Plasma Wayland (added 2026-10-20, #87; default session) | `/usr/local/bin/w0vncserver`, TigerVNC `v1.16.2` (`b555312d`) built in `~/work/bootstrap-w0vnc` like d69's | user `w0vncserver.service` (Wayland + KDE only, `WantedBy=plasma-workspace-wayland.target`), run under `systemd-inhibit --what=idle:sleep --mode=block` by the `10-inhibit-sleep.conf` drop-in; `wall-vnc-watchdog.service` restarts it when its stream stops | `127.0.0.1:5900` |

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
([receipt](evidence/2026-10-20-recording-closeout.md#164-wall-vnc-watchdog-on-d69)).
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
needs KDE/X11, then switch it back. Each switch restarts `sddm` and logs
Patrick out, so the coordinator checks for his apps first.

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
  (mirrored outputs count once). `cover_player.py outputs` lists them.
- `cover_player.py precheck --port 5900` (5901 on f44) reads one frame from the
  host's loopback VNC tile, in memory, and fails unless the output is at least
  98% cover colour. Run it before recording anything.
- `cover_player.py check-frame FRAME [--crop OUT]` deletes a saved frame unopened
  when it is under half cover colour, and `--crop` keeps only the covered
  output.
- `record_run.py` runs a whole check on the host: cover, precheck, each build's
  resident with private config/data/cache/state, `putgd-cli` recordings
  (monitor, region or window; any format; optional pause), `--motion`,
  `--av-sync` split at the pause, one gated frame per copy (`--frames`), and
  the kernel's GPU hangs for the run. A region must straddle the video's edge
  (for example `--rect 0,0,960,540`) to pass the frame gate. `--vnc-port 0`
  skips the precheck, in disposable guests only. See the script's docstring for
  the options.

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
  `X-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1` and its `Exec` path
  (remove it afterwards and run `kbuildsycoca6`). On f44 the full-screen cover
  stayed behind the session's windows (0% cover); crop strictly inside your own
  window there and check every crop edge is its background token.

Keep graphical tests visible there throughout the run. Capture screenshots or
short recordings of the initial desktop, decisive actions, results, failures and
completion. Save the visual files with the host, session/compositor/version, exact
source commit/tree, installed and running build identities, scenario parameters
and pass/fail result. Preserve failed attempts. Exclude revealed credentials and
protected login values from screenshots, recordings, logs and review material.
Follow [evidence retention](evidence-retention.md).

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
  d69 (Debian 13 Plasma Wayland, `.69`), k72 (Kubuntu 26.04 Plasma Wayland, `.72`) and
  m74 (Linux Mint 22.3 Cinnamon X11, `.74`) each serve one session at a time, under a lock from `tools/lab/desktop_lock.py`.
  You need the lock for anything that runs in the desktop session, draws on it,
  sends it input, captures or records it, or logs it out. That includes installing
  a package, extension or desktop entry the session loads.
- **SSH builds, tests and VMs on the builders are shared.** Run builds with
  `nice -n 10` and a bounded `-j`. Check `uptime` before you start another heavy
  job on a host whose load is already above its core count; k72 and m74 have four cores.
- **Recording, capture-latency and memory slots need a quiet host.** While someone
  holds a desktop for that work, don't start new heavy SSH jobs on that host
  (builds, flatpak-builder, full ctest). Jobs already running may finish. The
  slot holder records the host load next to its results.
- **Don't change the desktop under test.** Never install, upgrade or downgrade
  compositor or desktop packages on these hosts: mutter, gnome-shell, kwin,
  plasma, xorg, mesa, GPU drivers, pipewire or xdg-desktop-portal*. Read every
  dnf or apt transaction before you confirm it, so a tool install doesn't pull
  them in. Use a nested or containerised setup instead.
- **The workstation is Patrick's.** No builds, full ctest, packaging,
  flatpak-builder, VMs or encoding runs there. Git, editing, reading and a single
  short test are fine.

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
python3 tools/lab/desktop_lock.py acquire f44 --owner NAME --session ID --purpose TEXT --minutes 30
python3 tools/lab/desktop_lock.py heartbeat f44 --session ID [--blocked REASON]
python3 tools/lab/desktop_lock.py extend f44 --session ID --minutes 30
python3 tools/lab/desktop_lock.py release f44 --session ID [--verified-clean] [--not-mine ID ...]
python3 tools/lab/desktop_lock.py break f44 --session ID --reason TEXT
```

- `--session` is how other sessions reach you. Use your Claude Code `local_...`
  id; a Codex or terminal session uses a unique name such as `codex-<branch>`
  and says in `--purpose` where its notes are.
- Acquire before you start anything on the desktop, not while you prepare. If
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
- `acquire` lists what's already on the host, such as an earlier holder's
  processes or Patrick's apps. Those aren't yours: leave them and tell your
  coordinator. `status` shows who released or broke the lock last.

### Release only after a full cleanup check

Processes and agents started over SSH aren't part of the desktop session. A GNOME
or Plasma logout doesn't stop them, and neither does ending your agent session.
Before you release:

1. Stop everything you started on the host: residents, `flatpak ps` instances,
   input agents (`sudo -n systemctl stop putgd-lab-input`), pattern windows and
   recordings, plus the workstation-side SSH forwards. Kill only the exact PIDs
   you started.
2. Remove anything temporary you added: desktop entries (then run
   `kbuildsycoca6 --noincremental`), user extension copies (then log out and in
   again, and check that the packaged extension loaded), installed packages, and
   `~/.local/share/putgd`, `~/.cache/putgd` or `~/.config/putgd` if your runs
   created them.
3. Run `release HOST --session ID`. On the host it runs `flatpak ps` and
   `pgrep -u patrick -af 'putgd|host_input|videotestsrc'`, and checks for user
   putgd desktop entries, extension copies and data, and an installed `putgd`
   package. It lists anything that appeared since you acquired the lock and
   doesn't release yet. Once nothing of yours is left, run it again with
   `--verified-clean`; the lock is released only if the check passes. Items that
   were there at acquire are reported but don't block. A matching process counts
   if it runs in the desktop session (`DISPLAY` or `WAYLAND_DISPLAY` is set), is
   an input agent or test pattern, or has an environment the tool can't read.
   Anything else is shared SSH work, such as another session's build or memory
   collector, and is only counted. If a new item belongs to another session (its
   notes or its owner say so), name it with `--not-mine ID`; the receipt records
   it. Never use `--not-mine` for your own items.
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
- Patrick's own apps in a desktop session (browsers, terminals, btop, other
  agents such as Codex) are never leftovers. Before any logout or display-server
  restart, check the session for them. If any are there, ask Patrick, and here
  silence never means go.
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
acquire, the release check and break rules; it runs in CI's `lab` contracts.

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
   [retention rules](evidence-retention.md).

Windows bases retain their stricter disk/UEFI/TPM/identity bundle and independent
copy rules; see [Windows lab](windows-vm-lab.md). Copying a Linux golden does not
authorize partial Windows-base transfers.

## Access inventory checked 2026-10-16

Read-only pinned-SSH checks reached both hosts, identified Fedora 44 and Debian 13,
and confirmed `sudo -n id -u` returns `0`. Fedora had `qemu-system-x86_64` and
`qemu-img`; Debian had neither command on the tested PATH. `/dev/kvm` was not
present in either check. VM-host readiness therefore still requires preflight
and setup; these checks do not establish guest or graphical app acceptance.
Recheck capabilities and versions for every campaign rather than treating this
inventory as a permanent readiness receipt.

## Kubuntu host preflight, 2026-10-17

Pinned SSH and `sudo -n id -u` passed on `.72`. The machine has an Intel i5-6500T
(four logical CPUs), 15 GiB usable RAM and approximately 141 GiB free at preflight.
`/dev/kvm` is readable/writable by patrick; KVM API version 12 and VM creation
passed. This establishes KVM access, not guest boot or acceptance readiness.

The existing Plasma 6.6.6 X11 session renders at 1920x1080. Its forced HDMI-A-1
EDID arguments are active in the current boot, firmware is present in initramfs,
and GRUB/hook syntax checks passed. Existing automatic X11 login and the VNC
sleep/idle inhibitor were preserved. TigerVNC now listens only on loopback;
direct LAN VNC is refused. Its `.72` wall tile passed mouse/keyboard checks and
was returned to view-only. No sharing-portal approval is needed for this X11 session.

Native build bootstrap uses the committed `909f8ee` source archive, standard
distro packages and the verified official AWS SDK 1.11.907 dependency lock.
Exact source/tool versions, build outcome and access/display evidence are retained
privately in `work/remote-builders/k72/`. This setup did not perform a fresh reboot,
guest boot, graphical put.gd acceptance or a memory campaign.

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
`systemctl --user unmask xfce4-notifyd.service`. Evidence:
[2026-10-21-test-lab-reliability](evidence/2026-10-21-test-lab-reliability/README.md).
