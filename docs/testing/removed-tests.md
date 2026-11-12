# Removed tests

In November 2026 the test suite was cut twice to shorten CI and stop flaky tests
that guarded nothing a user would notice. The first round took about 21,000
lines of test source and 446 reference images, the second about 26,000 lines.
This page lists what went, what no longer gets caught, and what to do if a
regression shows up.

The bar for keeping a test was that its failure would otherwise be silent and
costly: data loss, credential exposure, uploads to the wrong place, irreversible
release publication, input to a live desktop, destroyed golden images or false
acceptance evidence. Anything else had to justify the time it cost.

The removals are accepted risk. A regression they let through gets fixed when it
appears, and the fix decides whether a test comes back.

## What went in the first round

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
`tests/releases` were mostly kept in this round; the [second round](#second-round)
cut the C++ tests too.

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

## Second round

Feature pull requests between 2026-11-08 and 2026-11-12 added about 13,500
lines of tests back. The second round judged those tests, and the older tests in the same files, against
the same bar. From the commit before the first of them was merged (`0d9830c72`)
to the merge of #1423, `tests/` lost 25,985 lines and gained 624, in 89 files.

| Pull request | Lines removed | What went | What no longer gets caught |
| --- | ---: | --- | --- |
| [#1420](https://github.com/patrick-hudson/put.gd-desktop/pull/1420) | 3,429 | Scrolling capture: `scroll_stitcher_contract` went from 257 rows to 10, `scroll_capture_x11` from 16 functions to 2, `scroll_service_integration` from 7 to 3. The page shapes (sticky bars, noise, repeating overlaps, numbered lines, layout changes, live pages, overscroll, slow guests), the X11 image cases, and the state-label rows for Stop, Cancel and a missing selector. | A stitch or control-loop change that gives a wrong image, or ends a capture early, on one of those page shapes. Breakage in the X11 helper's image path. A `holdStart` that releases early. Desktop acceptance captures real pages and judges the stitched image on every desktop. |
| [#1421](https://github.com/patrick-hudson/put.gd-desktop/pull/1421) | 12,350 | GUI tests: 111 of 117 History view functions; the Settings appearance, shortcuts and updates suites and most of the others, with `gui_settings_startup_unlock`; `gui_application`, `gui_version`, `gui_window_buttons`, `gui_theme` and `gui_theme_cinnamon_dconf`; `gui_tray`, `gui_tray_art` and `gui_tray_notification_latency`; `wayland_region_overlay`; the review window's footer, layout and workspace suites; 63 of 65 editor window functions and 46 of 53 functions in `test_editor.cpp`. | History and Settings layout, wording, view states and autosave; copy and retry flows going wrong in the UI; the window controller, About, title-bar button order and theme colours; tray state, art and toast timing; review window layout; the editor canvas's display of redactions (the saved and uploaded image is still checked). |
| [#1422](https://github.com/patrick-hudson/put.gd-desktop/pull/1422) | 6,809 | Storage, recording and jobs: most of the custom uploader's body-type, reply-style, wording, timeout and Test connection cases; the B2 and S3 validation tables, progress, cancellation, lane and Test connection cases; `s3_module_integration` with its fixture modules; Proton transport timing, progress and helper reuse cases; the recording pipeline's rate measurements; recording service, helper and model walk-throughs, `click_overlay_contract` and `kwin_click_effect_contract`; image and editor job walk-throughs; 96 of 120 `operations_contract` cases. | An uploader body type or reply style other than multipart, JSON or header breaking; wording drift; timeout tuning; the resident mapping the AWS SDK; progress and parallelism regressions; slower Proton uploads from lost helper reuse; recording cadence, which physical acceptance measures; capture naming, byte budgets and slot ordering in the operation queue. |
| [#1423](https://github.com/patrick-hudson/put.gd-desktop/pull/1423) | 3,573 | Desktop adapters, capture, tray and Python and JS tests: the pixel, geometry, clipboard, click, region and countdown cases of `budgie_labwc_integration`; most of `labwc_bridge_contract`, `gnome_extension_setup`, `capture_delay_contract`, `bar_placement_contract` (with its single-monitor run) and the scroll notice cases of `native_tray_integration`; walk-throughs in `kwin_transfer_contract`, `gnome_core_transport_integration`, `capture_process_integration`, `kde_ownership`, `x11_core_clipboard` and the GNOME extension's gjs test; 22 lab-recipe pytest cases; `test_ubuntu_mirrors.py`; most of `test_mcp.py` and `test_config_schema.py`. | Wrong pixels, geometry or orientation from labwc capture; clipboard and click-highlight regressions on Budgie; a wrong GNOME extension setup message; delayed capture ordering and validation; a recording bar placed inside the clip or off screen; wrong tray pixels or wording; parity between each MCP tool and its session method. Desktop acceptance drives delayed capture, the recording bar and Budgie capture. |

The pull requests measured the suites they touched before and after:

| Suites | Builder | Before | After |
| --- | --- | ---: | ---: |
| `scroll_stitcher_contract`, `scroll_capture_x11`, `scroll_service_integration` | d69 | 177 s | 20 s |
| GUI and editor tests, offscreen | m74 | about 397 s | about 15 s |
| Storage, recording and job suites (24 CTest entries before, 21 after) | f44 | 267 s | 85 s |
| Desktop adapter suites | k72 | 231 s | 103 s |

[#1426](https://github.com/patrick-hudson/put.gd-desktop/pull/1426) brought back
eight functions that #1421 removed and that meet the bar, each cut down to the
check that matters:

| Test | Functions | What they catch |
| --- | --- | --- |
| `gui_application` | `railActionsFlushSettingsAndHideScreenWindowsBeforeDispatch`, `screenRailActionKeepsUnstoredCredentialsInTheirWindow` | A capture started from Settings that leaves put.gd's windows on screen, so a revealed credential could land in a screenshot that uploads |
| `gui_application` | `explicitQuitFlushesFinishedEditsBeforeStoppingTheResident`, `finalCloseWaitsForPendingAutosaveDuringAcceptedSetup`, `closeRejectionRetainsTheDraftAndAllowsRetry` | Settings edits dropped on quit, on close during a save, or after a refused close |
| `gui_editor_window` | `saveCopyFinishesThePendingCropLikeUpload` | Save Copy writing the uncropped image when a crop was drawn but not applied |
| `gui_settings_capture` | `shortcutRecordingCancellationAndFailuresReleaseSuspension` | Global hotkeys left suspended after a failed or cancelled shortcut recording |
| `gui_tray` | `failuresStayBadgedUntilAcknowledged` | A failed upload losing its mark on the tray icon |

It also brought back seven contrast-ratio checks from `gui_theme` as
`gui_theme_contrast`. That test carries the CTest label `nightly`: it runs on
pushes to `nightly` and not on pull requests. See [ci.md](../release/ci.md).
The rest of `gui_application`, `gui_tray` and `gui_theme` stays removed.

The second round closed eight `flaky-test` issues. #1402
(`scroll_service_integration`), #635 (`gui_history_view`), #1113
(`recording_pipeline_throughput`), #393 (`operations_contract`) and #1416
(`kwin_transfer_contract`) went with the tests that flaked; none was a product
fault. #1318 (`b2_integration`) was a fault in the test fixture and #1417
(`image_job_integration`) a deadline in the test itself; both were fixed in the
tests. #1332 (`budgie_labwc_integration`)
was a product fault: the countdown helper on Budgie and Hyprland could hang in
Qt Wayland's display teardown, so a cancelled or finished countdown was
reported as `delay_unavailable` at its deadline. #1423 fixed the helper and
removed the test; see [capture monitor, delay and repeat](../design/capture-delay.md#the-countdown).

## Also changed in CI

[#1336](https://github.com/patrick-hudson/put.gd-desktop/pull/1336) runs
`ui_catalog_render` on a pull request only when a changed path can alter the
catalog. See [ci.md](../release/ci.md). Every push to `nightly` still renders it.
An incomplete render, surface map mismatch or secret-state mismatch that a pull
request skips therefore fails the render or its checks on the next nightly push.
A secret-state mismatch also makes the live gate fail closed until it is fixed.
Purely visual changes are not compared at any stage.

## If a regression shows up

Find the row in either round that covers it. To bring a test back, take it from
the pull request that removed it, with `git revert` for a whole batch or by
copying the one function. Most second-round removals are functions or rows in
files that still exist; copy those from the pull request's parent commit. Fix any flakiness before it returns, and keep it only if it meets
the bar at the top of this page.

The flake ledger reopens a closed `flaky-test` issue when a test that still exists
fails and then passes its retry on a run that can write to the tracker. A test that
fails both attempts fails the job instead.
