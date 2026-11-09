# Removed tests

In November 2026 the test suite was cut to shorten CI and stop flaky tests that
guarded nothing a user would notice. About 21,000 lines of test source and 446
reference images went. This page lists what went, what no longer gets caught,
and what to do if a regression shows up.

The bar for keeping a test was that its failure would otherwise be silent and
costly: data loss, credential exposure, uploads to the wrong place, irreversible
release publication, input to a live desktop, destroyed golden images or false
acceptance evidence. Anything else had to justify the time it cost.

The removals are accepted risk. A regression they let through gets fixed when it
appears, and the fix decides whether a test comes back.

## What went

Each pull request's diff shows the exact tests it removed.

| Pull request | Lines removed | What went | What no longer gets caught |
| --- | ---: | --- | --- |
| [#1324](https://github.com/patrick-hudson/put.gd-desktop/pull/1324) | 591 | `test_build_contract.cpp`, `test_tray_season.cpp`, `test_windows_lab.py`, `test_lab_ui_contract.py`, `test_ui_names.py`, `test_sdk_docs.py` | Seasonal tray date boundaries. Spelling drift between the lab driver and the app's strings. |
| [#1326](https://github.com/patrick-hudson/put.gd-desktop/pull/1326) | 372, plus 446 PNGs | The catalog pixel comparison, `ui_catalog_light`, `ui_catalog_text_150`, `tests/gui/catalog-references/`, `update_catalog_references.py` | Automatic detection of moved, recoloured or clipped controls. Review the rendered catalog by eye. |
| [#1329](https://github.com/patrick-hudson/put.gd-desktop/pull/1329) | 11,969 | Tests of lab tools that nothing runs automatically (listed below) | An edit that breaks one of those tools shows up the next time someone runs it, not at pull request time. |
| [#1330](https://github.com/patrick-hudson/put.gd-desktop/pull/1330) | 5,789 | 456 tests of live lab harness internals across 24 files | Breakage in those mechanics shows as a red or grey nightly desktop cell or a failed manual run. Boat and snapshot breakage appears only when someone uses them. |
| [#1331](https://github.com/patrick-hudson/put.gd-desktop/pull/1331) | 1,379 | GUI paint, spacing, geometry and exact-copy pins; hosted-runner recording rate floors; the positive PCManFM restart cases | Moved or restyled controls. Recording throughput on CI, which physical acceptance still measures. PCManFM failing to restart on LXDE or LXQt. |
| [#1328](https://github.com/patrick-hudson/put.gd-desktop/pull/1328) | 296 | Tests that matched workflow text and release copy; the second run of the surface contracts | A workflow step being reworded or removed, unless a surviving behavioural test notices. |
| [#1335](https://github.com/patrick-hudson/put.gd-desktop/pull/1335) | 448 | Slow timing tests (listed below) | The specific timeouts and grace periods those tests waited out. |

`tests/desktop` lost about half its source. The C++ product tests and
`tests/releases` were mostly kept.

### Dormant-tool test files (#1329)

`test_cover_player.py`, `test_neon_autologin.py`, `test_staging_recovery.py`,
`test_storage_preflight.py`, `test_surface_map.py`, `test_surface_r3_fixtures.py`,
`test_surface_recipes_destinations.py`, `test_surface_recipes_dialogs.py`,
`test_surface_recipes_editor.py`, `test_surface_recipes_events.py`,
`test_surface_recipes_review.py`, `test_surface_recipes_selectors.py`,
`test_surface_recipes_settings.py`, `test_surface_recipes_tray.py`,
`test_surface_recording_bar.py`, `test_surface_tray_fixtures.py`,
`test_surface_tray_input.py`, `test_surface_tray_observations.py`,
`test_tray_progress_lab.py` and `test_wall_vnc_watchdog.py`, all under
`tests/desktop`. The tools themselves were kept.

`test_native_acceptance.py` was reduced to its native keychain ownership and
post-login evidence checks. `test_surface_recipes_history.py` lost 50 tests and
`test_surface_recipes_links.py` lost 51, leaving their wrong-identity, failed-result,
restore and stray-browser checks.

`test_staging_recovery.py` covers code normal lab installs run, unlike the others
on this list. It went because an install failure shows in the nightly check, and
what it guarded was rare races inside a disposable overlay. Nothing else tests
those races.

### Slow timing tests (#1335)

| Test | Unloaded seconds | What it checked |
| --- | ---: | --- |
| Seven `backgroundStartupWaitsForLateTray` rows | 79.8 | Late tray and recovery grace periods |
| Eleven heap-cleanup and preview timing tests in `operations_contract` | 61.8 | Resident memory release cadence and preview admission. Physical memory gates still apply. |
| `recordToggleWaitsForASlowStopThroughTheRealClient` | 16.5 | The real client waiting past a 15 second stop |
| `secondLauncherHasOneDeadlineDespitePartialTraffic`, `secondLauncherWaitsForCompleteDelayedResponse` | 18.6 | Launcher deadlines |
| The 31 second idle wait in the S3 module test | 31.0 | Worker identity after the old thread-pool expiry |
| `successFeedbackWaitsForHoverAndFocusAndFailuresStay` | 19.2 | Editor success and failure feedback timing |
| Seven settings fit tests and the `gui_history_view_text150` run | 23.3 | Layout stability and repeat history behaviour at 150% text |
| `nonresponsiveOwnerKeepsEventLoopAlive` | 14.6 | Responsiveness when a clipboard or capture service stops replying |

## Also changed in CI

[#1336](https://github.com/patrick-hudson/put.gd-desktop/pull/1336) runs
`ui_catalog_render` on a pull request only when a changed path can alter the
catalog. See [ci.md](../release/ci.md). Every push to `nightly` still renders it.
An incomplete render, surface map mismatch or secret-control change that a pull
request skips therefore fails the render or its checks on the next nightly push.
A secret-control mismatch also makes the live gate fail closed until it is fixed.
Purely visual changes are not compared at any stage.

## If a regression shows up

Find the row above that covers it. To bring a test back, take it from the pull
request that removed it, with `git revert` for a whole batch or by copying the
one function. Fix any flakiness before it returns, and keep it only if it meets
the bar at the top of this page.

The flake ledger reopens a closed `flaky-test` issue when a test that still exists
fails and then passes its retry on a run that can write to the tracker. A test that
fails both attempts fails the job instead.
