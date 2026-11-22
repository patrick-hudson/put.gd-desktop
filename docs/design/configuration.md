# Configuration

The resident app owns one `config.json` in its configuration folder. GUI and CLI
report the exact path. No separate YAML file or preferences database exists.
The schema is [config.schema.json](../../schemas/config.schema.json). The shared
`SettingsService` is the authoritative validator, including registered provider rules,
absolute paths, hotkey syntax, and cross-profile references.

## Locations

| Folder | Linux |
| --- | --- |
| Configuration: `config.json` and its `config.json.lock` | `$XDG_CONFIG_HOME/putgd`, normally `~/.config/putgd` |
| Data: `history.sqlite`, `artifacts/`, `previews/`, `destination-tests/`, `destination-status.json`, editor and recording staging, `resident.lock` | `$XDG_DATA_HOME/putgd`, normally `~/.local/share/putgd` |

These are Qt's `GenericConfigLocation` and `GenericDataLocation` plus `putgd`, so they
do not depend on Qt's organization or application name. The screen-recording helper
keeps its portal restore token in `putgd-record/` inside the data folder
([recording](recording.md)). The resident makes both folders owner-only.

Releases up to 0.1.1 and earlier nightlies used Qt's app-scoped locations with the
organization and application both named `putgd`, which doubled the name:
`~/.config/putgd/putgd/config.json` and `~/.local/share/putgd/putgd/`. The resident
moves that layout at startup, after a second launch has handed off to a running
resident and before settings or the journal open. Each folder is decided on its own:

- If the new folder is not yet in use (configuration: no `config.json`; data: no
  `history.sqlite`), the earlier folder is renamed to `.putgd-migration` inside the new
  folder, each entry is renamed into place, and the emptied staging folder is removed.
  Every step is a rename on one filesystem; nothing is copied or replaced. If put.gd
  stops part way, the next start finishes the move before opening anything.
- If the new folder is already in use, neither folder changes. put.gd uses the new
  one and reports the earlier folder as `legacyPath`.
- An earlier folder that is a symbolic link, or that cannot be renamed, stays in use
  (`legacy_active`). Move its contents yourself while put.gd is stopped.
- A staged entry whose name already exists in the new folder stays staged
  (`conflict`, with `stagingPath` and `conflicts`); nothing is replaced.
- While an earlier resident still holds its journal lock, startup stops with
  `storage_busy` and nothing moves. A stale lock from a crashed resident is released.

Journal records store absolute artifact and preview paths. When the journal has moved
with its folder, it reads them under the folder it is in now, so retained artifacts,
previews and queued uploads keep working.

`diagnostics.locations` reports `config` and `data`, each with `path` (the folder in
use) and `state`: `current`, `migrated`, `legacy_retained`, `legacy_active` or
`conflict`, plus `legacyPath`, `stagingPath`, `conflicts` or `reason` when they apply.
The Settings window shows the same diagnostics and the configuration file path.

## Launch at login and uninstall (Linux)

**Settings → General → Launch at login** is off by default. Enabling it writes
put.gd's managed entry at `$XDG_CONFIG_HOME/autostart/putgd.desktop` (normally
`~/.config/autostart/putgd.desktop`) and a managed startup script at
`$XDG_DATA_HOME/putgd/login/start.sh`.

After this version has run with Launch at login enabled, uninstalling the
DEB/RPM or deleting the last matching AppImage removes these managed startup
files while preserving configuration and user data. For an
active account, a user-session file watch performs cleanup even if put.gd has
been quit, after a short settling delay of about two seconds. An inactive
account removes the files at its next login. Package scripts do not search or
delete files in user homes.

Older managed entries are migrated when the updated app runs in that account.
An account that has never run the updated version can still have a legacy entry
pointing directly at the executable; that entry cannot clean itself after the
executable is removed. Run the updated app in each such account before removing
it, or disable Launch at login there first. No package hook migrates inactive
accounts by accessing their homes.

The saved **Launch at login** preference remains unchanged. After reinstalling,
the first manual start reads that preference and recreates startup integration
if it is still enabled. Removing the link does not delete history, local
captures or saved credentials.

For an AppImage, startup uses the persistent script rather than a temporary
mount path. An eligible same-channel replacement beside the configured image
keeps startup working. When none remains, cleanup exits quietly. An image that
still exists but is not executable reports an error if no eligible replacement
can start.

To turn the saved preference off and remove startup integration before
uninstalling, use **Launch at login** while put.gd is running. The CLI uses the
same saved preference:

```sh
putgd-cli settings.get
putgd-cli settings.patch '{"patch":{"preferences":{"launchAtLogin":false}},"expectedRevision":"RETURNED-OBSERVED-REVISION"}'
putgd-cli ui.snapshot
```

Use `result.metadata.observedRevision` from `settings.get` for the patch. Startup
integration is applied asynchronously after saving: wait until
`ui.snapshot` reports `result.diagnostics.loginStartup` with `enabled: false`
and `applied: true`. If it reports an error, correct it before uninstalling.
The MCP equivalent is `patch_configuration` with
`patch: {"preferences":{"launchAtLogin":false}}` and the current
`expected_revision`. put.gd removes only its managed files; a user-created entry
is preserved and reported as `autostart_conflict`.

