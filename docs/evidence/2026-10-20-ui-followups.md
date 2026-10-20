# UI follow-ups: diagnostics upload, failure text, notification buttons, Auto limits, tooltip (#158, #152, #151, #157, #149, #175)

Source: the round-8 `ui-notify-polish` code PR (branch `claude/ui-followups-round8`).
Builds were scratch builds of that branch on the hosts below, each with a private
profile (its own XDG folders, runtime directory and archive under `/tmp`), synthetic
destinations and no real credentials.

## Suites

- k72 (Kubuntu 26.04, Qt 6.10.2): the full `ctest --preset dev` run on the branch's first
  version passed 84 of 85. The one failure was `operations_contract`
  (`concurrentBatchIsBoundedAndCopiesInSourceOrder`), the race #187 fixed; once the branch
  included #187 it passed. On the final version `native_tray_integration`, `gui_tray`,
  `gui_settings`, `session_client_integration`, `operations_contract` and
  `diagnostics_upload_contract` pass, and so does `tests/desktop/test_tray_progress_lab.py`
  (35 tests).
- The offscreen tests cover the dialog's upload option for a working, a failing and a
  slow destination, the agent commands, the Auto hints, the failure wording, the resident
  hint and self-close on the notification wire, and the tooltip strings. They don't show
  what a desktop draws.

## #158: the package dialog offers the upload (d69, m74)

| What | Result | Picture |
| --- | --- | --- |
| No upload destination (d69, Plasma 6.3) | No checkbox; "Upload unavailable: no upload destination is set up." | [no destination](ui-followups-r8/158-dialog-no-destination-d69.png) |
| A destination without credentials (d69) | "Upload unavailable: Test connection to Synthetic first failed: …", with the destination's own reason | [test failed](ui-followups-r8/158-dialog-test-failed-d69.png) |
| A working destination (m74, Cinnamon 6.6.4, Qt 6.4.2): the repo's offline S3 fixture (`tests/fixtures/s3_server.py`) on a loopback endpoint, with its synthetic keys in session mode | "Also upload to Synthetic fixture and copy a link", ticked, with the sharing warning; the button reads **Save and upload** | [available](ui-followups-r8/158-dialog-available-m74.png) |
| Click **Save and upload** | `ui.diagnostics` `inspect` showed `saving` until `saved` arrived, with `linkCopied: true` and upload state `completed`; the dialog shows the file, "Uploaded to Synthetic fixture" and the link | [uploaded](ui-followups-r8/158-dialog-uploaded-m74.png) |

