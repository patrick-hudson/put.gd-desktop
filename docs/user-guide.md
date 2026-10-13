# put.gd user guide

Open put.gd to reach Settings and setup. The tray menu also opens settings, recent activity, and persistent upload history. If the desktop has no tray, use the launcher to reopen settings. Capture actions remain available through the CLI. Desktop support is shown separately for capture targets, shortcuts, clipboard import, and clipboard results; inspect actual bindings and conflicts in Shortcuts and support. A configured shortcut is not proof of a successful binding. Unsupported targets require the documented desktop helper or permission repair.

## First setup

Fresh installations require an explicit destination choice. To save locally, choose **Save locally** and save settings. To change the folder, expand **Advanced settings → Capture → Local files**. Each action can independently use upload or local saving. Local image results copy image MIME, text results copy text, and file copies return local file URIs. Disabling **Keep local copies of uploads** leaves local-only saving active and removes no existing files.

For S3, enter the bucket, region and optional public URL in the initial form, then save. Region suggestions are searchable and accept custom values. Advanced settings holds endpoint and other provider details. **Add S3 destination** creates additional destinations; per-action assignments are under Advanced. **Remove destination** edits the draft only; affected actions need another profile or local saving before the draft can be saved. Removal preserves original history receipts and remote objects. Alternatively, explicitly import the development S3 template, review every destination field, and save. Its development bucket, region, endpoint, prefix, storage class, ACL, and optional public URL base are editable. Select **omit** to send no ACL header. Review each action's upload profile. This template does not provision a bucket, change its policy, or supply credentials. Profiles for other registered providers retain their own JSON settings.

Choose secret-store credentials or explicitly choose session credentials. Save the profile before entering credentials. Use **Store credentials for saved profile**; password fields clear after submission. Session credentials exist only until the resident app exits. Secret-store profiles contain only a reference in config.json; unlock that saved reference explicitly when needed. The destination test runs only when you press its button, uses a synthetic object, and reports success or the actionable error. It does not modify bucket controls. Any uncertain test cleanup must be resolved from the resident diagnostics/test receipt.

Existing file names and auto-rename are the defaults. Captures use names such as `Screen-Capture-2026-10-08-04-27-21.png`. Advanced → Capture → File names offers random names and templates for imported files, screen captures, clipboard images and text. Info icons explain each token. Skip does not upload or copy an unrelated existing URL. Overwrite is an explicit choice: it can change what existing public links serve and requires the checked object's precondition. Local saves preserve existing files and add a suffix on collision. Folder overrides route images, existing video files, text, and other files independently; recording is not included.

## Settings edits and operations

In **Advanced settings → Shortcuts**, choose **Record shortcut** beside an action,
press its key combination, and release all keys to keep it in the draft. Modifier-only
presses wait for a key. **Escape** always cancels recording, even while modifiers
are held; the row's **Cancel** button does the same. Moving focus away or closing
the window also cancels. **Clear** leaves the action unassigned. Use **Save settings**
to apply the draft, or Settings **Cancel** to keep previously saved bindings.
The app pauses its global shortcuts while recording so entering an existing chord
does not start a capture or upload. The status beneath each row reports the actual
desktop binding and any conflict or unsupported key. Agents can still edit the
single-chord PortableText value in JSON, such as `Ctrl+1`.

Settings shows the exact canonical configuration path and accepted revision. All non-secret settings are saved together through the shared validator. Invalid fields keep the accepted configuration active. Clean forms refresh after accepted external edits. An unsaved form preserves its draft and displays a conflict when external configuration is accepted; discard/reload before saving. Pending operations and historical receipts keep their original destination snapshot. See [configuration.md](configuration.md) for the JSON format and cooperative locking conventions.

Preferences controls notifications, launch at login, resource limits, recent activity size, failed artifact retention, preview cache size, and optional history expiry. Blank history retention keeps history indefinitely. Notification/tray/launch-at-login behavior needs real-session acceptance on the declared desktop.

Recent activity shows each operation, its items, upload states, local-copy states, URLs, clipboard outcome, and errors. **Copy result only** does not upload again. **Retry local save only** preserves publication independently. Retained failed/interrupted artifacts can retry through the shared operation service. If another app changes the clipboard while publication is running, its contents stay intact and copying the completed result is a deliberate action.

## Persistent history and deletion

History searches stored names in the local ledger without cloud listing. Filter by dates (ISO timestamps), content type, original destination/profile, upload outcome, local-copy outcome, and remote-deletion outcome. Pages contain at most 50 records by default. Details retain the original receipt and direct URL even when current destination settings change. Available cached image previews are decoded only for the selected entry with file and pixel limits; missing previews show a type fallback.

Copy/open uses the stored direct link. Known removed, already missing, changed, or hidden-with-version-retained links are disabled. Superseded entries retain their original URL with a warning that it may now serve replacement content. Deleting always uses the original receipt rather than adopting a replacement.

Select uploaded history rows and choose remote deletion. The confirmation identifies each exact history ID, original receipt/destination, and provider-specific effect. Canceling confirmation sends no deletion. After acceptance, retain the shown deletion operation ID: per-item outcomes distinguish removed, already missing, changed, failed, unknown, unsupported safe removal, and retained versions. Retry/cancel use that recorded deletion identity; cancel cannot undo a request already sent. Local copies and history records survive remote deletion. Browser/CDN caches may continue serving old bytes.

**Clear history records** clears only the local ledger. **Clean retry artifacts and previews** cleans eligible private artifacts and previews and can remove retry ability. Neither action deletes remote objects or user-owned local files. In-progress deletion identities are retained by the shared history service.

Offscreen widget tests verify form/model/service behavior. They do not establish working desktop hotkeys, tray integration, notifications, launch at login, or clipboard paste into another app; use the real-session lab acceptance instructions for those gates.


Each setting has an info icon: hover for details, or click/focus and activate it to
open readable help. **Save settings** validates and writes the draft, then shows a
success banner. **Cancel** discards unsaved edits and credential input and closes
Settings. **Reload from file** discards the draft and rereads external configuration;
an invalid file leaves the last accepted settings active and shows its error.
