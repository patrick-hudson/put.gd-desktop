# Verification record

> **Historical record.** This is a dated run record, moved unchanged from [implementation evidence](../../testing/verification.md) on 2026-11-05 (#1222). It describes what that run found for the build it names, and its open or pending statements describe that time. "Above" and "below" refer to its neighbours on the old page, in the order of the [history index](../README.md#verification-run-records). For today, see the [current state](../../testing/verification.md#current-state). Do not use it as a work list.

## Weekly stress lane on k72 (#360) — 2026-10-22

Code: [#364](https://github.com/patrick-hudson/put.gd-desktop/pull/364). How it
works: [CI coverage, weekly run on k72](../../release/ci.md#weekly-run-on-k72) and
[remote builders](../../lab/remote-builders.md#weekly-stress-lane-on-k72). Evidence:
[`records/2026-10-22-stress-weekly/`](https://github.com/patrick-hudson/put.gd-evidence/tree/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-22-stress-weekly).

- **Installed.** `tools/lab/install-stress-weekly.sh` ran on k72 and enabled
  lingering. `systemctl --user list-timers` shows the next run at
  Sun 2026-10-25 03:00:00 CDT.
- **Skip path.** With a fake quiet A/V lock in a test copy, the service skipped
  the week, logged why and wrote a `weekly.json` with status `skipped`. A dry
  run with the same lock reported the same skip.
- **Full run.** At `3ae60a5`, ten iterations of the 74 integration tests ran
  with four busy loops at time scale 3, 117.5–119.9 s each.
  `capture_process_integration` failed once
  ([#398](https://github.com/patrick-hudson/put.gd-desktop/issues/398)). The
  earlier run at `fe24932` reached the live desktop through the inherited
  session environment, which the wrapper and the unit now remove.
- **Token missing.** `~/.config/putgd-stress/github-token` doesn't exist yet.
  The run logged that it wouldn't comment and kept its results; #398 was
  filed by hand from the run's ledger with `flake_ledger.py issues`.
- **Tests.** `tests/ci/test_stress_weekly.py` (19) and
  `tests/ci/test_flake_tools.py` (20) run in the `contracts` job.