Before and after: [dialog pair](ui-followups-r8/158-dialog-before-after.png). Before, the dialog
had only an empty slot ([#122 evidence](settings-ui-122/09-f44-package-dialog.png)).

## #152 and #151: failure notifications on Cinnamon (m74)

A synthetic gradient was uploaded through the editor's file source to a fixture
destination holding AWS's published example key, so every upload ended `partial` with
`s3_permission_denied` (HTTP 403) and a local copy.

- Before ([#122 evidence](settings-ui-122/01-m74-failure-notification.png)): "Clipboard: no_result.
  Open recent activity for per-item results."
- After: **"Upload failed: access denied by the destination. A local copy was saved."**
  ([popup](ui-followups-r8/151-152-failure-banner-m74.png); [before and after](ui-followups-r8/152-failure-text-before-after-m74.png)).

Why the buttons disappeared (#151): Cinnamon 6.6.4's notification daemon ignores
`expire_timeout` for its popups. Normal popups hide after `notification-duration` (4 s)
and critical ones stay. Its list expires notifications only with `remove-old`, which is
off. What removed put.gd's notifications was `_onFocusAppChanged`: when a window of the
notifying app takes focus, Cinnamon destroys that app's non-resident notifications. A
`dbus-monitor` trace on m74 showed `NotificationClosed` with reason 2 at the moment focus
returned to the put.gd window. While put.gd kept focus, later failures showed no popup at
all.

With the fix, the same failure is sent with `resident: true` and `expire_timeout` 0:

- The popup stayed after put.gd's window was minimised and focused again, with no
  `NotificationClosed`.
- **Save diagnostics for this upload**, clicked 78 s after the popup appeared, sent
  `ActionInvoked`. put.gd saved the package and called `CloseNotification` (reason 3).
- The "diagnostics saved" notification that followed stayed in Cinnamon's list (no
  `NotificationClosed`) after its popup hid and after put.gd took focus again.
- Its **Show in folder** button in Cinnamon's applet list wasn't clicked (see follow-ups).

The [#151 figure](ui-followups-r8/151-buttons-before-after-m74.png) sets the two traces
side by side. Its "before" trace comes from this branch before the resident hint was
added; that build sent `expire_timeout` 0, while nightly sends 10000, also without the
hint. Cinnamon closes it the same way either way.

GNOME Shell and Plasma are unchanged: put.gd reads their `GetServerInformation` name and
sends them the same 10-second, non-resident notification as before (wire test). On f44
(GNOME Shell 50.0), a failed upload to a destination without keys was sent with
`expire_timeout` 10000 and no `resident` hint, reading "Upload failed: the destination
needs to be set up again in Settings. A local copy was saved."

## #157: the drag cursor on Plasma Wayland (d69, k72) doesn't reproduce

An unselected destination row was dragged below the next one with uinput input, with
frames at the start of the drag, while moving and while holding:

- d69, Plasma 6.3.6, Qt 6.8.2: KWin's closed-hand move cursor in every frame
  ([d69](ui-followups-r8/157-drag-cursor-d69.png)).
- k72, Plasma 6.6.6, Qt 6.10.2, one 1920x1080 output after the 2026-10-20 hardware work:
  the same ([k72](ui-followups-r8/157-drag-cursor-k72.png)).
- `WAYLAND_DEBUG=client` on the UI process shows a clean move. The source offers
  `set_actions(2)`, the target answers `set_actions(2, 2)` and accepts the MIME type, the
  compositor sends `action(2)` to both, and the drop ends in `dnd_drop_performed`.

The drag code hasn't changed since the #122 frames that showed the no-drop cursor
([d69](settings-ui-122/11-d69-drag-unselected-row.png),
[k72](settings-ui-122/15-k72-drag-unselected-row.png)), and the cursor doesn't come back. At
that time k72 had two outputs.

## #149: what Auto resolves to (m74, d69)

[Before and after on m74](ui-followups-r8/149-limits-before-after-m74.png): a nightly build
(`a045d71`) shows only "Auto". This branch's build shows "· no limit (keeps 4 GiB free)"
beside Largest file, Largest batch and Waiting uploads. On d69 the same rows read
"(keeps 2.34 GiB free)" ([Limits](ui-followups-r8/149-limits-auto-d69.png)): that profile's
data folder was on a 15.6 GiB tmpfs, so the reserve is 15% of it. A tooltip explains the
reserve and the destination's maximum. A typed size hides the hint.

## #175: the tray tooltip (m74)

A six-file clipboard batch (synthetic 4 MiB files, as a `text/uri-list`) went to the
fixture, which held every PUT for 40 s, so three files transferred at once. The pointer
hovered the tray icon, found by its green upload fill, and the crop was taken from inside
Cinnamon's tooltip background, verified on the host
([before and after](ui-followups-r8/175-tooltip-before-after-m74.png)):

- Nightly `a045d71`: "Uploading file 4/6: 4 MiB of 4 MiB (100%)": one file of the six.
- This branch: "Uploading: 12 MiB of 12 MiB (100%) · 3 uploading, 1 waiting".

`gui_tray` checks the strings, including that a single transferring file is still named
("Uploading file 1/2: 3 MiB of 4 MiB (75%)").

## Cleanup

Each desktop slot ran under `tools/lab/desktop_lock.py` and was released `--verified-clean`:

- The resident, the fixture, the uinput agent, `dbus-monitor` and any cover window were
  stopped by PID.
- put.gd's own notifications were closed by ID, and `/tmp/r8u` was removed.
- Captures were cropped on the host to the put.gd window or popup and checked there
  (stacking and colour checks) before copying.
- On m74, the clipboard ended empty: the `xclip` that served the batch's uri-list was
  stopped, which also cleared the fixture link that the package upload had copied.
- The nightly comparison build is a scratch tree on m74 (`~/work/put.gd-r8ui-before`).
