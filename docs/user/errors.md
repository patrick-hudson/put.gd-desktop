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
| `http_delete_refused`, `http_delete_unconfirmed` | The server refused the deletion, or answered while the link still serves the file or while the check of the link was cut short; nothing is recorded as deleted. Try again later or delete it on the server |

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
