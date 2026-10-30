# Session-independent staging scopes: #780 B1 correction

Companions: [code #798](https://github.com/patrick-hudson/put.gd-desktop/pull/798)
and [docs #801](https://github.com/patrick-hudson/put.gd-desktop/pull/801).
The prior user-manager checkpoint is superseded: its normal liveness tests
passed but it did not cover ending the last login. Independent validation
showed PID 1 killing the watchdog inside `user@1000.service` with lingering off.

New launches create delegated **system-manager scopes** under `/system.slice`.
The root launcher drops the wrapper back to the guest user with `setpriv`
before staging. Root package work inherits the system scope. No lingering is
enabled; stopping the user manager no longer owns the watchdog's cgroup.
Atomic `cgroup.events populated`, mandatory session scans, all #761 guards,
name-collision refusal and receipt-after-release remain required.

The driver is maintained in the code PR at `tools/lab/staging_acceptance.py`.
Historical executed drivers remain in their hashed private evidence archives.
The docs PR contains evidence and prose only. The linked source and final
identities will be frozen after the live campaign.

The launcher detects systemd <254 before using `--expand-environment=no`.
Unavailable prerequisites retain the session scan with a specific recorded
reason and an install-time warning. Deliberate privileged migration remains a
limit: `cgroup.procs`, `systemd-run` and `systemctl start` can move/start work
elsewhere; the session scan catches members retaining the session ID, while
changing both cgroup and session escapes both fences.

Validation is in progress on exactly two new forks: `putgd-lab-cinnamon-v3`
and `putgd-lab-ubuntu-gnome-v4`, with fresh overlays. No physical desktop,
S3 credential or Proton session is used. The required repeated cases are
production staging, setsid survivor/recovery, root scope/admission, collision
refusal, and the new real watchdog after logout plus controller SIGKILL.
