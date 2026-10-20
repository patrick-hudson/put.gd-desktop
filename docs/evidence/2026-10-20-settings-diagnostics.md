# Settings diagnostics controls and destination drag (issues #122, #133, #134, #135)

Source: branch `claude/elegant-jones-36e25b`. CI package `8412b31` (m74); scratch builds of
`8412b31` plus the fixes in `48662b4` (f44, d69, k72).

## Suites

- d69 (Debian 13, Qt 6.8.2) scratch build: `gui_settings` (70 tests, including the new
  `diagnosticsGroupShowsLogsAndSavesPackages` and `longDestinationLabelKeepsItsStateLine`),
  `gui_history_view` (with `failedUploadSavesDiagnosticsForThatOperation`), `native_tray_integration`
  (with `failedUploadOffersDiagnosticsAndShowInFolder`, under `dbus-run-session`),
  `diagnostics_bundle_contract`, `settings_contract`, `logging_contract`, `ui_endpoints_integration`,
  `ui_process_integration`, `local_ipc_contract`, and the MCP suite (`tests/integration/test_mcp.py`, 5 tests).
- f44 (Fedora 44, Qt 6.11.2) scratch build of `48662b4`: `gui_settings` 70/70.
- CI `workflow_dispatch` run 39357771157 on `8412b31` built every package. Its `linux (26.04)` job
  failed one test outside this change (`recording_service_integration`,
  `operationCancelEndsAnActiveRecording`); the 24.04 and ASan jobs passed it.

The offscreen tests prove widget behaviour only: the group's texts, the dialog's preview, redaction,
remembered folder and saved package, the UI commands, the failed History row action, the
notification buttons and FileManager1 call on a private bus, row elision and the drag image geometry.

## Native check: #122 on Cinnamon X11 (m74)

- Linux Mint 22.3, Cinnamon 6.6.4, Muffin 6.6.1, Nemo 6.6.3, Qt 6.4.2, kernel 6.14.0-37, physical
  desktop shown on the wall. CI DEB `putgd_0.1.1~dev.515+g8412b31671e3_amd64.deb`
  (SHA256 `7884c79520b9cb11f0a4541e959ccbbb9ecd02a8bd98a575cbccbf06aa3a9693`), installed and removed
  in the slot; private profile (own XDG folders and runtime directory).
- Input: `tools/lab/host_input.py` (uinput) clicks; frames from the wall VNC.
- The synthetic destination `synthetic-122` points at a bucket that doesn't exist, with AWS's published
  example key pair in session mode, so S3 rejects every upload. No real credentials were used.

| Step | Result | Screenshot |
| --- | --- | --- |
| Upload a synthetic gradient through the editor's file source | Item `failed` with `s3_permission_denied` (HTTP 403); the failure notification shows **Save diagnostics for this upload** | [failure](settings-ui-122/01-m74-failure-notification.png) |
| Click **Save diagnostics for this upload** | Package for that operation saved in `<archive>/diagnostics`; **put.gd — diagnostics saved** with **Show in folder** | [saved](settings-ui-122/02-m74-saved-notification.png) |
| Click **Show in folder** | Cinnamon sent `ActionInvoked`, put.gd called `FileManager1.ShowItems`, Nemo opened with the package selected | [Nemo](settings-ui-122/03-m74-show-in-folder-nemo.png) |
| Settings → General → Diagnostics | Debug logging, log summary (1 file, oldest record, retention), last unclean exit; Create diagnostics package… | [group](settings-ui-122/04-m74-settings-diagnostics.png) |
| Tick **Debug logging**; open the dialog; Save package; Show in folder | Saved with `debugLogging: true`, expiry shown; package saved and revealed in Nemo | (frames kept privately) |

