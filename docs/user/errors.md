# Structured failures and recovery

Every service/CLI failure has `code`, a source-free `message` and bounded `details`.
Ordinary diagnostics contain versions/stages/capability status, never source contents,
filenames, history URLs, authorization values or credential references.

| Code/state | User action |
|---|---|
| `output_required` | Choose an upload destination or enable Keep a local copy in General; neither output is currently enabled |
| `source_unsupported` | Inspect native support and choose an available source |
| `window_unavailable` | This compositor offers no window capture; the message names the missing protocol (on Budgie, labwc 0.20 or later and Mir 2.29 or later have it). Capture a region or the desktop instead |
| `recording_unavailable` | Recording can't start here; `details.reason` says why. When it is `wlr_capture_unavailable` (Budgie and Hyprland), the compositor does not offer the Wayland protocol the message names, or the connection to it failed: update the compositor, or restart the session if the connection failed |
| `busy_selecting` | Finish or cancel that interactive selection |
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
