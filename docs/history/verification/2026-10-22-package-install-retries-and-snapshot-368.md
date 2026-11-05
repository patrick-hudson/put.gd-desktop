# Verification record

> **Historical record.** This is a dated run record, moved unchanged from [implementation evidence](../../testing/verification.md) on 2026-11-05 (#1222). It describes what that run found for the build it names, and its open or pending statements describe that time. "Above" and "below" refer to its neighbours on the old page, in the order of the [history index](../README.md#verification-run-records). For today, see the [current state](../../testing/verification.md#current-state). Do not use it as a work list.

## Package install retries and snapshot (#368) — 2026-10-22

Code: [#388](https://github.com/patrick-hudson/put.gd-desktop/pull/388), head
`aca7858f`, all CI jobs green
([run 39727602972](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/39727602972)).
Policy: [CI coverage, package installs](../../release/ci.md#package-installs). Evidence:
[`records/2026-10-22-package-installs-368/`](https://github.com/patrick-hudson/put.gd-evidence/tree/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-22-package-installs-368).

- **Before.** Run 39717118907 (#357, `fd9e255`): `native (fedora44)` failed
  twice because no mirror, `dl.fedoraproject.org` included, served mesa
  26.2.4.
- **Demo.** Run 39729960271 on throwaway `e8f9443b` (`aca7858f` plus injected
  mirror faults, not merged). A fault on attempt 1 passed on the retry (fedora44,
  ubuntu24.04). Four failed attempts installed from the snapshot and the
  packages built (fedora44-snapshot, debian13). A fault on every attempt and on
  the snapshot failed the job (fedora44-allfail).
- **Lab.** On f44 (podman, pinned `fedora:44` and `ubuntu:24.04`), with
  networking cut: the offline install from a snapshot passed, a package outside
  the snapshot failed, and an unknown package failed without a retry.

Not covered: the `ci.yml` jobs' installs (#389).
