# put.gd user guide

Open put.gd to reach one window with **History**, **Destinations**, and **Settings**. The tray's Settings, History, and Activity commands open that window; Activity opens History with current work. Closing the window keeps the tray and accepted work running. The launcher reopens it. The tray pane and its capture commands are unchanged.

The left rail starts **Capture region**, **Capture window**, **Capture desktop**, **Upload clipboard**, or **Record screen** after finished settings edits are saved. The tray lists the same captures in this order: Capture Region, Capture Region & Edit, Capture Window, Capture Desktop, Capture Screen Recording, then Edit Image (from the clipboard or a file) and Upload Clipboard. Screen captures close this window to keep it out of the capture. Clipboard imports keep it open. A save failure keeps the window open; retry saving and start the action again. Store or clear unfinished credential entry before a screen capture. Region capture supports Escape to cancel. Window capture targets the focused external app where supported; desktop capture includes enabled monitors and panels. Clipboard import prefers copied local files, then an image, then plain text. Text URLs are imported as text. **Edit & upload** selects a region and opens a small editor first; see [Edit before uploading](#edit-before-uploading). The information button and action tooltips explain the selected action and its saved output. Capture is also available through global shortcuts and the CLI.

## Choose outputs

In **Settings → Capture**, select the default upload destination or **No destination (uploads off)**. Expand **Per-action exceptions** to choose an output for a particular action. **Use default** follows future default changes; an explicit exception keeps its own destination and sharing choice. Shortcuts remain independent.

In **Settings → General → Local copies**, **Keep a local copy** controls local saving independently of the upload destination. Set the main folder and optional image, video, text, and other-file folders there. Recording is not included; video files must already be finalized.

| Destination | Keep a local copy | Result |
| --- | --- | --- |
| Selected | Off | Upload only |
| Selected | On | Save locally and upload |
| None | On | Save locally only |
| None | Off | Report an output error before capturing or importing |

Local image results copy image MIME, text results copy text, and file results use local file URIs. Private uploads create no share link and do not change the clipboard. An upload failure does not turn an upload-only action into local saving; local-save and upload recovery remain separate. Existing files, accepted operations, and historical receipts are preserved when outputs change.

## Edit before uploading

Choose **Capture region & edit** from the rail or tray, or assign it a key in **Settings → Shortcuts** (it has no default key). Drag the region to edit; Escape cancels. To edit something you already have, use **Edit image** at the top of History (the clipboard image or a PNG/JPEG file), right-click the rail button, or drop a PNG or JPEG on it. **Edit again** on a History item reopens its local copy when **Keep a local copy** was on; the edited result is uploaded as a new item. In the editor:

- **Redact** (X) covers an area with opaque black and removes those pixels from the uploaded image. Use it for passwords, tokens, names and other private information.
- **Blur** (B) is only a visual effect; it can leave enough detail to guess text. Prefer Redact for anything private.
- **Crop** (C): drag the area to keep, then **Apply crop** or Enter; Escape cancels.
- **Arrow** (A), **Rectangle** (R), **Highlight** (H) and **Text** (T) add simple marks. Select (V) moves, resizes or deletes them.
- Ctrl+Z / Ctrl+Shift+Z undo and redo; Ctrl+1 shows 100% to check edges; Ctrl+0 fits.

**Upload** (Ctrl+Enter) sends only the edited image to the output shown at the bottom of the window, with the usual local copy, history and link copying. Plain Enter never uploads. **Save a copy…** writes the same edited PNG to a file. **Cancel** or closing the window discards the edit: nothing is saved or uploaded. Inputs above 24 megapixels are refused; output is PNG. An edit interrupted by quitting put.gd is listed as interrupted and is never uploaded automatically.

## Record the screen

Choose **Record screen** on the rail, **Capture Screen Recording** in the tray, or assign the record shortcut in **Settings → Shortcuts**. Drag the area to record on X11, Plasma and GNOME (GNOME needs the put.gd Shell extension for this); other Wayland desktops ask once which screen or window to share. The floating bar chooses MP4, WebM, GIF or WebP and system audio, then records, pauses and stops. Stop always opens a private review: trim, add blur or solid covers for part of the clip, play it back, **Save copy…**, **Discard** or **Upload**. Nothing is saved or uploaded until you choose one. See [screen recording](recording.md) for the details.

Recording needs an H.264 encoder. When none works, put.gd says so and what to install instead of starting:

- **Ubuntu, Kubuntu, Mint and Debian packages** bring one with their GStreamer dependencies.
- **Fedora**: Fedora installs a placeholder OpenH264 library. Replace it with Cisco's: `sudo dnf install openh264 gstreamer1-plugin-openh264`. If the `fedora-cisco-openh264` repository was turned off, first run `sudo dnf config-manager setopt fedora-cisco-openh264.enabled=1`.
- **AppImage**: it carries its own recorder. It encodes with your graphics driver where it can (VA-API or NVIDIA) and otherwise with its bundled x264 and x265, so it needs no setup.

## Destinations and credentials

Use **Add destination** to configure S3 or Proton Drive. Valid non-secret fields save automatically. Removing a destination removes it from settings and clears affected output references; original receipts, local files, and remote objects remain. Review the default and exceptions after removal.

To change the order of destinations, drag one up or down the list; a line shows where it will land. From the keyboard, select a destination and press Alt+Up or Alt+Down, or use **Move up** / **Move down** in its ⋯ menu. The new order saves right away and is also the order of the Destination dropdowns. It changes nothing else: the default, per-action exceptions and history stay with the same destinations, and unsaved edits stay with the destination you were editing. If the settings file changed elsewhere, the previous order comes back and Settings asks you to reload.

For S3, enter the bucket and region, then review the link domain and public-read/omit-ACL choice. Searchable regions accept custom values. **Advanced connection settings** contains endpoints, addressing, storage class, and transport limits. The development template is an explicit import, supplies no credentials, and changes no bucket policy. **Test connection** runs only when pressed, uses synthetic content, and shows progress and its result below the destination title. Success means the test upload, public-link verification and cleanup all passed. Errors give the next checks for credentials, permissions, destination settings or connectivity; a denied object check can be ambiguous. Resolve uncertain cleanup through its retained test identity.

S3 credentials require an explicit **Store credentials** action. Choose the system keychain or this session only; session credentials expire when the resident exits. Configuration contains references rather than secret values. By default put.gd reads saved keychain credentials once when it starts (**Settings → General → Credentials**); if your desktop keyring is locked, it shows its own unlock prompt then. Turn that off to unlock only with **Unlock saved credentials**. Uploads and shortcuts never unlock the keyring. After loading, the form shows only the first four access-key ID characters and whether the secret/token exists; short IDs are fully masked. **Show** beside the secret access key reveals either your typed value or the unlocked saved secret. If saved credentials are not loaded, choose **Unlock saved credentials** first. A revealed saved secret is read-only; **Hide** clears it so you can enter a replacement. Changing destination or provider, leaving Destinations, closing, and reloading hide saved secrets. Store, ordinary close, and reload clear credential inputs.

For Proton, choose **Connect account…** and finish sign-in in the browser. The connected email stays visible when switching destinations. Choose a top-level My files folder, create a new folder, or press **Refresh folders** beside the selector. Folder queries show **Loading folders…** and preserve the known email and selection. Account setup never uploads a file. Configure private/shared output and advanced new-link expiration/password settings as described in [Proton Drive](proton-drive.md). Proton links open a share page; they are not direct image-embedding URLs. Safe remote deletion and account sign-out are not available in this integration.

## Settings and autosave

Settings save automatically after a finished field edit, toggle, or selection. Focusing and leaving an unchanged value does not save. The footer explains autosave without routine success banners. Invalid fields show specific feedback while accepted settings stay active; independently valid changes may still save. A write failure keeps the draft and offers **Retry saving**, rather than replaying the request automatically.

Accepted external changes refresh a clean form. A dirty draft instead reports that settings changed elsewhere and offers reload. **Settings → Advanced → Config file → Reload from file** discards the draft and rereads configuration; an invalid external file leaves the last accepted settings active. The config section shows the canonical path and accepted revision. See [configuration.md](configuration.md) for fields, units, and cooperative locking.

**Settings → Capture → File names** keeps the original names of copied files by default, or uses random names/custom templates. Capture, clipboard-image, clipboard-text, and copied-and-uploaded-file (**Copied & uploaded files**) patterns have separate help. Local collisions preserve files by adding a suffix. Remote collision policy supports rename, skip, or explicit guarded overwrite where the provider supports it; skip never copies an unrelated existing URL.

**Settings → Shortcuts** records one combination per action. Press Record, enter a chord, then release all keys. Escape cancels and Clear removes the assignment. Moving focus or closing also cancels recording. Global shortcuts are paused during recording so an existing chord cannot trigger a capture. Accepted changes save automatically. Each row reports actual binding, conflicts, and unsupported keys; configured text alone does not prove a binding. CLI callers use Qt PortableText such as `Ctrl+1`.

General contains notifications, launch at login, Appearance (Match the desktop, Dark or Light, applied as soon as you pick it; see [appearance](appearance.md)) and Local copies. Advanced contains upload/image limits, queued-image memory, thumbnail cache and retention controls, **Diagnostics** (debug logging, logs, Create diagnostics package…; see [diagnostics](diagnostics.md)) directly above Config file, plus desktop support details. **Reset all to defaults** resets only the limits. Blank history retention means forever. Each setting has keyboard-accessible help. Tray, notifications, launch at login, capture, clipboard and shortcuts depend on the actual desktop support reported there.

## History and recovery

History combines current operations and persistent file receipts in list or gallery view. Search local stored names, expand filters for dates/content/destination/upload/local/remote status, and select a file to inspect its retained result. Browsing does not list or download cloud files. Previews use bounded retained images; videos show a still with a play glyph and their length, made after the upload finishes (see [screen recording](recording.md#history-poster-frames)). Selecting a file whose preview is missing (an older video, a failed poster or one cleanup removed) makes it from the retained file or an unchanged local copy; when neither is left, the inspector says "Preview unavailable" and the list keeps a type fallback. The default 50-artifact page can split a large batch across pages.

S3 progress uses body bytes and a matching total. Proton displays encrypted transfer bytes separately from source size because overhead and retries make a percentage misleading. Counters can go backward during retries. A complete counter can still be finalizing; the confirmed receipt establishes completion.

Copy/open uses the retained result. Copying a result does not upload again. Retry local save preserves publication independently; upload retry uses retained failed/interrupted artifacts and original receipt identity. A failure that stored nothing, such as an image too large to edit, is labelled **Edit failed**, **Upload failed** (a clipboard upload) or **Capture failed** with its reason and offers no retry; start a new capture instead. Accepted work continues after the UI closes. Image copying and Save as use the retained original, never a thumbnail or an unannounced remote download.

**Settings → General → After uploading** chooses individual links as they finish (default), or one ordered newline-separated list after overlapping uploads settle. Failed, cancelled, private, removed, and revoked results add no new links. A manual clipboard change or explicit History copy supersedes pending automatic writes; links remain available for deliberate copy. Partial copying remains a partial clipboard result.

For supported providers, remote deletion first prepares the exact selected history IDs, original destinations, and provider effects. Review that confirmation before accepting. Cancel sends no deletion. Retain the deletion operation ID for per-item removed, missing, changed, failed, unknown, unsupported, or retained-version outcomes. Cancellation cannot undo an already sent request. Local files and history survive remote deletion; caches may continue serving older bytes. Proton offers link creation/inspection/revocation, preserving the file and existing member access, but rejects unsafe remote deletion.

History maintenance clears local records or eligible retry artifacts/previews with confirmation. Neither deletes remote objects or user-owned local files. There is no single-record local-history removal endpoint.

Hovering the unchanged tray shows active work without opening the window: blue for capture/preparation/queue waits, green for storage activity, and a brief red/blue flash for genuine errors. Cancelling capture does not flash an error. The tooltip shows up to three active operations and an additional count. Closing all UI views releases the helper while the resident and tray remain.

Offscreen tests prove widget/core behavior only. Native desktop acceptance uses the installed-package lab, independent paste, actual capture bytes, and visible console witnesses described in [testing](testing.md).
