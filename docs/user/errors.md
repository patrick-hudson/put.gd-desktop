# Structured failures and recovery

Every service/CLI failure has `code`, a source-free `message` and bounded `details`.
Ordinary diagnostics contain versions/stages/capability status, never source contents,
filenames, history URLs, authorization values or credential references.

| Code/state | User action |
|---|---|
| `output_required` | Choose an upload destination or enable Keep a local copy in General; neither output is currently enabled |
| `source_unsupported` | Inspect native support and choose an available source |
| `window_unavailable` | This compositor offers no window capture; the message names the missing protocol (on Budgie, labwc 0.20 or later and Mir 2.29 or later have it). Capture a region or the desktop instead |
| `recording_unavailable` | `recording.prepare` can't start here; `details.reason` says why (a later request for a recording that has ended returns the same code with no reason). When it is `wlr_capture_unavailable` (Budgie and Hyprland), the compositor does not offer the Wayland protocol the message names, or the connection to it failed: update the compositor, or restart the session if the connection failed |
| `busy_selecting` | Finish or cancel that interactive selection, or wait for the running capture countdown |
| `repeat_unavailable` | Repeat last region has nothing to repeat: no region was captured since put.gd started. Capture a region first |
| `repeat_layout_changed` | The monitors changed (added, removed, moved, resized, rescaled or rotated) since the last region capture. Capture a region again |
| `session_locked` | The screen was locked or blanked when a capture delay ended (the countdown was no longer on screen), so nothing was captured. Unlock and capture again |
| `delay_unavailable` | This desktop can't show the capture countdown (the message says why), so a delayed capture is refused. Capture without a delay |
| `queue_full`, `input_excessive` | Finish queued work or adjust documented resource limits |
| `disk_space_insufficient` | Free up space in put.gd's data folder and try again (the details give `freeBytes`, `reserveBytes` and what was needed; a recording also gives `shortfallBytes`): an import needs its bytes, and a recording three 64 MiB budgets, above the system reserve. Nothing was accepted |
| `source_unavailable` | Fix the reported item index; invoke again; nothing was published |
| `archive_failed` | Fix the original local folder, then retry local save; upload is independent |
| `artifact_missing`, `artifact_changed` | Restore access or invoke a new action; mutable local copies are not retry sources |
| `retry_would_upload` | Finish share link (a retry with `scope` `sharing`) only makes links, and a file of this upload still needs its upload (`details.itemId`). Use Retry upload, which finishes the links too |
| `provider_unavailable` | Restore the adapter/credential access for the recorded destination |
| `unsupported_capability` | Choose a provider/policy that supports the required safeguards |
| `existence_unknown`, `collision_exhausted`, `precondition_changed` | Inspect the saved candidate and retry deliberately |
| `unknown` upload / `publication_unknown` | Reconcile the saved publication identity before another PUT |
| copy `suppressed` / `failed` | Explicitly copy the existing result; no new PUT is needed |
| `delete_confirmation_changed`, `delete_target_changed` | Review the exact original targets/effects again |
| remote `changed` | Replacement is preserved; no destructive request was sent |
| remote `unknown` / `hidden_version_retained` | Explicitly retry the recorded deletion/progress |
| `history_migration`, `history_version` | Preserve the database and restore a supported version; no partial migration is committed |
| `config_invalid`, `config_missing`, `config_too_large` | Fix the original file; accepted settings remain available |
| `config_busy`, `config_conflict`, `config_unwritable` | Reload/rebase or fix filesystem access; prior bytes remain intact |
| `invalid_json`, `invalid_request`, `unsupported_version`, `wrong_session`, `request_too_large` | Correct the versioned session request; no operation was accepted |
| `session_unavailable` | Start the resident app in the same user session |
| `storage_busy` | Quit the other or earlier put.gd process that holds the data folder, then start put.gd again; nothing was moved |
| `storage_migration_failed` | Fix access to the reported folders and start put.gd again; the move resumes from its staging folder and nothing was removed |
| `storage_unavailable` | Make the user configuration and data folders resolvable and writable |
| `sxcu_invalid`, `sxcu_unreadable` | The file is not a ShareX custom uploader, or could not be read; export it again from the service |
| `import_busy` | Another `.sxcu` import is still being saved; import again when it has finished. Nothing of this file was stored |
| `credential_reference_in_use` | The keychain entry this import's ID would use (`putgd/http/<ID>`) already belongs to another destination, or is still being written; choose another ID. Nothing was stored |
| `credential_store_timeout` | The system keychain did not answer an import within two minutes, usually an unanswered prompt; nothing was imported. Unlock the keychain, or import with credentials for this session only |
| `sxcu_unsupported`, `sxcu_unsupported_type` | The message says which part put.gd does not take (a URL shortener, an interactive placeholder, a credential repeated in the link, another method); set the destination up by hand without it ([custom uploader](custom-uploader.md)) |
| `http_settings_invalid`, `http_insecure_url` | Fix the named field of the custom uploader; for a plain `http://` address beyond loopback, turn on **Allow plain HTTP for this destination** or use `https://` |
| `http_text_only`, `http_type_unsupported` | This uploader does not take that kind of file; choose another destination, or change its body type or **Uploads** |
| `http_connect_failed`, `http_upload_interrupted` | Nothing was stored; check the address and the network, then retry |
| `http_authentication_failed` | The server refused the stored values (401/403); enter them again and store them |
| `http_too_large`, `http_rate_limited`, `http_request_refused` | The server refused the upload with a 4xx status, and put.gd records a failure. Its own message follows when the destination has an Error message value |
| `http_server_error` | The server answered 5xx before the request was complete; nothing of it was accepted. put.gd's HTTP client reads no reply while it is still sending, so such a server usually shows as `http_upload_interrupted`. A 5xx after the whole request is `http_outcome_unknown` |
| `http_redirect_refused` | The server redirected to another host, to plain HTTP, or too often; put.gd did not follow. Set the address it named as the request URL if that is right |
| `http_outcome_unknown`, `http_response_unreadable` | The upload was sent and may be on the server; put.gd cannot check. This includes a 5xx that arrived after the whole request was sent and a reply that ended before it was complete (shorter than its `Content-Length`, or a chunked reply without its last chunk). Look on the server, or **Retry upload**, which sends the file again and may leave a second copy. For an unreadable reply, fix the URL value under Response first |
| `http_link_holds_secret` | The server accepted the upload but put a credential of the destination into the link it returned; put.gd neither records nor copies that link, and **Retry upload** would send the file again with the same result. If the named value is not a credential, clear **Secret** on its row (for a value under **Other secret values**, write it back into the address); otherwise the server's reply is at fault ([what counts as a credential](custom-uploader.md#what-counts-as-a-credential)) |
| `http_delete_unsupported` | The upload has no deletion URL put.gd may use: none came back, or the one that did is on another host and would need a stored value, uses plain HTTP the destination does not allow, or points at this computer. History says which; delete it on the server |
| `public_url_unreachable` (custom uploader) | Test connection uploaded the file but could not open the returned link. When the link is plain HTTP and **Allow plain HTTP** is off, put.gd does not open it and says so |
| `http_delete_refused`, `http_delete_unconfirmed` | The server refused the deletion, or answered while the link still serves the file (also when a check of the link was cut short and its bytes match as far as they came); nothing is recorded as deleted. Try again later or delete it on the server |