All five packages saved there were hashed: none contained the example key or secret, the bucket name or
the image's file name; each held the error code and the S3 request IDs. The first dialog frame showed
wrapped text clipped (the dialog didn't grow with its preview); `48662b4` fixes that, checked on f44 and d69.
A click on **Show in folder** about ten seconds after the notification appeared did nothing, because
Cinnamon had already expired it (see follow-ups).

## Native check: #133, #134, #135 on GNOME 50 Wayland (f44)

- Fedora 44, GNOME Shell and Mutter 50.0, Nautilus 50.0, Qt 6.11.2, kernel 6.19.10. Scratch build,
  private profile with three synthetic S3 destinations; the default one has a long name.

| Step | Result | Screenshot |
| --- | --- | --- |
| Destinations rail with the long name | One elided line, then **Default · Ready to test** (#134) | [rail](settings-ui-122/05-f44-long-label.png) |
| Grab row 1 at mid-height, hold over the lower half of row 3 | Drop line below row 3 with its end dot; the slim drag label sits beside the pointer, clear of it (#133); saved order `synthetic-alternate, synthetic-third, development-s3`, selection and Default kept | [drag](settings-ui-122/06-f44-drag-below-row3.png) |
| Alt+Up; ⋯ → **Move up** | Each saved; capture default still `development-s3` | [menu](settings-ui-122/08-f44-menu.png) |
| Drag a row that isn't selected | **Failed on `8412b31`**: the press rebuilt the rows, so no drag started and the selection followed the pointer. Fixed in `48662b4` (rows updated in place); the same drag then worked | [after fix](settings-ui-122/07-f44-drag-unselected-row.png) |
| `ui.quit`, start, Destinations | Order kept | (frame kept privately) |
| `ui.diagnostics` openPackage, savePackage, showInFolder | Dialog shows every line after the fix; Nautilus opened with the package selected; GNOME showed the saved notification | [dialog](settings-ui-122/09-f44-package-dialog.png), [Nautilus](settings-ui-122/10-f44-show-in-folder-nautilus.png) |

## Native check: #135 on Plasma 6.3 Wayland (d69)

- Debian 13, KWin and Plasma 6.3.6, Dolphin 25.04.3, Qt 6.8.2, kernel 6.12.107. Scratch build of `48662b4`.

| Step | Result | Screenshot |
| --- | --- | --- |
| Drag the unselected row 2 below row 3 | Drop line and drag label as on GNOME; saved `development-s3, synthetic-third, synthetic-alternate`. KDE shows its "not allowed" cursor during the drag although the drop is accepted (follow-up) | [drag](settings-ui-122/11-d69-drag-unselected-row.png) |
| Alt+Up; ⋯ → **Move down**; restart | Each saved; order kept after the restart | [after restart](settings-ui-122/12-d69-order-after-restart.png) |
| Create diagnostics package… → Save package; notification **Show in folder** | Full dialog; saved; Plasma's notification button opened Dolphin with the package selected | [saved](settings-ui-122/13-d69-saved-dialog-and-notification.png), [Dolphin](settings-ui-122/14-d69-show-in-folder-dolphin.png) |

## Native check: #135 on Plasma 6.6 Wayland (k72)

- Kubuntu 26.04.1, KWin and Plasma 6.6.6, Qt 6.10.2, kernel 7.0.0-38; two outputs (1920x1080 and
  1422x800). Scratch build of `48662b4`. Patrick's own apps were open on this desktop, and the wall VNC
  served a frozen frame (from before k72's 02:08Z suspend), so frames are Spectacle captures of the put.gd
  window only (active-window shots, or a full shot cropped to the window on the host before copying). A
  temporary KWin script raised the put.gd window and was unloaded.

| Step | Result | Screenshot |
| --- | --- | --- |
| Drag the unselected row 2 below row 3 (twice) | Drop line and drag label; each drop saved; the same "not allowed" cursor as Plasma 6.3 | [drag](settings-ui-122/15-k72-drag-unselected-row.png) |
| `ui.quit`, start, Destinations | Order kept; long name elided with **Default · Ready to test** | [after restart](settings-ui-122/16-k72-order-after-restart.png) |
| Select row 2, Alt+Down | Saved `development-s3, synthetic-third, synthetic-alternate` | [Alt+Down](settings-ui-122/17-k72-alt-down.png) |

## Cleanup

Each slot ran under `tools/lab/desktop_lock.py` and was released `--verified-clean`: resident, input
agent and the file manager windows put.gd opened were stopped; only put.gd's notifications were
dismissed; the m74 DEB was removed; the private profiles under `~/putgd-lab/s122` were deleted. Full
frames and one package are kept privately under `work/s122/` on the workstation.

## Follow-ups

- [#151](https://github.com/patrick-hudson/put.gd-desktop/issues/151): Cinnamon expires the 10-second
  notification and its buttons; a later click does nothing.
- [#152](https://github.com/patrick-hudson/put.gd-desktop/issues/152): the failed-upload notification shows
  a raw clipboard code (`no_result`) and not the failure.
- [#157](https://github.com/patrick-hudson/put.gd-desktop/issues/157): Plasma Wayland shows the no-drop cursor
  during a destination drag, though the drop works.
- [#158](https://github.com/patrick-hudson/put.gd-desktop/issues/158): wire #123's upload option into the
  dialog's reserved slot once #123 has merged.
- The `recording_service_integration` failure on the 26.04 CI job is tracked on the PR; it is outside this
  change.
