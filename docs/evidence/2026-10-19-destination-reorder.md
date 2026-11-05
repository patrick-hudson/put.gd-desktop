# Destination reorder (issue #59)

Source: PR #126, commit `3cb43d8` (branch `claude/loving-pasteur-7a373d`).

## Suites

- d69 (Debian 13, Qt 6.8.2), scratch build of `a10b83f`: `gui_settings`, `gui_application`,
  `ui_process_integration`, `ui_endpoints_integration`, `ui_control_integration`,
  `settings_contract`, `cli_integration`, `ui_split_services_integration` passed. The MCP suite
  (`tests/integration/test_mcp.py` against the built `putgd-cli`) passed (5 tests).
- f44 (Fedora 44), full scratch build of `3cb43d8`: the same eight suites passed.
- CI on the PR and a full manual CI run (`workflow_dispatch` 39335797505) on `3cb43d8`. The manual run built
  `putgd_0.1.1~dev.496+g3cb43d852f6a_amd64.deb` (ubuntu24.04,
  SHA256 `d77e034e0793cbedb1ecdf97382c06995955a23bac6d3d14a9e07ce514b69e89`).

The offscreen GUI tests cover the move (menu, Alt+Up, the UI command), the selected
destination and its unsaved edits, unchanged routing and Default badge, dropdown order,
restart, and a real `config_conflict` (another writer changes the file before the save).
They prove widget behaviour only.

## Native check: Cinnamon X11 guest

- Host d69 (`192.168.68.69`), QEMU 10.0.13 with KVM, 2 vCPU. A fresh overlay `cin59-f5367f2c`
  of golden `cinnamon-x11` (SHA256 `4a21566d…693da`, `transfer-7dfa9973258d`), shown on the
  display wall throughout.
- Guest: Linux Mint, Cinnamon 6.6.4, Muffin 6.6.1, Qt 6.4.2, kernel 6.14.0-37. The DEB above
  was installed with `lab.py install`; the running `/usr/bin/putgd` matched the install receipt.
- Pointer input is QMP `input-send-event` on the guest's USB tablet, so Qt ran a real XDND
  drag inside the Cinnamon session.

| Step | Result | Screenshot |
| --- | --- | --- |
| Three synthetic S3 destinations, default `development-s3`, clipboard exception `synthetic-third` | Rail shows development, alternate, third | [before](destination-reorder/02-destinations-before-head0.png) |
| Drag row 1 below row 3 | Saved order `synthetic-alternate, synthetic-third, development-s3`; selection stays on development; `captureDefaults` and actions unchanged | [after drag](destination-reorder/04-after-drag-head0.png) |
| Drag it back to the top | Accent drop line with its end dot at the target gap; saved order back to the original | [drop line](destination-reorder/05-mid-drag-indicator-zoom.png) |
| Select alternate, press Alt+Down | Saved `development-s3, synthetic-third, synthetic-alternate`; alternate stays selected | [Alt+Down](destination-reorder/07-after-alt-down-head0.png) |
| ⋯ menu on the last row | **Move up** Alt+Up enabled, **Move down** Alt+Down disabled; Move up then saved the earlier order | [menu](destination-reorder/08-menu-open-head0.png) |
| `putgd-cli ui.moveDestination {"direction":"up","profileId":"synthetic-third"}` | `{"order":[development-s3, synthetic-third, synthetic-alternate],"selected":"synthetic-third","saving":true}`; saved | (private) |
| `settings.reorderProfiles` with a duplicate, a missing ID, a stale revision | `profile_order_invalid` (details list duplicate/missing), `config_conflict`; revision and order unchanged | (CLI output) |
| `settings.reorderProfiles` alternate, development, third; `ui.quit`, start, reopen | Order kept after the restart; Default output still development | [after restart](destination-reorder/12-after-restart-head0.png) |
| Capture → Default capture output → Destination | Dropdown lists alternate, development, third | [dropdown](destination-reorder/14-dropdown-order-head0.png) |

The overlay was retired with guarded cleanup (`status: completed`) using an explicit completion
manifest. The golden hash was unchanged afterwards. All 14 screenshots, the run state, package
receipt and cleanup receipt are kept privately under `work/verification/cin59-f5367f2c/` on
the workstation.

## Follow-ups

- [#133](https://github.com/patrick-hudson/put.gd-desktop/issues/133): the drop line is hidden
  under the drag image when a row is grabbed mid-height (it showed when the row was grabbed
  near its top).
- [#134](https://github.com/patrick-hudson/put.gd-desktop/issues/134): a long destination name
  wraps and hides its state line, including **Default** (predates this change).

Not covered here: GNOME and Plasma (Wayland) drags were not run natively ([#135](https://github.com/patrick-hudson/put.gd-desktop/issues/135)).
