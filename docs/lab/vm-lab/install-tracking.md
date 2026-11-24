# VM lab: install launcher and process tracking

Part of [Disposable graphical lab](../vm-lab.md).

This continues [install staging](install-staging.md).

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
[B1 evidence](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-30-staging-cgroup-b1/README.md). That run
ended the closing login with `kill-session --signal=KILL`, which sudo cannot
relay. Independent validation then showed a plain stop of that login killing
the supervisor wrapper on both guests, and `terminate-user` killing it on
Ubuntu (sudo-rs), through sudo's relay. The
[login-end evidence](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-30-staging-cgroup-f1/README.md) repeats
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
See the [Cinnamon cancellation proof](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-30-lab-staging-748/README.md)
for the earlier exact identities, refused retries, later successful install and cleanup.
The [cgroup recovery regression](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-30-staging-cgroup-780/README.md)
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
