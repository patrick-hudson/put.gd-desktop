# Stalled package installs (#467) — 2026-10-23

Code: [#498](https://github.com/patrick-hudson/put.gd-desktop/pull/498), head
`1926941a`, all CI jobs green
([run 40008537011](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/40008537011)).

| File | What it shows |
| --- | --- |
| `467-after.png` | Run 40008409878 on throwaway commit `04aac39e`: the public run page with its three jobs, and each job's "Package install retries" summary. |
| `summaries.md` | Those three summaries in full, copied from the job logs. |
| `demo/stall-demo.yml`, `demo/proxy.py` | The demo-only workflow and stalling proxy, from branch `claude/ci-install-timeouts-467-demo` (never merged, deleted after the run). |

Commit `04aac39e` is #498's first commit (`6e75f69b`) plus the demo files.
`tools/ci/install-packages.sh` is identical there and at #498's head. Each job
ran on a hosted `ubuntu-24.04` runner. apt used plain-HTTP
`archive.ubuntu.com` sources through `demo/proxy.py`. While stalling, the proxy
sends response headers and then one body byte every 15 s, so apt prints
`Get:1 … InRelease [1049 kB]` and then nothing, and its own 60 s idle timeout
never fires. That is the shape of the nightly hang in run 39851369595.

- **Stall, then retry** (job 120602161071, 2 min 10 s, green). With
  `PUTGD_PKG_STALL=60`, the proxy stalled for the first 75 s. Attempt 1 was
  stopped 60 s after its last line. After the 30 s back-off, attempt 2 passed
  and installed dwz 0.15.
- **Stall, then snapshot** (job 120602160473, 3 min 14 s, green). A first
  install, with the proxy relaying, saved the snapshot; dwz was then purged.
  With `PUTGD_PKG_STALL=60` and `PUTGD_PKG_ATTEMPTS=2`, both online attempts
  were stopped, and the offline install from the snapshot installed dwz.
- **Every attempt stalls, no snapshot** (job 120602160906, red). With the
  default limits, the four attempts were stopped at 600 s each, with 30, 60 and
  120 s back-off between them. The step failed after 43 min 30 s
  (19:04:03–19:47:33Z), inside the 90-minute timeout that the native package
  jobs use.

Every new path also has a case in `tests/ci/test_install_packages.py`: a
silent stall, an attempt over its time limit with a snapshot fallback, every
attempt stalling without a snapshot, and the acquire options. Each case also
checks that the stalled package manager process is gone.