## Scrolling capture

These come from [scrolling capture](../design/scroll-capture.md). A capture that ends with a
partial image is not a failure: it is the result's `endReason` (`height_cap`, `stopped`,
`lost_track`, `time_limit`, `top_not_reached`) and the image it has is exact and kept.
`lost_track` is also what a change of layout inside the rectangle ends as (the window was resized, a bar appeared at the bottom); the note says "the layout inside the selection changed during the capture; the image ends with the rows from before the change". A column of the selection that scrolls at its own pace (a minimap, a parallax strip) ends it the same way, with the note "a part of the selection scrolls at a pace of its own (a minimap, a column with a parallax effect), so no single image shows it beside the page; the image ends with the rows from before it moved". A live web page can end it too when it lays itself out anew while it is scrolled, or draws its text at another place after a step ([known limits](../design/scroll-capture.md#known-limits)); the rows in the image are exact. `top_not_reached` means the scroll to the top could not be finished, so the image starts
partway down the page; its warning says "Could not reach the top of the page. The image
starts partway down."

| Code | `details` | User action |
|---|---|---|
| `scroll_unavailable` | `reason` | Scrolling capture can't start here, or the desktop stopped giving it what it needs mid-capture. `recording_helper_missing`: this installation has no `putgd-record`, reinstall the package. `injection_unavailable`: the desktop gave no way to turn the wheel (X11 without XTEST; GNOME without Mutter's RemoteDesktop service; Plasma 6.7 or earlier without the `putgd-record` desktop entry that lists `org_kde_kwin_fake_input`, restart the resident after installing it; a wlroots compositor without `zwlr_virtual_pointer_v1`); the message says which. `frame_source_unavailable`: the desktop's native frame stream or a GStreamer element is missing, or the stream ended during the capture. `region_unavailable`: this build has no region selector for the desktop (the Ubuntu 24.04 DEB on Plasma Wayland has none; use the AppImage). Scroll capture is unavailable for every caller there: it is refused with it before anything starts, with a rectangle named through `putgd-cli` or MCP as well as without one, and the window's button reports it. On GNOME the same reason, or `helper_outdated`, means the loaded Shell extension is older than version 22: log out and back in once. The reasons `scroll_not_probed`, `scroll_probe_pending` and `scroll_probe_failed` are transient: try again |
| `scroll_active` | `operationId` | A scrolling capture is already running, or a recording, the editor or another capture wants to start during one. Stop or cancel it first |
| `scroll_no_movement` | `parkPositions` | Nothing under the selection scrolled, at the centre or at the two other places tried. Select a scrollable area (include the page, not only a fixed toolbar) and check that the application takes wheel scrolling |
| `scroll_region` | `rect`, `minimum` | The region spans monitors, lies off the output, or is under 64 px in a direction. Select again inside one monitor |
| `scroll_helper_failed` | `check` | The recording helper died or broke protocol (`exited`, `crashed`, `protocol`, `start_timeout`, `command_timeout`, `build`). Nothing was captured or kept. For `build`, reinstall the package and restart put.gd; otherwise try again and send a [diagnostics package](../design/diagnostics.md) if it repeats |
| `scroll_output_invalid` | none | The resident refused the helper's PNG (wrong size, hash, header or location). Nothing was admitted. Try again |
| `scroll_state`, `scroll_busy`, `scroll_not_found` | none | A `scroll.*` session call that does not fit the session: the command is not valid in its state, another `start`, `select` or `stop` is pending, or a `cancel` is on its way (`cancel` itself is always accepted, but a `stop` whose image was admitted before it arrived has ended the session, so that `cancel` is `scroll_not_found`), or no session has that ID. Inspect the session and send the right command |
| `notification_close_failed` | none | A put.gd notification was on screen and the notification server did not close it after three tries, so the capture was not started (a notice on screen would end up in the image). Dismiss the notification and capture again |
| `capture_timeout` | `check` | `first_frame`: the frame stream sent nothing within 3 seconds. `modifiers_released`: a modifier key stayed down for 10 seconds on X11, so no wheel step was sent. Release the keys and try again |
| `display_changed` | `stage` | The monitors or the size of the captured area changed during the capture. Nothing was kept. (A change of layout inside an area of the same size is `lost_track`, with the rows from before.) Try again |
| `encoding_failed`, `input_excessive` | `stage`, `check` | The spool or the PNG could not be written, or the PNG would pass its size bound. Free disk space in put.gd's data folder, or lower the height cap |

`output_required`, `recording_active`, `editor_active` and `busy_selecting` also mean what
they do elsewhere here. `invalid_request` covers a `scroll.prepare` whose rectangle is
under 64 px or whose `maxHeight` is outside 2,000 to 60,000.

## Text recognition and QR codes

These come from [text recognition](../design/text-recognition.md), which runs in the temporary
`putgd-ocr` helper. Each message is fixed and holds no path or recognized text. `putgd-cli
capabilities` has a `text` block with `available`, the `error` that stops it, `landlockAbi`, the
installed `languages` and the state of each source. It starts no helper. A completed operation that
found nothing is not a failure: it ends as "No text found".

| Code | `details` | Message and user action |
|---|---|---|
| `engine_unavailable` | none | "Text recognition needs the Tesseract English data. Install tesseract-ocr-eng (Debian, Ubuntu, Mint), tesseract-langpack-eng (Fedora) or tesseract-data-eng (Arch)". Install that package. The DEB and RPM require it, so it is missing only after it was removed or when installed without dependencies. The AppImage carries English data and a development build reads the folders in [languages](../design/text-recognition.md#languages-and-where-data-is-read-from). Arch's English data is a larger model, and put.gd loads only the part it needs. A model with no LSTM data is refused as `language_unavailable`. The same code means Tesseract itself failed to start |
| `text_helper_missing` | none | "Text recognition needs the putgd-ocr helper. Reinstall put.gd". `putgd-ocr` is not next to the resident, or is not executable. Reinstall the package or the AppImage. A development build needs `PUTGD_OCR` on and the Tesseract and ZXing-cpp development packages |
| `helper_sandbox_unavailable` | `landlockAbi` in capabilities | "Text recognition is unavailable: this system cannot confine the recognition helper". put.gd refuses to read an image unless the helper can lock itself down first, and it needs the kernel's Landlock at version 3, which is Linux 6.2 or newer with Landlock enabled (`landlockAbi` of 0, 1 or 2 in capabilities means it is not). Update the kernel, or enable Landlock in the kernel's list of security modules (`lsm=` on the boot command line). A container or a security policy that stops a process from restricting itself with seccomp or resource limits gives the same error. Nothing is read and nothing is copied until it is fixed |
| `language_unavailable` | `language` | "Text recognition data for this language is not installed". A language you or a caller named has no data, or its file did not load, or the job named more than three languages or more than 64 MiB of data. Install the language package or copy its `.traineddata` file to `~/.local/share/putgd/tessdata/`, or choose other languages in Settings, Capture, Text recognition. A language chosen in Settings that cannot load is only skipped when another loads, and the notification says so |
| `input_excessive` | none | "The image is too large for text recognition". The image is over 24 megapixels, or a side is over 32,768 pixels, or it is so wide that no band could hold four lines of text. Select a smaller region |
| `text_busy` | none | "Text recognition is still running". Another text operation is running (a selection that is not finished counts). Wait for it, or end it with Escape or `text.cancel`. Nothing queues |
| `text_job_timeout` | none | "Text recognition timed out". The helper did not answer within its 20 second deadline. Select a smaller region. A page with a lot of text normally returns what it read after about 16 seconds instead, marked as cut |
| `text_job_failed` | none | "Text recognition failed". The helper crashed, was stopped, hit a memory or time limit or exited with an error. Try a smaller region, and send a [diagnostics package](../design/diagnostics.md) if it repeats. Nothing was copied |
| `text_job_io` | none | "Text recognition could not read the image or write its result". Try again, with a smaller region if it repeats |
| `text_incompatible` | none | "The text recognition helper does not match this build of put.gd". The installed `putgd-ocr` is from another build or answered out of turn. Let uploads finish, then restart put.gd, and reinstall the package if it stays |
| `text_interrupted` | none | "Text recognition was interrupted; nothing was copied". put.gd quit or crashed while it was reading. It is listed in History as "Text recognition failed" and is never run again. Start Copy text again |
| `edit_stale` | none | "A Redact mark or the crop changed before recognition finished", or "That recognition result is no longer current; inspect the editor again". You or a script changed a Redact mark or the crop, so the old result no longer belongs to the image. Quick redact added nothing. Run it again, and read the editor's `text.result` again before a script copies |
| `editor_finalizing` | none | "The edit was already submitted or discarded". Quick redact or another editor method arrived after the edit ended, so it added nothing. Open the editor again |
| `qr_too_long` | `bytes`, `maximumBytes` | "This link is too long for a QR code". A QR code holds at most 2,331 bytes at the error-correction level put.gd uses. Use a shorter link |

`link_unavailable` with a `reason` of `private`, `revoked`, `removed`, `expired` or `none` is
Show QR code's refusal for a link that cannot be offered. An `expired` link that put.gd cannot refresh
is final. `image_unavailable` means History's Copy text has no retained local copy. `source_unsupported`
for a source of text recognition means it holds no PNG, JPEG or clipboard image, and
`source_unavailable` that a file could not be read.

## GNOME 50 setup states

If GNOME 50 setup reports `login_required`, log out and back in once. Any other
state below has its own step. These states report what's missing
(`result.source.helper.setup` in `putgd-cli capabilities`, `desktop.setup`, the MCP
helper). Capture errors use the code in the second column.

| State | Code | What to do |
|---|---|---|
| `login_required` | `gnome_login_required` (`gnome_extension_updated` after an update) | Log out and back in once |
| `user_extensions_disabled` | `gnome_user_extensions_off` | Turn on **Extensions** in the Extensions app; put.gd's extension starts with them |
| `disabled_by_user` | `gnome_extension_disabled` | Turn the put.gd extension on again in the Extensions app |
| `foreign_user_copy` | `gnome_foreign_extension` | A copy put.gd didn't install is in your user extensions folder (by default `~/.local/share/gnome-shell/extensions/putgd@put.gd`); remove it, start put.gd, then log out and back in once |
| `unsupported_shell` | `gnome_shell_unsupported` | This GNOME Shell isn't 50; use a supported session |
| `settings_locked` | `gnome_settings_locked` | Your administrator has locked GNOME's extension list |
| `settings_unavailable` | `gnome_settings_unavailable` | `gsettings` is missing or failed; the message gives the reason |
| `install_failed` | `gnome_extension_install_failed` | A directory under `~/.local/share/gnome-shell/` isn't writable; the message names it. Logging out won't help until that's fixed; then start put.gd again |
| `not_installed` | `gnome_extension_missing` | Reinstall the put.gd package |
| `extension_blocked` | `gnome_extension_blocked` | Per-user extensions are off on this system; use the DEB or RPM |
| `extension_error` | `gnome_extension_error` | GNOME Shell couldn't start the extension; the message gives its reason |
| `system_copy_outdated` | `gnome_system_extension_outdated` | Update the put.gd package to match this AppImage |

Unknown publication/deletion, failed local save and successful remote upload are
independent states. Cancelling a job cannot promise rollback after a network mutation
was sent. Origin deletion does not invalidate browser/CDN caches or remove local files.

The UI also reports `settings_not_saved` when an action cannot proceed with unsaved
settings, and `credentials_not_stored` when a screen capture would close unfinished
credential entry. The window stays open. Resolve the displayed save failure or
Store/clear the credentials, then start the action again. No action is admitted
by either rejection.
