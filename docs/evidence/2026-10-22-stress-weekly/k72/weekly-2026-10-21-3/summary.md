### Stress: weekly-2026-10-21-3 on kubuntu

| Test | Label | Runs | Failed | Outcome | Where | First failure |
| --- | --- | ---: | ---: | --- | --- | --- |
| `capture_process_integration` | integration | 10 | 1 | flaky | stress on kubuntu: 10 runs, 4 busy loops, scale 3 | `FAIL!  : CaptureProcessTests::malformedResultsSettleOnce(early-exit) 'QStringList({"helper_exit","begin_send"}).contains(error.details["check"].toString())' ret` |

1 flaky, 0 failed, 74 test results.

| Test | Runs | Failed |
| --- | ---: | ---: |
| `capture_process_integration` | 10 | 1 |

Iterations: 10, wall 117.52-119.88 s (median 118.66 s).