This contract, including active-account and next-login cleanup, was approved in
[#382](https://github.com/patrick-hudson/put.gd-desktop/issues/382#issuecomment-6379948629).

## Settings document

The resident JSON CLI and Settings window use this service. Native acceptance must
verify live edits in an installed desktop session; headless checks alone do not
establish that behavior.

All required schema fields must be present and unknown fields are rejected. Declared optional fields, including `captureDefaults`, may be absent in older documents. Schema version 1
has no predecessor to migrate. Unsupported versions stay untouched for explicit recovery.
Maximum configuration size is 1 MiB. Missing or invalid configuration on first startup
means setup; it never selects the development destination. An invalid later edit preserves
the last accepted in-memory settings and the bad file so it can be corrected.

| Field | Meaning and default |
| --- | --- |
| `schemaVersion` | `1` |
| `actions` | Exactly `region`, `window`, `desktop`, `clipboard`, plus optional `edit` (Capture region & edit), `monitor` (Capture monitor), `repeat` (Repeat last region) and `text` (Copy text from region). An absent `edit` inherits the region route with no hotkey and an absent `monitor` copies the desktop route with no hotkey; saved documents include both |
| `actions.monitor` | Capture monitor (#1363): the monitor the pointer is on, at its native size. A full action like `desktop`, with its own `destination`, `profileId`, `shortcut` and optional `publicationMode`, so output routing and per-action exceptions treat it like the other captures |
| `actions.text` | Copy text from region (#1428): a hotkey-only entry `{shortcut}`, with nothing else accepted. It copies the text and QR codes in a selected region and stores nothing, so it has no destination, archive, naming or place under per-action exceptions, and it is exempt from `output_required`. An absent entry is added with no key when the document is read. Its hotkey is unassigned by default. See [text recognition](text-recognition.md#operations-journal-and-clipboard) |
| `actions.repeat` | Repeat last region (#1363): a hotkey-only entry `{shortcut}`. It captures the rectangle of the last region capture again with the `region` action's output, and never has a route of its own. Absent entries read as no key |
| Each action's `destination` | `upload` by default; `local` disables uploads; `default` inherits `captureDefaults` |
| Each action's `profileId` | A configured profile ID, or `null` until destination setup |
| Each action's `shortcut` | One Qt PortableText combination; empty means unbound; independent of output inheritance |
| Each action's `publicationMode` | Optional `default`, `share` or `private`; `default` uses the destination's sharing preference |
| `captureDefaults` | Optional shared output: `destination` (`upload` or `local`), `profileId` (configured ID or `null`), and `publicationMode` (`default`, `share` or `private`) |
| `profiles` | At most 32 provider destinations; empty on fresh startup. Array order is the display order of Settings → Destinations and the Destination dropdowns only; everything else refers to profiles by `id`. Reorder with `settings.reorderProfiles` (see [cli.md](../automation/cli.md)) |
| Each profile's `id`, `label`, `provider` | Unique stable ID, display label, registered adapter ID |
| Each profile's `settings` | Non-secret settings validated by its provider adapter |
| Each profile's `credentials` | `keychain` plus a nonempty `reference`, or `session` plus `null` reference |
| `archive.keepUploads` | `false`; enables local saving for every new action, independently of its upload destination |
| `archive.root` | User Documents folder plus `put.gd`; must be absolute |
| `archive.folders` | `images`, `videos`, `text`, `other`; each absolute or `null` |
| `naming.mode` | `original` by default preserves imported basenames; `generated` uses random names (see `naming.randomStyle`); `template` also renames imported files |
| `naming.randomStyle` | Optional, `uuid` by default: Random names are a UUID. `string` makes them a random string of `naming.randomLength` characters plus the final extension (`.gz` of `a.tar.gz`) when it fits the extension limit (a third of the name byte limit, dot included) (#1729) |
| `naming.randomLength` | Optional integer 4 to 64, default 12: characters in a `string` random name before the extension |
| `naming.randomUppercase` / `naming.randomDigits` | Optional booleans, both `true` by default: the character set of a `string` random name also includes `A`–`Z` / `0`–`9`. Lowercase `a`–`z` are always in the set; each character is drawn independently, so a name need not contain every kind. The name is drawn once when the file is taken in and stays fixed on retry. When a `string` name is already taken and `collisionPolicy` is `rename`, a new random string of the same shape replaces it, instead of the `-<uuid>` suffix other names get. When the destination reports a name taken, the replacement is redrawn while it matches another item's candidate in the batch, up to 32 draws in total, then falls back to the `-<uuid>` suffix; if the destination's collision search runs out of attempts, the item fails with `collision_exhausted`. With `rename`, a duplicate name during the initial batch naming is redrawn up to 32 times and is a conflict only if every draw still matches; `skip` and `overwrite` handle batch duplicates without redrawing |
| `naming.fileTemplate` | Optional imported-file pattern, default `{name}.{ext}` |
| `naming.captureTemplate` | Optional screenshot pattern, default `Screen-Capture-{yyyy}-{MM}-{dd}-{HH}-{mm}-{ss}.{ext}` |
| `naming.imageTemplate` / `naming.textTemplate` | Optional clipboard patterns, default timestamped `Image-…` / `Text-…` names |
| `limits.pendingImageBytes` | Optional aggregate retained decoded-image budget, default 67,108,864 bytes (64 MiB); released after persistence/cancellation |
| `naming.recordingTemplate` | Optional screen-recording pattern, default `Screen-Recording-{yyyy}-{MM}-{dd}-{HH}-{mm}-{ss}.{ext}` |
| `limits.recordingAnimationSeconds` / `recordingAnimationEdge` | Optional animated GIF/WebP export budget: retained seconds (default 60) and output long edge (default 1280); see [recording](recording.md) |
| `actions.record` | The screen-recording action. Saved documents carry it as a hotkey-only entry `{shortcut}` that keeps following the region route; a full entry (`destination`, `profileId`, `shortcut`, optional `publicationMode`) routes recordings separately. Absent entries read as hotkey-only with no key |
| `actions.scroll` | Capture scrolling region (#1376): a full action like `monitor`, with its own `destination`, `profileId`, `shortcut` and optional `publicationMode`. Its hotkey is unbound by default. A document saved before it existed gets an entry that copies the `region` route with no key, so the first load changes nothing about where scrolling captures go. `editor.open {"source":"scroll"}` takes the `edit` action's route instead. See [scrolling capture](scroll-capture.md) |
| `naming.collisionPolicy` | `rename` by default; `skip` or explicit `overwrite` |
| `limits.maxFiles` | 100; maximum clipboard batch file count |
| `limits.pendingOperations` | 8 |
| `limits.concurrentUploads` | 3 for new configurations (1–32): files transferring at once across every operation. A batch's files overlap, a new action starts while an earlier upload continues, and a free slot goes to the operation holding the fewest. A multipart file holds one slot for its whole transfer and may borrow idle slots for extra parallel parts, up to four parts at once, handing each back as soon as another file waits for it. Providers that take one request at a time per account (Proton Drive) stay sequential for that account. Existing configurations keep their saved value (the former default, 1, uploads one file at a time) |
| `limits.maxFileBytes`, `maxBatchBytes`, `pendingArtifactBytes` | `"auto"` each. A byte count (1 byte to 1 TiB), `"auto"` or `null`. `"auto"` and `null` ("No limit") set no byte limit of put.gd's own: imports copy in 1 MiB chunks and uploads stream from disk, so memory does not grow with file size (the S3 worker stays near 39 MB from 60 MiB to 6 GiB). Every import is still bounded, before any byte is copied, by the destination's maximum object size (5 TiB for S3) and by free disk space in the data folder less a system reserve of min(5% of the filesystem, 4 GiB) and never under 1 GiB. GNOME warns of low disk space at 5% free, and 1 GiB keeps logs, the session and updates working on a small disk; 15% (journald's SystemKeepFree rule, used before #213) kept 4 GiB on any disk over 27 GiB, which stopped a 32 GiB VM with 2.36 GB free from recording at all. The reserve applies whatever these limits say, and concurrent imports reserve their bytes so two can't both pass the check. A saved value of exactly 2147483648, the default before `"auto"` existed, reads as `"auto"`; to keep a fixed 2 GiB limit, choose a different value such as 2047 MiB. `settings.get` reports the resolved values and the disk figures under `effectiveLimits`. Settings → Advanced → Uploads shows the same beside each field set to Auto, such as "Auto · no limit (keeps 4 GiB free)" (#149) |
| `limits.clipboardTextBytes` | 16 MiB UTF-8 |
| `limits.imagePixels`, `imageBytes` | 100 million pixels and 512 MiB |
| `retention.failedArtifactDays` | 7; zero disables age expiry, not the pending-byte budget. The resident applies it on its own a minute after start and then hourly (#202), as well as on **Clean artifacts** (`cache.clear`). When cleanup retires a failed item's artifact, it first aborts the item's incomplete multipart upload, best effort (see [s3.md](../user/s3.md)) |
| `retention.recentActivityLimit` | 100 visible recent entries; does not prune history |
| `retention.previewCacheBytes` | 256 MiB, image previews and video posters together; zero disables retained previews and History's previews made on selection (`history.preview`) |
| `retention.historyRetentionDays` | `null` means no automatic record expiry |
| `retention.logDays` | Optional, default 14 (1–365): days to keep persistent log files ([diagnostics.md](diagnostics.md#retention)) |
| `retention.logBytes` | Optional, default 50 MiB (1 MiB–1 GiB): total size of persistent log files; the oldest go first |
| `preferences.uploadClipboardBehavior` | `each_link` (default, including when absent in older configurations) copies every successful upload link individually; `list` copies an ordered list once overlapping uploads settle |
| `preferences.regionSelectionMode` | Optional, default `live` (also when absent): region screenshots select over the live desktop, and an open menu must close first (#341). `screenshot_first` captures every monitor before the selector opens; you drag over that frozen frame, open menus included, and the result is cut from it with no second capture (#629). put.gd's own tray menu closes before either mode starts, on GNOME too (#924). Applies to Capture region and Capture region & edit; recording always selects live. Settings → Capture → Region selection, `settings.patch` and MCP `patch_configuration` set it |
| `preferences.captureDelaySeconds` | Optional integer, `0` (default, also when absent), `3`, `5` or `10`: the delay the tray's and the main window's Capture region, Repeat last region, Capture window, Capture monitor, Capture desktop, Capture region & edit and Copy text from region wait before capturing (scrolling capture and recording never wait), with a countdown that can be cancelled. Chosen in Settings → Capture → Capture delay, in the tray's **Capture delay** submenu and in the main window's **Capture delay** status-band control. A hotkey capture is immediate unless `preferences.captureDelayShortcuts` is on, and `action.invoke` (CLI, MCP `perform_action`) takes its own `delaySeconds` and ignores this preference. `settings.patch` and MCP `patch_configuration` set it. See [capture delay](capture-delay.md) |
| `preferences.captureDelayShortcuts` | Optional boolean, `false` by default (also when absent): whether a capture started by a hotkey also waits `preferences.captureDelaySeconds`. Settings → Capture → Capture delay → **Also delay captures started by a hotkey**; `settings.patch` and MCP `patch_configuration` set it. See [capture delay](capture-delay.md) |
| `preferences.recordingFrameRate` | Optional integer, `30` (default when absent) or `60`. Settings → Capture → Screen recording → Frame rate, `settings.patch` and MCP `patch_configuration` set it. New recordings use it when `recording.prepare` omits `fps`; an explicit `fps` wins. See [adaptive rate and progress](recording/session-contract.md#frame-rate) |
| `preferences.recordingMicrophone` | Optional boolean, default `false` (also when absent): the recording bar's Microphone toggle, remembered. The microphone is opened only while the toggle is on and released when the recording stops. See [Microphone](recording/microphone-and-clicks.md#microphone) |
| `preferences.recordingMicrophoneDevice` | Optional string, default empty (the system default input): the sound server's source name of the input to record, as `recording.microphones` lists it; at most 256 characters, no control characters. Settings → Capture → Screen recording → Microphone. The monitor of an output is never used |
| `preferences.recordingClickHighlights` | Optional boolean, default `false`: a ring at the pointer on each click in recordings, where the desktop offers the clicks. Settings → Capture → Screen recording → Click highlights. See [Click highlights](recording/microphone-and-clicks.md#click-highlights) |
| `preferences.textRecognitionLanguages` | Optional array of 1 to 3 different Tesseract language identifiers, such as `eng`, `deu` or `chi_sim`, in order. Each matches `^[a-z]{3,4}(_[a-z0-9]+){0,2}$` and is not `osd` or `equ`. Absent means the desktop locale's language when its data is installed, then English. Validation checks only the pattern, the count and uniqueness. Whether the data is installed is checked when a recognition starts, and a saved language whose data is gone is skipped when another loads. Settings → Capture → Text recognition, `settings.patch` and MCP `patch_configuration` set it. See [text recognition](text-recognition.md#languages-and-where-data-is-read-from) |
| `preferences.quickRedactTerms` | Optional array of up to 200 strings, absent by default: words or phrases the editor's Quick redact **My redaction list** suggests covering, matched case-insensitively against recognized text. Each is 1 to 100 code points after Qt's trimming, on one line (no line feed, carriage return, U+2028 or U+2029), with at least one letter or digit; the schema keeps only the line-break rule and a minimum length of 1. Every save path (Settings, `settings.patch` and MCP `patch_configuration`) trims each one and drops exact repeats after trimming, in the order written, so case variants such as `projectalpha` and `ProjectAlpha` are both kept (matching ignores case; saving does not merge them); a no-break space inside a word is kept. A bad entry fails with `config_invalid` naming its position (`/preferences/quickRedactTerms/2`), "Word 3: An entry is longer than 100 characters." and so on, never the value. It is plain configuration, not a keychain item: it is the user's own words on their own machine. It is never uploaded or logged, and a diagnostics package keeps only `{"count": N}` ([diagnostics](diagnostics.md)). Settings → Capture → Text recognition → Always suggest redacting (one entry per line; an empty box removes the key), `settings.patch` and MCP `patch_configuration` set it. The editor gets it in its bootstrap frame. See [text recognition](text-recognition.md#the-editor) |
| `preferences.quickRedactPatterns` | Optional array of up to 50 strings, absent by default: regular expressions (PCRE2, case as written) that **My redaction list** matches against the OCR text as shown (no NFKC, digit or case folding): each visual line, and each pair of consecutive lines joined with a line break. Each is 1 to 500 code points as written (it is not trimmed when it runs; trimming, Qt's `QChar::isSpace`, only decides whether it is blank) on one line (no line feed, carriage return, U+2028 or U+2029), must not be blank, must compile as written and must not match empty text. The shape of a pattern is not judged. At run time PCRE2's `(*LIMIT_MATCH=10000)(*LIMIT_DEPTH=10000)` is put in front of it and a child process the editor kills at its deadline supplies the time bound. Every save path drops exact repeats, as written; the schema enforces the 500 limit and the line-break rule. A bad one fails with `config_invalid` naming its position, such as "Pattern 2: This pattern does not compile: … at offset 5." or "Pattern 2: This pattern matches empty text.", never the pattern. Settings → Capture → Text recognition → Patterns (regular expressions), one per line; Settings does not save until each pattern is fixed. Stored, sent and left out of diagnostics like `quickRedactTerms` |
| `preferences.sensitiveTextCheck` | Optional object `{enabled, categories}`, absent by default (off, with the categories `card`, `key` and `list`). `enabled` is a boolean. `categories` lists 1 to 7 different Quick redact categories: `email`, `phone`, `ip`, `card`, `key`, `name` and `list`. With `enabled` true, an image capture going to a destination has its text recognized before it uploads, and put.gd asks only when it finds text of these categories or can't finish checking ([checking captures before uploading](text-recognition.md#checking-captures-before-uploading)). Anything else fails with `config_invalid`. Settings → Capture → Text recognition → Before uploading, and Look for. Holds no user text, so diagnostics keep it as it is |
| `preferences.scrollCaptureMaxHeight` | Optional integer 2,000 to 60,000, default `30000` (also when absent): the tallest image a scrolling capture may make, in pixels. The smaller of it, `limits.imagePixels / width` and `limits.imageBytes / (4 × width)` applies, and for the editor route also the editor's 24 MP admission. Settings → Capture → Scrolling capture → Maximum height, `settings.patch` and MCP `patch_configuration` set it; `scroll.prepare` may pass a `maxHeight` of its own for one capture. A capture that reaches it ends `height_cap` with a warning ([scrolling capture](scroll-capture.md#output-and-limits)) |
| `preferences.scrollCaptureFromTop` | Optional boolean, default `false` (also when absent): auto-scroll takes the page to its top first, when it begins before anything has moved, so the image starts at the top. It applies to auto-scroll only. Settings → Capture → Scrolling capture → Auto-scroll goes to the top first |
| `preferences.scrollCaptureAutoScroll` | Optional boolean, default `false` (also when absent): a scrolling capture starts with auto-scroll on, put.gd turning the page; `false` waits for the user to scroll. The bar's **Auto-scroll** switches either way during a capture, and `scroll.prepare` may pass a `driver` of its own. Settings → Capture → Scrolling capture → Start with auto-scroll ([scrolling capture](scroll-capture.md#drivers)) |
| `preferences.fileManagerFolders` | Optional, `zip` (default, also when absent) or `files`: how a folder chosen with Upload with put.gd in a file manager, or passed to `upload.files` (CLI, MCP `upload_files`), uploads, as one `<folder>.zip` or file by file (`limits.maxFiles` counts the expanded files). Settings → General → File manager → Folders. See [file-manager-upload.md](file-manager-upload.md#folders) |
| `preferences.fileManagerMenu` | Optional boolean, default `true`: the AppImage's per-user Upload with put.gd entries; `false` removes the files it wrote. DEB/RPM entries are system files and always on. See [file-manager-upload.md](file-manager-upload.md#turning-it-on) |
| `preferences.notifications`, `launchAtLogin` | `true`, `false` |
| `preferences.updates` | Optional, default `{"check": true, "channel": "build"}`, both keys optional. `check`: look for a newer version once a day (one request to app.put.gd with nothing identifying). `channel`: `build` follows this installation's channel (Settings stores it when you pick the build's own), or `stable` or `nightly` to watch that one; an AppImage then offers to switch to it ([updates.md](updates.md#switching-channels)). Stable notifies once per new version, remembered per channel; nightly only shows it in Settings. Development and source builds never check. Checks disabled at build time report `disabled_at_build` ([updates.md](updates.md)) |
| `preferences.unlockCredentialsAtStartup` | Optional, default `true`: read saved keychain S3 credentials once when put.gd starts. A locked keyring shows its own unlock prompt then; uploads and hotkeys never unlock |
| `preferences.appearance` | Optional. `theme`: `system` (default, also when absent) follows the desktop's light or dark preference, `dark` or `light`. `accent` (null) and `overrides` (`{"dark": {}, "light": {}}`) are reserved for custom colours (#197) and must keep those values. See [appearance](appearance.md) |
| `preferences.editorRecentColors` | Optional, written by put.gd: up to 6 lowercase `#rrggbb` custom editor colors, most recent first |
| `preferences.diagnostics.debugLogging` | Optional, default `false`: keep debug records in the persistent log. put.gd sets it back to `false` 24 hours after it was turned on ([diagnostics.md](diagnostics.md#debug-logging)) |
| `preferences.diagnostics.packageFolder` | Optional absolute folder, or `null`: where **Create diagnostics package** and **Save diagnostics for this upload** save packages. Settings writes it when you choose a folder; omitted or `null` means `<archive root>/diagnostics/` ([diagnostics.md](diagnostics.md#diagnostics-package)) |

Local saving and uploading are independent outputs. An upload route with a configured
profile uploads; `archive.keepUploads: true` also saves a local copy before publication.
With no upload destination, that flag enables local-only output. If neither output is
enabled, `output_required` is returned before creating an operation or acquiring a source.
Changing these settings does not affect already accepted operations or delete existing files.

This changes the earlier meaning of `destination: "local"`: it now disables uploading,
while the shared local-copy switch controls saving. A legacy local route with
`archive.keepUploads: false` is still valid configuration, but produces `output_required`.
To deliberately enable local-only clipboard output, apply this non-secret patch through
`settings.patch` or MCP `patch_configuration`, with the current expected revision:

```json
{"archive":{"keepUploads":true},"actions":{"clipboard":{"destination":"local","profileId":null}}}
```

Before this patch the local route may have no enabled output; afterward it saves locally.
To make region capture inherit a shared local-only default instead, use:

```json
{"archive":{"keepUploads":true},"captureDefaults":{"destination":"local","profileId":null,"publicationMode":"default"},"actions":{"region":{"destination":"default","profileId":null,"publicationMode":"default"}}}
```

An inherited action requires `captureDefaults`, a null own `profileId`, and absent/default
own `publicationMode`. Its hotkey is always its own setting. Explicit action routes
remain exceptions when the default changes. Documents without `captureDefaults` retain
explicit per-action routing; changing capture output in the GUI adopts the optional shared
default. This extension remains schema version 1; older strict readers that do not support
it reject the new field, so use matching application/CLI/MCP versions.

The first three hotkeys default to `Ctrl+Shift+1`, `Ctrl+Shift+2`, and `Ctrl+Shift+3`.
Clipboard sharing, Capture region & edit, Capture monitor, Repeat last region, Copy text and screen
recording start unbound, allowing explicit setup on GNOME. The recording key toggles a recording (start a ready one, stop
a running one) so the keyboard can end a capture without reaching for the bar. Availability and
actual hotkey bindings must be reported by the native adapter; configuration validation
does not claim a successful grab or resolve conflicts by taking another app's binding.

Unset archive overrides resolve to `<root>/<category>`. Video routing handles existing
regular files; configuration defines no recording actions. Filesystem availability is checked
when saving output, so an unavailable archive folder does not prevent accepting unrelated
configuration or block a requested upload.

The service watches both the file and parent directory, debounces notifications for 75 ms,
and re-arms after atomic replacements. If watching fails, it rescans every two seconds;
that includes a fresh install whose configuration folder does not exist yet.
Content SHA-256 revisions deduplicate accepted changes. Metadata distinguishes the
accepted revision from observed file bytes and reports validation/watch status.
A rescan reports a status change, which refreshes every open view, only when it
accepted new settings or the metadata changed; an identical rescan is silent (#234).
In CLI `settings.get`, these fields are under `result.metadata`, while
`result.document` is the accepted configuration. When external bytes are invalid,
`metadata.validation.ok` is false and its error identifies the failing JSON path;
`acceptedRevision` continues to identify the accepted document and
`observedRevision` identifies the invalid file. Metadata inspection itself can
still succeed. Backend `diagnostics` does not include this validation state.
Loading, validating, and inspecting configuration performs no capture, clipboard-content
read, provider request, or secret-store unlock. Accepted changes notify consumers so they
can refresh preferences and request hotkey rebinding. Operations use immutable snapshots;
historical receipts continue to identify their original destination.

For external edits, save a complete JSON document to a temporary file in the same directory
and rename it atomically to `config.json`. Cooperative writers use `config.json.lock` and
an expected content revision. App writes use `QLockFile`, re-read under the lock, validate
the complete candidate, and use `QSaveFile` with direct-write fallback disabled and owner-only
permissions. A stale revision returns `config_conflict` without changing the file.
An editor ignoring the lock can race a write; this is cooperative conflict detection.

The shared patch service recursively overlays objects, replaces arrays/scalars, and treats
JSON `null` as an explicit value. It is not RFC 7396 merge patch: `null` clears optional
folder/profile fields rather than removing required keys. The complete resulting document
must validate.

GUI forms autosave through the same revision check, one request at a time. Each edit becomes an
operation: a destination's fields, keyed by its ID; a removal with the routes that fall back; the
destination order; or one setting outside the list, each capture route being one of those. Each
operation is applied to the latest accepted document when it is sent.

- **When edits are queued.** Typed text is queued after a 600 ms pause or when the field loses
  focus; choices, toggles and pastes at once. Focusing an unchanged field issues no save.
- **Queue order.** A later edit of a destination, or a later move, replaces the queued one in its
  place. A setting outside the list changed again goes to the end, after the destinations its
  routes may name. An operation waiting for a destination lets later ones go first.
- **The form's own check.** A group of fields that fails it is never sent, whether it was typed
  or comes from a restored draft: an S3 destination's Region and Endpoint, which always save
  together (Region checked against the Endpoint, so `aws-global` only with Amazon S3; an S3
  destination on a Cloudflare R2 account endpoint skips the Region check, since R2 requests are
  signed with `auto`), and a Cloudflare R2 destination's Account ID, Jurisdiction
  and Bucket, which save together once all three are valid ([r2.md](../user/r2.md)). The accepted
  values stay, and unrelated fields and other destinations still save. A new R2 destination, or
  a switch of a destination to R2 made in the open window, waits as a whole until those three are
  valid, and route changes to it wait with it; captures meanwhile use the saved settings. A switch
  restored from a checkpoint is replayed field by field and can be refused until the three are
  valid.
- **Refusals.** A field the resident refuses holds back only its own group. A request the
  resident refuses again unchanged, such as an edit of a destination whose saved Region today's
  rules refuse, stops with its reason while the rest of the queue goes on.
- **Failures and conflicts.** A transport/write failure stops the queue until Retry saving is
  pressed. A `config_conflict` keeps the queue, except a reorder not saved yet: whether the
  conflict comes back as the answer to any save or the window finds it itself (snapshot or file
  watcher), the queued reorder is dropped and the list goes back to the order the window last
  accepted, then follows the file's order once the window reads it again, with an add not saved
  yet kept after its neighbour. A refused reorder does the same unless
  a later one is queued, and a later order that settles clears an older one's refusal.
- **Retry saving after a conflict, and restored drafts.** The resident reads the file again and, only if that file
  validates, the queue is applied to it. Queued values equal to what the window last had accepted
  are dropped, so a setting changed elsewhere that the window didn't touch keeps its new value.
  A group goes whole if any of it changed: Region with Endpoint, R2's three fields, and a route's
  destination with its profile ID. So changing one field of a group writes the window's value of
  the others over a change made elsewhere. A restored draft's changes are replayed against the
  configuration it was based on in the same groups, so the same overwrite can happen there. An edit of a destination no longer in the file is
  dropped and named in the footer; an add whose ID is now taken gets a fresh ID. The form keeps
  a refused field group or setting as typed, and an add the resident refused goes again as typed.
  A destination already in the file whose whole change was refused, such as a switch to another
  service, shows what the file has.
- **Routes.** Each capture route carries only the fields that changed (destination and profile
  ID together). With no default output in the file, the sharing mode shown is the Region
  action's and is sent only if changed. A sharing mode the user chose is sent as is; when the
  request leaves the default output without one, it takes the Region action's mode from the
  file as it is when sent. A route that names a destination
  still being added waits until that add is saved. If the resident refuses the add, the routes
  waiting on it are dropped from the queue and the footer says "A route to <name> wasn't saved:
  <name> wasn't saved."; a route chosen to it afterwards isn't queued either, and the footer says
  "A route to a destination that wasn't saved isn't saved yet." The form keeps those routes, and
  once the add is saved they are sent. A route to a destination removed elsewhere is dropped and
  the footer says so. An action set to "default" (inherit the default output) while the file has
  no default output waits while the default output's route is queued, isn't queued while that
  route names an add that was refused, and is dropped when that route is dropped; it goes with
  the default output once the add is saved. Recording, which follows Region by leaving its route unset,
  waits for nothing.
- **Removals.** A removal's route fallbacks are worked out from the file when it is sent, for
  the routes that still name it there. Removing a destination cancels any route still queued to
  it, and the form's routes to it show the file's again. An add removed before it was sent
  leaves nothing to remove. One removed while in flight is removed once its save is confirmed.
  If that answer was lost, it is removed only when the resident's settings hold exactly the
  destination that was sent, decided once the window has the resident's current settings again
  (`ui.snapshot`, or the file read again on a conflict's Retry saving); Retry saving waits for
  that, and if it fails the removal stays pending.
- Reload discards the queue before it reads the file, so if the file can't be loaded, values the
  form still shows aren't queued.

Access keys, secrets, session tokens, and source payloads are never configuration values.
Provider adapters must reject secret-bearing settings fields. Credential references identify
OS-secret-store records; session credentials are explicitly supplied to the running process.

The session CLI exposes `settings.developmentTemplate` for inspection and
`settings.importDevelopment` with `expectedRevision` for deliberate setup. Import
validates the full candidate through the registered S3 adapter; without that adapter
it fails and keeps current configuration. The example never activates itself. Its
bucket/region/HTTPS endpoint/prefix/storage class/ACL/public URL/addressing and all
connect/request/low-speed timeout controls are explicit non-secret values. Replace
its placeholder local root before using the file directly. Credentials remain references.

Watcher reload and CLI writes/reads run on a serialized configuration worker. Public
synchronous APIs remain available for startup/tools/tests; acceptance signals and watcher
updates always run on the service's owner thread. Indexed history has no automatic
expiry with null `historyRetentionDays`; a positive value expires old confirmed records
at startup and hourly, retaining active deletion identities. Cache cleanup leaves
history and user-owned local files intact.

S3 settings are validated by the registered adapter. `bucket` is a bucket
name from 3 to 63 UTF-8 bytes; `region` must be nonempty. `endpoint` is an absolute HTTPS
URL without user information, query or fragment (literal-loopback HTTP is allowed
for local endpoints). `objectPrefix` is at most 1022 UTF-8 bytes; the final prefix/key
is limited to 1024 bytes. Empty path segments of the prefix are dropped, as the SDK
drops them from the request path, and one separator joins a nonempty prefix to the
name (#1582). A prefix with a `.` or `..` segment passes this validation, so a saved
configuration still loads, but uploads and Test upload and link refuse it with
`s3_settings_invalid`.
`storageClass` accepts STANDARD, REDUCED_REDUNDANCY, STANDARD_IA, ONEZONE_IA,
INTELLIGENT_TIERING, GLACIER, DEEP_ARCHIVE or GLACIER_IR. Cold storage may not support
immediate public GET. `aclMode` is public-read or omit; no error changes that choice.
`publicUrlBase` is null or an absolute HTTPS URL with an optional already-encoded
path and no credentials, query or fragment (literal-loopback HTTP is also accepted).
Null derives an object URL from endpoint/bucket/addressing. `addressing` is path,
virtual or auto; virtual rejects dotted buckets and literal IP endpoints, and auto
uses virtual only for an AWS host with an undotted bucket. Path mode is required for
the development dotted bucket. Validation performs no network or secret-store IO.
For S3-compatible services, the endpoint must support conditional writes. An `s3`
profile whose endpoint host is an R2 account endpoint signs with `auto` whatever
`region` holds; new R2 destinations use the `cloudflare-r2` provider below. Backblaze B2's S3 API lacks conditional writes,
so Test connection fails there; use the `backblaze-b2` provider instead. See
[S3-compatible services](../user/s3.md#s3-compatible-services).

`backblaze-b2` profiles (#282) hold exactly `apiUrl`, `bucket`, `objectPrefix`,
`linkMode` (`public` or `authorized`), `linkValidityDays` (1–7), `publicUrlBase`
(null or HTTPS, public links only), `connectTimeoutMs` and `requestTimeoutMs`.
`requestTimeoutMs` bounds every B2 request except a file upload or large-file part,
which has no whole-request limit: the worker's stall limit (no data for
max(`connectTimeoutMs`, 30 s)) ends it instead (#1782).
Credentials are a keychain or session reference to the application key ID and key.
See [Backblaze B2](../user/b2.md).

`cloudflare-r2` profiles (#1678) hold exactly `accountId` (32 lowercase hexadecimal
digits), `jurisdiction` (`default`, `eu` or `fedramp`), `bucket` (R2's rule: 3–63
lowercase letters, digits and hyphens, starting and ending with a letter or digit),
`objectPrefix`, `linkMode` (`public` or `presigned`, default `presigned`),
`linkValidityDays` (1–7), `publicUrlBase` (null or HTTPS; public links need the
bucket's r2.dev address or a custom domain), `connectTimeoutMs`, `requestTimeoutMs`,
`lowSpeedBytesPerSecond` and `lowSpeedTimeSeconds`. Requests go through the S3
adapter with the endpoint `https://ACCOUNT[.eu|.fedramp].r2.cloudflarestorage.com`,
region `auto`, path addressing, ACL omit and storage class STANDARD; the S3 rules
then apply to those settings. Credentials are a keychain or session reference to the
R2 API token's access key ID and secret. Receipts record the `cloudflare-r2`
destination. See [Cloudflare R2](../user/r2.md).

`custom-http` profiles (#1361) hold exactly `requestMethod`, `requestUrl`,
`allowInsecureHttp`, `body`, `fileFormName`, `headers`, `parameters`, `arguments`,
`secrets`, `data`, `dataSecret`, `accepts`, `connectTimeoutMs`, `requestTimeoutMs`,
`responseUrl`, `thumbnailUrl`, `deletionUrl` and `errorMessage`. A row of `headers`,
`parameters` or `arguments` is `{"name":"Authorization","secret":true}` or
`{"name":"format","secret":false,"value":"json"}`: a secret row never has a value in
configuration, and `secrets` rows are always a name with `"secret":true`. With
`dataSecret` true, `data` is empty. `{secret:key}` may appear in `requestUrl`, a row
value and `deletionUrl` only, and must name a value the profile stores. `data` is sent
as written apart from `{filename}` and `{input}`; a body that holds a key is made
secret as a whole with `dataSecret`. `requestUrl` is HTTPS, or HTTP to a literal
loopback host, or any HTTP with `allowInsecureHttp`. Validation performs no network or
secret-store IO. Credentials are a keychain or session reference to the named values.
See [Custom uploader](../user/custom-uploader.md#settings).

`connectTimeoutMs` bounds connection setup. `requestTimeoutMs` maps to the SDK whole
Curl-request timeout (`httpRequestTimeoutMs`) for every request except a PUT with a
body of positive `Content-Length` (PutObject, UploadPart). That one has no
whole-request limit: the connect timeout, the low-speed limit and the resident's
worker budget still bound it (#1758). `lowSpeedBytesPerSecond` maps to the
SDK low-speed limit, and `lowSpeedTimeSeconds` maps to its low-speed duration
(`requestTimeoutMs` in SDK terminology). These settings are positive integral values
up to 2147483647. The adapter bounds concurrent SDK resources and caches clients by
effective settings plus explicit credential generation. A changed accepted profile
creates another effective client; pending operations and deletion retain originals.
See [S3 behavior](../user/s3.md) and [credential setup](credentials.md).


Filename templates accept `{name}` (original stem), `{ext}` (extension without a
dot), `{yyyy}`, `{MM}`, `{dd}`, `{HH}`, `{mm}`, `{ss}` (local action time),
and `{uuid}` (stable item identity). Literal text is allowed; paths, control
characters and unknown/unclosed tokens are rejected. For example,
`Screen-Capture-{yyyy}-{MM}-{dd}-{HH}-{mm}-{ss}.{ext}` produces
`Screen-Capture-2026-10-08-04-27-21.png`. Names are resolved using the accepted action timestamp before archive/publication
and persisted with immutable artifacts. Retries do not regenerate timestamps; only
confirmed collisions can add a suffix. Local saving never overwrites existing files.
Legacy configurations without optional fields remain valid and retain their mode.

The decoded-image budget applies to images held by accepted jobs waiting for or
undergoing disk encoding, not total RSS. A new acquired image that cannot fit fails
with `image_memory_limit`; the app does not silently substitute a later clipboard.
Encoding workspace, native frozen monitor frames and the current clipboard result
are separate transient/ownership costs. Retry artifacts and previews live on disk;
previewCacheBytes bounds disk bytes rather than a preloaded image heap.

## Destination checks

The resident keeps each destination's last check in `destination-status.json` in the
data folder (#1692), owner-only. A check is **passed** or **failed**, and comes from
**Test** (`source: test`) or from an upload (`source: upload`). The file holds no
credential, link or object name: per destination its state, source, the error code of
a failure, a connection fingerprint and a credential revision. Closing Settings,
restarting put.gd and logging in again keep it. The resident does not rerun the check
on a timer, and the row shows no age.

A record counts only while it matches the destination as saved now:

- **Connection fingerprint.** A SHA-256 of the provider, the credential reference
  (`mode` and `reference`) and these settings, compared exactly:

  | Provider | Settings in the fingerprint |
  | --- | --- |
  | `s3` | `endpoint`, `bucket`, `region`, `addressing`, `objectPrefix`, `storageClass`, `aclMode`, `linkMode`, `publicUrlBase` |
  | `backblaze-b2` | `apiUrl`, `bucket`, `objectPrefix`, `linkMode`, `publicUrlBase` |
  | `proton-drive` | `accountRootUid`, `folderUid` |
  | `custom-http` | `requestMethod`, `requestUrl`, `allowInsecureHttp`, `body`, `fileFormName`, `headers`, `parameters`, `arguments`, `secrets`, `data`, `dataSecret`, `responseUrl`, `deletionUrl`, `accepts` |

  These decide where requests go, who signs them, what a bucket or key policy can
  scope to (the folder in the bucket), what the server may refuse (storage class,
  ACL), the link and cleanup the test followed, and for a custom uploader the file
  types it accepts, which decide the synthetic file its test sends. Left out, so
  changing them keeps the record: the label, the destination's place in the list,
  being the capture default or an action's destination, timeouts and slow-transfer
  limits, S3/B2 link validity days, the Proton folder name, share preferences and
  share password, and a custom uploader's thumbnail URL and error message. A provider
  with no list is fingerprinted by all of its settings.
- **Credential revision.** A count per destination, kept in the same file. Every
  Store of credentials (keychain or this session) and every Proton sign-in
  (`connectAccount`, including **Reconnect account…**) adds one when it starts and one
  more when it succeeds, so a finished check does not vouch for keys stored after it.
  Each advance is saved as a best effort: when the status file cannot be written, the
  Store still goes ahead, and a restart can show the earlier result until the next
  Test or upload replaces it. A failed Store also leaves the destination untested. Two destinations can name the same
  keychain entry ([credentials](credentials.md)): a Store or sign-in for one advances
  the revision of every configured destination with the same `mode` and `reference`.
  Session keys are each destination's own. Reading saved keychain credentials (Unlock,
  or the startup read) adds nothing.

Changing a fingerprinted setting makes the record stale without a write; the
destination shows untested. A Store or sign-in writes the new revision. Changing a
setting back shows the earlier result again only while that record is still kept and
its revision is still the current one: a Test or an upload that updates the record
for the changed settings replaces it, and new credentials outdate it for good. Removing a destination forgets
its record.

**Session-only keys.** A destination in `mode: session` whose provider keeps keys (S3,
Backblaze B2, a custom uploader with secret values) has its record and revision in
memory only; neither is written to the file. Its keys end with the resident, so after
a restart it is untested. A destination with no keys, such as a custom uploader
without secret values (saved in `mode: session`), and every keychain destination are
saved as described above. The rule looks at the destination's mode when the file is
written: a session-only pass of a destination switched to keychain mode is written by
the next save of any record. Loading does not filter by mode, so after switching back
that written pass survives a restart unless a successful save in session mode has
dropped it, until a Test or upload replaces it.

What updates the record:

- **Test.** Its result replaces an upload's record, passed or failed with its error
  code, with `source: test`, even when the state and code are the same. A Test whose
  result equals the Test record already kept leaves it as it is, without a write.
  A Test held back before it reached the provider, such as one asked for while journal
  recovery is still running (its error carries `details.notAttempted`), leaves the
  record as it was.
- **Uploads.** When an upload operation ends, a file that became uploaded in that run,
  including a Retry that confirms an earlier uncertain upload without sending it
  again, records passed; an existing pass, from a Test or an upload, stays as it
  was. Otherwise a refusal that this run met, on an upload or on its link, records
  failed. An item error that a Retry carried from History and that is still the same
  error code does not count. A refusal above counts even when the item's upload or
  link outcome stays unknown. The refusals are `s3_authentication_failed`,
  `s3_permission_denied`, `s3_bucket_not_found`, `b2_authentication_failed`,
  `b2_permission_denied`, `b2_key_restricted`, `b2_bucket_not_found`,
  `proton_credentials_invalid`, `proton_account_changed`, `proton_folder_unavailable`,
  `http_authentication_failed`, and the Proton helper's own `authentication_required`,
  `permission_denied`, `account_changed` and `destination_unavailable`, which only the
  Proton provider passes on. Every other outcome, such as a network failure, a
  timeout, a rate limit, a cancel, a size limit or an unknown outcome, leaves the
  record.
- A Test or upload records nothing when, at the moment it finishes, the destination's
  fingerprint or credential revision differs from the ones it started with.

A file that cannot be read whole starts empty. On reading, a revision must be an exact
integer from 0 to 2^53, and a record's `errorCode` must be one of the refusals above;
anything else drops that entry. A failed Test whose error was not a refusal, such as
a timeout, is therefore shown after a restart as untested.

`ui.snapshot` and `settings.get` (MCP `inspect`, at
`result.configuration.result.destinationStatus`) carry `destinationStatus`, keyed by
profile ID, with an entry for every configured destination against its saved settings
and current credential revision. An untested entry is `{state: "untested"}`. A passed
entry adds `source` and `fingerprint`; a failed one adds `errorCode` too. Settings →
Destinations shows it: **Connected** (Proton: **Account and folder checked**), **Test
failed** (Proton: **Check failed**), **Needs testing** after an upload failure
(Proton: **Needs checking**), and **Ready to test** (Proton: **Configured**)
otherwise. Needs-credentials, signed-out and folder states come first. **Make
default** asks for confirmation unless the destination is passed. The Settings window
computes the same fingerprint for the destination being edited, so an unsaved
connection edit shows untested at once, and a test's completion message follows the
same rule: a test whose destination was renamed or moved while it ran reports its
result as current. The message compares only the profile ID and fingerprint, not the
credential revision: when keys are stored while a Test runs, the message can report
success while the resident discards the result and the row stays untested.
