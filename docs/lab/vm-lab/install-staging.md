# VM lab: install staging and recovery

Part of [Disposable graphical lab](../vm-lab.md).

This continues [Package and run](package-and-run.md#package-and-run).

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
