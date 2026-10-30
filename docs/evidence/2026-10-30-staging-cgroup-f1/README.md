# Scoped staging survives ended logins: #780 F1, F3 and F5

[Code PR #798](https://github.com/patrick-hudson/put.gd-desktop/pull/798) and
[docs PR #801](https://github.com/patrick-hudson/put.gd-desktop/pull/801) remain
unmerged; issue #780 stays open. This follows the
[B1 evidence](../2026-10-30-staging-cgroup-b1/README.md), whose closing-login
step used `loginctl kill-session --signal=KILL`. sudo cannot relay SIGKILL, so
that run never exercised the relay that killed the wrapper in independent
validation.

## Changes under test

- **F1.** The outer sudo stays in the SSH login's `session-N.scope` and relays
  the SIGTERM/SIGHUP that ending the login sends it. In a system scope the
  wrapper now forks first: only a waiter stays sudo's child and reports the
  exit status, while the wrapper continues in a new session, reparented to
  PID 1. Ending the login ends sudo and the waiter only.
- **F3.** `recover-staging` releases an admitted supervisor's job once its
  terminal, root-owned `result.json` exists and its
  `putgd-supervisor-install-<id>.scope` is `populated 0` or authenticated
  gone, with every wrapper identity and session check, under `install-guard`.
  Session-tracked or identity-less supervisors, `progress.json` without a
  result, and populated or unreadable cgroups stay held. Refusals now name
  their reason instead of `ssh failed (exit 3)`.
- **F5.** The lab user is no longer granted `systemd-run`. The root-owned
  `package_scope.py` fixes every `systemd-run`/`setpriv` argument and is granted
  alone: by new bootstrap, and per overlay by each install's staging hook.
  Existing goldens and snapshots are unchanged; a fresh overlay's first staging
  falls back to the session scan.

## Acceptance

Placement: boat. Exactly two sandboxes, one per snapshot, each with fresh
owned overlays: `putgd-lab-cinnamon-v3` / `bx_ecm5jez9` (Mint 22.3, systemd
255, sudo 1.9.15p5) and `putgd-lab-ubuntu-gnome-v4` / `bx_9gerfjdw` (Ubuntu
26.04.1, systemd 259, sudo-rs 0.2.13). Both guests kept `Linger=no`. Every case
ran the real `tools/lab/lab.py install` CLI with a synthetic `putgd` DEB and
empty APT sources. Each held case killed that controller's process group with
SIGKILL on the QEMU host, then ended the guest login only with a plain
`systemctl stop session-N.scope` or `loginctl terminate-user lab`. The relay
was not sidestepped.

| Acceptance | Mint 22.3 | Ubuntu 26.04 |
| --- | --- | --- |
| Session fallback without the grant: the first install on a fresh overlay stages under the session scan (`scope launcher ... is not installed`, controller warning printed), grants the launcher, then uses a system scope for the supervisor; `ok=true`, lock released. The grant names only `package_install.py` and `package_scope.py` (root, 0755, matching hash). | [results](cinnamon-results.json) case 1, [controller](cinnamon-fallback-controller.log) | [results](ubuntu-results.json) case 1, [controller](ubuntu-fallback-controller.log) |
| Supervisor phase, `systemctl stop` of the login holding the outer sudo: sudo and the waiter end, the wrapper (UID 1000) stays in its scope, recovery refuses while it runs, the root helper finishes with `ok=true`, the wrapper writes `supervisor-result.json` (exit 0), then `recover-staging` releases (`supervisor-finished`). | case 2 | case 2 |
| Supervisor phase, `loginctl terminate-user lab`: the same outcome. On Mint, logind did not stop the already-closing SSH login again, so sudo survived until a plain stop of that login, recorded as a second step. Ubuntu's terminate-user ended it directly, and `user@1000.service` left the active state (`failed`). | case 3 | case 3 |
| Staging phase, held by a guest-wide APT Pre-Invoke gate, with `systemctl stop` and then, as a separate case, `terminate-user`: the staging wrapper survives, writes `staging-result.json` (exit 0) once the gate opens, and `recover-staging` releases (`staging-finished`). | cases 4, 5 | cases 4, 5 |
| A root `setsid()` daemon from the guest's own Pre-Invoke hook, in a separate session: after the controller is killed, the login is stopped and the exact wrapper is SIGKILLed, both ordinary and `--abandoned` recovery refuse with `staging cgroup remains populated` and the lock stays held. A root observer sees `populated 1` then the scope gone, and `--abandoned` releases. | case 6 | case 6 |

Each case records the wrapper, waiter and sudo identities (PID, start ticks,
UID, session, cgroup), the login scope, one-second samples after the login
ended, the refusal text while running, the terminal receipts and the recovery
report. The drivers' [Mint](cinnamon-driver.log) and [Ubuntu](ubuntu-driver.log)
logs end with `{"passed": true, "cases": 6}`.

[Source identity](source-identity.json): both guests ran byte-identical
`lab.py`, `package_staging.py`, `package_install.py` and `package_scope.py`
from code commits `ee128aeb` and `05481f0f`. Mint ran the `05481f0f` driver.
Ubuntu ran the `ee128aeb` driver, which lacks only the conditional second stop
for a login that `terminate-user` leaves closing. Ubuntu never needed that step:
its assertions required the login, sudo and waiter gone after
`terminate-user` alone.

## Unit regressions

Local, fakes only: [66 lab](local-unit-lab.log),
[12 supervisor/launcher](local-unit-package_install.log),
[29 cleanup](local-unit-overlay_cleanup.log) and
[41 staging](local-unit-staging_recovery.log). The new F1, F3 and F5 tests were
copied onto `ef64bfa5` and all fail there:
[log](new-tests-on-ef64bfa5.log). On that head the relayed signal kills the
wrapper, the release keeps the lock with only a bare guard refusal, and
`package_scope.py` does not exist. The F1 test now tracks its shell child, so
a regression fails without orphaning a loop
([log](f1-tests-on-ef64bfa5-no-orphans.log)).

## Failure retained

The first Mint attempt asserted that `terminate-user` alone ends the login
holding sudo. It does not on Mint 22.3: logind leaves an already-closing
session to finish
([failure](cinnamon-attempt1-failure.json)). The wrapper had survived. The
held job was then released by the new supervisor path
([recovery](cinnamon-attempt1-supervisor-recovery.json): `supervisor-finished`,
scope gone, `ok=true`). That overlay was retired as failed
([preview](cinnamon-attempt1-cleanup-preview.json),
[cleanup](cinnamon-attempt1-cleanup.json),
[postcheck](cinnamon-attempt1-post-cleanup.json)). The driver then gained the
recorded second stop, and a fresh overlay ran every case.

## Retirement

Raw exports were copied and verified file by file against sandbox hashes
(Mint 79 files including the failed attempt, Ubuntu 44) before guarded
cleanup. Each overlay was retired once, with a separate dry run:
Mint [preview](cinnamon-cleanup-preview.json), [cleanup](cinnamon-cleanup.json),
[postcheck](cinnamon-post-cleanup.json); Ubuntu
[preview](ubuntu-cleanup-preview.json), [cleanup](ubuntu-cleanup.json),
[postcheck](ubuntu-post-cleanup.json). Both sandbox deletions completed
([operations](sandbox-deletions.json)); the named snapshots remain. The
forwarded bootstrap agent was stopped. No physical desktop, Proton session, S3
credential or workstation apt was used, and no lock was needed. The root
observer key, APT fixture config and gate hook existed only in the disposable
overlays. There is no product UI change to picture.
