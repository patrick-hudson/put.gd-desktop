# Package install retries and snapshot (#368) — 2026-10-22

Code: [#388](https://github.com/patrick-hudson/put.gd-desktop/pull/388), head
`aca7858f`, all CI jobs green
([run 39727602972](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/39727602972)).

| File | What it shows |
| --- | --- |
| `368-before.png` | Run 39717118907, attempt 1, job `packages / native (fedora44)`, #357 head `fd9e255`: the public job page with the failed "Standard distro dependencies" step, and that step's log lines. The logs come from the job-logs API, because GitHub shows logs only to signed-in viewers. |
| `before-fedora44-log-excerpt.txt` | The same job's dnf lines for mesa 26.2.4. Every mirror, `dl.fedoraproject.org` included, answered 404 or broke TLS. |
| `368-after.png` | Run 39729960271 on throwaway commit `e8f9443b`. That commit is `aca7858f` plus a demo-only workflow change (not merged) that wrapped `dnf`/`apt-get` to inject mirror faults. It shows the public job page of `native (fedora44-snapshot)` and every annotation the helper wrote. |
| `demo-annotations.tsv` | Those annotations as returned by the check-runs API, per job id. |

The demo jobs, all using the pinned images and the snapshots saved by run
39727602972:

- `fedora44`: the `updates` repo pointed at `wrong.host.badssl.com` on attempt 1
  (TLS failure). Attempt 2 passed, and the RPM built and audited.
- `ubuntu24.04`: every apt fetch went through a refused proxy on attempt 1.
  Attempt 2 passed.
- `fedora44-snapshot` and `debian13`: all four online attempts failed (30, 60
  and 120 s back-off). The install ran offline from the package snapshot and the
  package built and audited. The snapshot cache save was skipped.
- `fedora44-allfail`: the online attempts and the snapshot install all failed
  (`Cache-only enabled but no cache for repository "updates"`), and the job
  failed.

The job summary tables ("Package install retries") are visible only to
signed-in viewers, so the pictures show the identical annotation lines instead.
The helper's contract suite is `tests/ci/test_install_packages.py`. A lab check
on f44 (podman, `fedora:44` and `ubuntu:24.04`) cut networking and confirmed
three things: the offline install from a snapshot works, a package outside the
snapshot fails red, and an unknown package fails without a retry.
