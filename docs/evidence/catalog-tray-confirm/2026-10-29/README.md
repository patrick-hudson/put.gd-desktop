# Tray and confirmation catalog acceptance

Implementation: [PR #738](https://github.com/patrick-hudson/put.gd-desktop/pull/738), with closing references to #688 and #722. The map now renders the real tray menu and confirmation states with per-interaction capture overrides.

Source: `90d1134e3c9264f5cd82080a5f1077dd95538e06`, tree `e2d4bc8227a1beb252f3a574b7c2869a2a3a5ceb`.
Built version: `26.1030.2344-dev+g90d1134e3c92`; source was clean. Nightly (including #704) was merged before the editor follow-ups.

One boat sandbox, `bx_4vhusf6d`, forked from `putgd-build-base` with a 7200-second TTL and no account environment. Hydration was verified before builds. The SSH host key was read through `boat exec` and pinned locally; the private key stayed on the workstation. No cloud credentials or live provider sessions were used.

All captures are synthetic Qt offscreen references from the actual menu/dialog code. They establish widget/catalog behavior and map coverage, not native tray, compositor, physical desktop, or release acceptance.

## Results

- Boat build passed with distro Qt 6.4.2 and official AWS SDK 1.11.907.
- `test_surface_map.py`: 52/52 passed, including v1/v2 coverage and per-state capture tests.
- CTest: 113 successful final verdicts; 112 passed first, and the known `gui_tray` #625 flake passed its one permitted retry. The first failure was the pre-existing local-save retry tooltip case. No test failed twice; no waiver was used for any other final test.
- Final individual test durations also fit the original scale-3 outer guards; see `timing-guard-audit.json`. Operations completed in 175.62 s, Proton in 27.96 s.
- Catalog check passed with 194 sidecars. All tray/confirmation actions have hotspots; both surfaces have zero unmodelled controls/keys.
- Browser validation passed: 5 tray images/26 hotspots; 16 confirmation images/52 hotspots; all tray lightboxes and a hotspot click worked with zero JavaScript errors.
- The required `CI` check passed for code PR #738 on this exact commit (Ubuntu 24.04/26.04, Windows, sanitizer and contract jobs).

## Mappings

| Interaction | Capture(s) | Control selector(s) |
| --- | --- | --- |
| `tray.menu-upload-status` | [`00-tray-menu-uploading`](catalog/00-tray-menu-uploading.png) | `trayUploadStatus` |
| `tray.menu-cancel-upload` | [`00-tray-menu-uploading`](catalog/00-tray-menu-uploading.png) | `trayCancelUpload` |
| `tray.menu-output-submenu` | [`00-tray-menu-idle`](catalog/00-tray-menu-idle.png), [`00-tray-output-submenu`](catalog/00-tray-output-submenu.png) | `trayOutputSummary`, `glob:trayOutput*` |
| `tray.menu-change-output` | [`00-tray-menu-idle`](catalog/00-tray-menu-idle.png) | `trayChangeOutput` |
| `tray.menu-capture-region` | [`00-tray-menu-idle`](catalog/00-tray-menu-idle.png) | `region` |
| `tray.menu-capture-window` | [`00-tray-menu-idle`](catalog/00-tray-menu-idle.png) | `window` |
| `tray.menu-capture-desktop` | [`00-tray-menu-idle`](catalog/00-tray-menu-idle.png) | `desktop` |
| `tray.menu-capture-region-edit` | [`00-tray-menu-idle`](catalog/00-tray-menu-idle.png) | `edit` |
| `tray.menu-edit-sources` | [`00-tray-menu-idle`](catalog/00-tray-menu-idle.png) | `edit-sources` |
| `tray.menu-edit-clipboard` | [`00-tray-edit-submenu`](catalog/00-tray-edit-submenu.png) | `text:Edit clipboard image` |
| `tray.menu-edit-file` | [`00-tray-edit-submenu`](catalog/00-tray-edit-submenu.png) | `text:Edit an image file…` |
| `tray.menu-record` | [`00-tray-menu-idle`](catalog/00-tray-menu-idle.png) | `record` |
| `tray.menu-upload-clipboard` | [`00-tray-menu-idle`](catalog/00-tray-menu-idle.png) | `clipboard` |
| `tray.menu-attention` | [`00-tray-menu-proton-busy`](catalog/00-tray-menu-proton-busy.png) | `trayFailedUploads` |
| `tray.menu-history` | [`00-tray-menu-idle`](catalog/00-tray-menu-idle.png) | `history` |
| `tray.menu-destinations` | [`00-tray-menu-idle`](catalog/00-tray-menu-idle.png) | `destinations` |
| `tray.menu-settings` | [`00-tray-menu-idle`](catalog/00-tray-menu-idle.png) | `preferences` |
| `tray.menu-update-available` | [`00-tray-menu-idle`](catalog/00-tray-menu-idle.png) | `update` |
| `tray.menu-about` | [`00-tray-menu-idle`](catalog/00-tray-menu-idle.png) | `text:About put.gd` |
| `tray.menu-quit` | [`00-tray-menu-idle`](catalog/00-tray-menu-idle.png) | `text:Quit put.gd` |
| `confirm.editor-discard` | [`19-editor-discard-confirm`](catalog/19-editor-discard-confirm.png) | `confirmDiscard`, `text:Keep editing` |
| `confirm.review-discard` | [`41-review-discard-confirm`](catalog/41-review-discard-confirm.png) | `confirmDiscard`, `text:Keep reviewing` |
| `confirm.history-delete` | [`42-history-delete-confirmation`](catalog/42-history-delete-confirmation.png), [`confirm-history-delete-supported`](catalog/confirm-history-delete-supported.png) | `deleteConfirmationDetailsExpanded`, `confirmCancel`, `confirmActionButton` |
| `confirm.history-cancel-all` | [`history-cancel-all-confirmation`](catalog/history-cancel-all-confirmation.png) | `text-glob:Cancel * uploads`, `text:Keep uploading` |
| `confirm.history-clear` | [`confirm-clear-history`](catalog/confirm-clear-history.png), [`confirm-clear-cache`](catalog/confirm-clear-cache.png) | `confirmDiscard`, `text:Cancel` |
| `confirm.settings-reload` | [`53-reload-confirm`](catalog/53-reload-confirm.png) | `confirmDiscard`, `text:Cancel` |
| `confirm.reset-limits` | [`confirm-reset-limits`](catalog/confirm-reset-limits.png) | `confirmDiscard`, `text:Cancel` |
| `confirm.clear-logs` | [`confirm-clear-logs`](catalog/confirm-clear-logs.png) | `confirmDiscard`, `text:Cancel` |
| `confirm.remove-destination` | [`confirm-remove-destination`](catalog/confirm-remove-destination.png) | `confirmDiscard`, `text:Cancel` |
| `confirm.make-default-untested` | [`confirm-make-default-untested`](catalog/confirm-make-default-untested.png) | `confirmDiscard`, `text:Cancel` |
| `confirm.switch-older` | [`confirm-switch-older`](catalog/confirm-switch-older.png) | `confirmDiscard`, `text:Cancel` |
| `confirm.revoke-link` | [`confirm-revoke-link`](catalog/confirm-revoke-link.png) | `confirmDiscard`, `text:Cancel` |
| `confirm.add-destination-name` | [`confirm-add-destination-name`](catalog/confirm-add-destination-name.png), [`confirm-new-proton-folder`](catalog/confirm-new-proton-folder.png) | `class:QLineEdit`, `class:QPushButton` |
| `confirm.message` | [`19-editor-discard-confirm`](catalog/19-editor-discard-confirm.png), [`41-review-discard-confirm`](catalog/41-review-discard-confirm.png), [`53-reload-confirm`](catalog/53-reload-confirm.png), [`confirm-make-default-untested`](catalog/confirm-make-default-untested.png), [`confirm-remove-destination`](catalog/confirm-remove-destination.png), [`confirm-reset-limits`](catalog/confirm-reset-limits.png), [`confirm-clear-logs`](catalog/confirm-clear-logs.png), [`confirm-clear-history`](catalog/confirm-clear-history.png), [`confirm-clear-cache`](catalog/confirm-clear-cache.png), [`confirm-switch-older`](catalog/confirm-switch-older.png), [`confirm-revoke-link`](catalog/confirm-revoke-link.png) | `glob:qt_msgbox_*label` |

Each linked image has an adjacent `.controls.json` file. `catalog-validation.json` records every resolved hotspot and any unpictured interactions.

Tray icon states, tooltips, native activation/scroll gestures, native shortcut suffix behavior and the fallback implementation state remain unpictured. Confirmation Escape behavior is a key strip; the general in-app notices entry has no single dialog capture. No captured tray/confirmation controls are intentionally unmodelled.

## Reproduction

The configure/build, short socket-path preflight, unprivileged CTest, catalog and map commands are recorded below. `editor-dialog-probe/` retains the temporary lookup-failure patch, failing QTest log, elapsed-time result and restored source status. The deliberately missing lookup failed with exit 1 in 0.164 seconds, below its 15-second guard. This negative probe used `007679e3`; the editor test file is byte-identical in the final commit (Git blob `40afba30e96b28c727a418966b92870910abe27e`). The source was restored before the final full suite.

The 29b capture dismisses the preparation failure through `editor.view keepEditing`, dismisses saved-copy feedback, restores the normal destination, and asserts the clean state before saving. Its inventory retains `recentColorSwatch_123456` and `recentColorSwatch_abcdef`.

Preliminary evidence is retained privately: a first complete catalog at `f4f89bf3`, an initial Qt built-in-label selector correction, a launch blocked by home-directory traversal permissions, and the superseded/interrupted `6c03bbd6` campaign. None is presented as final-commit acceptance.

## Saved visuals

[Tray surface in the map](tray-surface-map.png) · [confirmation surface in the map](confirm-surface-map.png) · [clean recent colours](catalog/29b-editor-recent-colours.png).

The two map screenshots are direct browser captures, with the entire card positioned below the sticky header. No image editing was used. First clipped capture attempts are retained in the private archive. Browser checks exercised all tray lightboxes and a hotspot click with no JavaScript errors.

The 5 tray states contain 26 hotspot rectangles for all 20 menu interactions. The 16 confirmation states contain 52 rectangles for all 13 confirmation actions plus the message-text interaction. There are no unmodelled controls or keys on either surface. The catalog check validates 194 sidecars. Its five editor-only “control in another state” notices remain legacy-compatible and are outside this mapping change.

Rendering change: an interaction whose selector matches several controls now gets a hotspot on every match, not only the first. This applies to models without `capture` too. It only adds hotspots. An independent check rendered nightly's `model.json` with nightly's and this branch's `surface_map.py`. Coverage output was identical, and six surfaces gained hotspots: destinations 15→17, settings-general 7→8, settings-capture 11→13, settings-shortcuts 5→20, editor 22→30, review 16→20. The coordinator accepted this as an improvement.

## Runtime parameters and earlier attempts

The boat host measured 0.106 seconds per fsync during concurrent tests. Earlier campaigns at outer timeout scale 3 exhausted the 90-second Proton and 360-second operations guards. A subsequent concurrent operations run completed 125 cases but failed the pre-existing next-slot fairness assertion. That exact fairness case passed unchanged in isolation in 2.471 seconds. These original logs and reports remain in the private archive; they are not represented as passing runs.

The final campaign uses serial CTest, `PUTGD_TEST_TIMEOUT_SCALE=10` for outer hang guards and `PUTGD_TEST_TIME_SCALE=3` for asynchronous waits. No assertion, product timing contract, socket ownership check, or frozen performance harness was modified. The missing-dialog probe retained its separate 15-second guard.

```sh
export PATH=/usr/bin:$PATH
cmake --preset dev -DPUTGD_TEST_TIMEOUT_SCALE=10
cmake --build --preset dev --parallel 4
python3 tests/desktop/test_surface_map.py
# New, short, private runtime/TMPDIR values; run ctest_preflight.py and
# check_socket_paths.py before launching the unprivileged test user.
PUTGD_TEST_TIME_SCALE=3 python3 tools/ci/ctest_retry.py \
  --ledger "$PWD/build/dev/catalog-flake-ledger.json" \
  --junit "$PWD/build/dev/catalog-ctest.xml" -- --preset dev --parallel 1 \
  --resource-spec-file "$PWD/tools/ci/ctest-resources.json"
# Catalog: QT_QPA_PLATFORM=offscreen; DISPLAY/XAUTHORITY/WAYLAND_DISPLAY unset;
# XDG_RUNTIME_DIR=/tmp/pcr-r3 (owned/private, checked at 30 UTF-8 bytes).
build/dev/putgd-ui-catalog /tmp/pct-r3
python3 tools/lab/surface_map/surface_map.py check --catalog /tmp/pct-r3
python3 tools/lab/surface_map/surface_map.py build --catalog /tmp/pct-r3 \
  --output /home/user/catalog-evidence/map --embed
```

The final JUnit, retry ledger, registered-test list, socket preflight, host/build identity and binary hashes accompany this record. The full private restore archive also retains the earlier attempts and partial catalogs. Source and evidence checksums were verified before deleting the disposable sandbox; the named base snapshot was preserved.

## Review resolutions

The deletion-action capture now uses explicit synthetic S3 receipts; the existing Proton refusal capture is retained separately. The affirmative button must exist before the capture is saved. Message-label inventories preserve real focusability, with `label` used when a QMessageBox label has no focus; regression tests exercise both policies. Generated menu identities now assert their expected prefix.

The suggestion to accept `Undefined` for cleared optional editor state was not applied: [Qt's constructor](https://doc.qt.io/qt-6/qjsonvalue.html#QJsonValue) is `QJsonValue(Type = Null)`, and `viewState()` always emits these fields. The stronger check passed in the final catalog; Ubuntu 26.04 CI also completed that capture. A missing state key should fail the assertion.
