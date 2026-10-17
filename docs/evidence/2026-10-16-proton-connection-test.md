# Proton destination connection test — 2026-10-16

## Problem and cause

Adding Proton hid `testDestination` in both `SettingsWindow` provider visibility
paths. The Make default handler checked `testedDestinations_` regardless of
provider, so Proton always prompted that its connection had not passed a test.
The user clarified that this warning was the observed failure. The resident's
ordinary destination tester in `src/main.cpp` rejects providers other than S3.
Both restrictions also exist in HEAD, independently of the staged S3 changes.

## Change

Show Test connection for Proton and route its setup job through the production
provider's existing `inspectAccount` operation. Require a saved account and folder,
match the returned account, and check the selected top-level folder (or My files
root). Authentication/runtime errors propagate; missing or changed destinations
fail with setup guidance. Success records `testScope: account-folder` and satisfies
the current-session default warning. Feedback and the button tooltip explicitly
exclude upload and sharing verification. The test performs no upload or deletion.
Retesting clears earlier success, success is bound to the exact tested profile,
and stale results do not mark the currently displayed destination as tested.

## Source and isolation

- Base: `af8c035a1364def01be430e97d12f47f92358563` on
  `codex/issue-36-upload-clipboard`, with pre-existing staged S3/settings work.
- Build: `0.1.1-dev.221+gaf8c035a1364.dirty`; this is a working-tree candidate.
- Owned edits: `src/app/SetupService.cpp`, `src/ui/SettingsWindow.cpp`, `src/ui/SettingsWindow.h`,
  `tests/gui/test_settings.cpp`, `docs/proton-drive.md`, and this summary.
- Existing index and unrelated `tools/qemu-grid/` work preserved. No commit,
  push, installation or running-session restart performed.
- Private retained evidence: `work/verification/2026-10-16-proton-connection-test/`.
  `final-identity.json` records final source/binary SHA-256; `build-info.json` records build
  provenance; `checksums.json` indexes logs and the fix-only patch.
- Synthetic account/folder data only; fixture provider responses establish UI and
  setup contracts, not Proton service or graphical desktop acceptance.
- Temporary root `/tmp/pt.ptz62hhh`, private runtime `runtime/`. Preflight checked
  the runtime socket and Qt temporary socket templates (six-byte suffixes):
  43–67 UTF-8 bytes, below 107. GUI fixture dispatch opens no listener.

## Verification

- `cmake --preset dev` and `cmake --build --preset dev`: passed with distro Qt
  6.4.2 shared libraries and the configured official AWS SDK.
- Regression before implementation: hidden-button assertion failed; all five
  unavailable-destination cases also failed (`red.txt`, six expected failures).
- Final focused regression: 10 QtTest outcomes passed, including top-level folder,
  My files root, signed-out, no folder, changed account, missing folder and denied
  authentication (`final-focused-dialog-fixed.txt`). Changing the selected folder requires
  retesting. Successful tests assert no S3 tester call
  and no untested-warning dialog when making Proton the capture default.
- `ctest --preset dev -R
  '^(gui_settings|ui_endpoints_integration|ui_split_services_integration|settings_contract|proton_contract)$'
  --output-junit /tmp/pt.ptz62hhh/targeted.xml`, with TMPDIR and XDG_RUNTIME_DIR
  above: settings, GUI and Proton suites passed. Two Unix-listener suites failed
  because the sandbox denied socket binding; preserve these failures.
- Corrected socket-only command, with local socket access outside the sandbox:
  `ctest --preset dev -R '^(ui_endpoints_integration|ui_split_services_integration)$'
  --output-junit /tmp/pt.ptz62hhh/socket-tests.xml`, with the same environment:
  **2/2 passed**. No production network endpoint or user keychain contacted.
- The complete GUI suite passed after the first final binary link
  (`pre-scope-adjustment-gui.xml`); the exact-settings scope adjustment is
  reverified in `final-gui-dialog-fixed.xml`: **64 QtTest outcomes passed**.
- The changed-folder warning test initially used `QMessageBox::done(Yes)`,
  which did not select the static dialog's Yes button. The test now clicks the
  actual button and proves that the chosen default saves. The first failure is
  retained in `scope-test-dialog-failure.txt` and `final-gui-scope.xml`.
- Fix-only manual review checked provider dispatch, callback lifetime, folder
  identity and test-result scope. No unresolved finding. Automated simplification
  was skipped because all implementation/test files overlap pre-existing edits.
- `git diff --check`: passed.

## Acceptance limits

Current-candidate real Cinnamon functional and memory acceptance is pending;
GNOME/KDE expansion was not started. No live Proton account test was performed.
These fixture/offscreen checks do not establish desktop or service acceptance.
