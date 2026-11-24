# Verification record

> **Historical record.** This is a dated run record, moved unchanged from [implementation evidence](../../testing/verification.md) on 2026-11-05 (#1222). It describes what that run found for the build it names, and its open or pending statements describe that time. "Above" and "below" refer to its neighbours on the old page, in the order of the [history index](../README.md#verification-run-records). For today, see the [current state](../../testing/verification.md#current-state). Do not use it as a work list.

## Login-startup uninstall contract (#382) — 2026-10-24

Patrick corrected the contract: remove managed startup links for active accounts
when the installation disappears, and for inactive accounts at next login;
preserve settings and keep package scripts out of homes. Code PR #544 and docs
PR #545 implement/document that choice. Clean `d5b561dd` development artifacts
passed **five native and six actual AppImage** targeted lifecycle rows across
Mint/Cinnamon X11, Fedora/GNOME Wayland, Debian/Kubuntu Plasma Wayland, Ubuntu
GNOME Wayland and Aurora Plasma Wayland. Every row includes real enabled and
post-removal logins, active cleanup with the resident quit, unchanged settings/
data, explicit CLI disable, reviewed pictures and a 180-second memory check.
All RSS/PSS measurements are below 40 MiB. Fedora native's strict library-map
failure is a proven pre-existing distro Qt/OpenSSL dependency and remains
explicit in the raw receipt. Startup CTest, 26 shell tests, 14 source-collection
tests and complete branch/integration CI passed; earlier failing attempts are
retained. Test VMs were retired after verified export and physical locks released.

[Exact matrix, source/artifact identities, pictures and limits](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-24-uninstall-startup-382/package-acceptance/README.md).
#382 is closed per Patrick’s explicit decision that old entries in accounts
which never ran the updated app are not a first-release blocker given limited
existing usage. The [decision and closure receipt](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-24-uninstall-startup-382/legacy-rollout-decision.json)
record that exception. Code #544 merged into `nightly` at
`35a16d0e6c63bc57bffe8d7bfeeeca5ad428c28c`; companion #545 carries the docs.
Published-nightly lifecycle verification, including AppImage replacement with
real channel metadata, continues under #384. These results are targeted
development-artifact acceptance, not full A–Q/A–K or stable-draft certification.
The retained-link/TryExec tests at `91864c41` are superseded history.
