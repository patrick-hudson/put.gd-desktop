# Weekly builder stress lane on k72 (#360)

The first runs of `putgd-stress-weekly.service` on k72 (Kubuntu 26.04, four
cores), 2026-10-21 local time. They were started by hand with
`systemctl --user start`, testing the PR head through
`systemctl --user set-environment PUTGD_STRESS_REF=...`, which was unset right
after. Paths are shortened to `~`. The iterations' JUnit and logs, and
`build.log`, stayed on k72.

| Directory | What it shows |
| --- | --- |
| `k72/weekly-2026-10-21/` | The skip path. The service started with `PUTGD_STRESS_LOCK_DIR` pointing at a test copy that held a fake quiet A/V lock. It skipped the week and logged why, in under a second. |
| `k72/weekly-2026-10-21-2/` | Code `fe24932`, before the headless-environment fix. The tests inherited the Plasma session (`DISPLAY=:0`, `WAYLAND_DISPLAY`) from the systemd user manager. Five native and X11 tests timed out in 10 of 10 iterations, at about 360 s per iteration. Kept as the reason for the fix. |
| `k72/weekly-2026-10-21-3/` | Code `3ae60a5`, with the fix. The incremental build took 31 s. Ten iterations of the 74 integration tests ran with four busy loops at time scale 3, about 118 s each. `capture_process_integration` failed 1 of 10 runs (#398). The token wasn't installed, so the run logged that and kept its results. |

`stress-weekly-k72.png` is rendered from these logs and from
`systemctl --user list-timers` on k72.
