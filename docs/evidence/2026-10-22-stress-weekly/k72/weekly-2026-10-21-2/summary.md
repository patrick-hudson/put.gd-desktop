### Stress: weekly-2026-10-21-2 on kubuntu

| Test | Label | Runs | Failed | Outcome | Where | First failure |
| --- | --- | ---: | ---: | --- | --- | --- |
| `native_background_startup_integration` | integration | 10 | 10 | **failed** | stress on kubuntu: 10 runs, 4 busy loops, scale 3 | `Timeout` |
| `native_region_overlay` | integration | 10 | 10 | **failed** | stress on kubuntu: 10 runs, 4 busy loops, scale 3 | `Timeout` |
| `native_shortcut_rejection_integration` | integration | 10 | 6 | flaky | stress on kubuntu: 10 runs, 4 busy loops, scale 3 | `Timeout` |
| `recording_helper_integration` | integration | 10 | 10 | **failed** | stress on kubuntu: 10 runs, 4 busy loops, scale 3 | `FAIL!  : RecordingHelperContract::productHelperAnswersTheHandshakeAndRefusesCaptureTruthfully() 'acceptedCodes.contains(error.code)' returned FALSE. (recording_` |
| `ui_native_lifecycle_integration` | integration | 10 | 10 | **failed** | stress on kubuntu: 10 runs, 4 busy loops, scale 3 | `Timeout` |
| `x11_isolated` | integration | 10 | 10 | **failed** | stress on kubuntu: 10 runs, 4 busy loops, scale 3 | `Timeout` |

1 flaky, 5 failed, 74 test results.

| Test | Runs | Failed |
| --- | ---: | ---: |
| `native_background_startup_integration` | 10 | 10 |
| `ui_native_lifecycle_integration` | 10 | 10 |
| `x11_isolated` | 10 | 10 |
| `recording_helper_integration` | 10 | 10 |
| `native_region_overlay` | 10 | 10 |
| `native_shortcut_rejection_integration` | 10 | 6 |

Iterations: 10, wall 360.12-370.15 s (median 360.15 s).
