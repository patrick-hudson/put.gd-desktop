# Verification record

> **Historical record.** This is a dated run record, moved unchanged from [implementation evidence](../../testing/verification.md) on 2026-11-05 (#1222). It describes what that run found for the build it names, and its open or pending statements describe that time. "Above" and "below" refer to its neighbours on the old page, in the order of the [history index](../README.md#verification-run-records). For today, see the [current state](../../testing/verification.md#current-state). Do not use it as a work list.

## Flaky-test rethink (#351) — 2026-10-22

Code: [#351](https://github.com/patrick-hudson/put.gd-desktop/pull/351), head
`7ffcf42`, all CI jobs green
([run 39712963621](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/39712963621)).
Policy and tools: [CI coverage, flaky tests](../../release/ci.md#flaky-tests). Evidence:
[`records/2026-10-22-flaky-tests/`](https://github.com/patrick-hudson/put.gd-evidence/tree/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-22-flaky-tests).

- **History.** Every failed `linux` and `sanitizers` job with a failed test from
  2026-10-16 to 2026-10-22, all attempts, re-pulled from the Actions API
  (`ci-failures-2026-10-16-22.csv`): 22 flakes, 2 open non-wait failures (#352,
  #353), 6 real bugs.
- **Stress lane, before and after.** `tools/ci/stress_suite.py`, the twelve
  tests that flaked in CI. Nightly `33c768e`: 6 of 8 runs failed on d69 with the
  tests and four busy loops on two CPUs, 1 of 8 on m74, 6 of 6 on d69 with three
  busy loops on one CPU (`gui_history_view` overran its 5 s budget, "5350 ms
  would have been sufficient"). #351: 0 of 8 on d69 (`4984e80`), 0 of 10 on m74
  (`7ffcf42`), 0 of 6 on d69 (`ea703c6`, timeouts scaled), and the 74
  integration tests 0 of 8 on k72 with four busy loops, twelve at once
  (`c6918fe`). Runs during the work found four races scaled waits alone did
  not fix; all are fixed in #351.
- **Retry and ledger.** On #351 a temporary forced-flaky integration test passed
  its retry, was listed in the job summary and opened tracker issue #355 (run
  39710070705); a forced-flaky `unit` test failed the job without a retry (run
  39711215419). Both were reverted (`0ed3b74`, `f49ffcb`) and #355 closed.
- **Parallelism.** k72, full suite, three runs each: 106.8 s at eight with the
  old pool; with the profiled pool 137.5 s at six, 113.2 s at eight, 97.4 s at
  ten, 83.8 s at twelve. Hosted CTest at twelve: 76.6 s (24.04), 95.0 s
  (26.04), against 107.4 s and 97.8 s at eight.

Not covered: the lane is not scheduled (proposal #360); Python contract suites
(#99, #160 were flakes there) are outside this policy.
