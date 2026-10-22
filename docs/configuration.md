# Configuration

The resident app owns one `config.json` in its configuration folder. GUI and CLI
report the exact path. No separate YAML file or preferences database exists.
The schema is [config.schema.json](../schemas/config.schema.json). The shared
`SettingsService` is the authoritative validator, including registered provider rules,
absolute paths, shortcut syntax, and cross-profile references.

## Locations

| Folder | Linux | Windows |
| --- | --- | --- |
| Configuration: `config.json` and its `config.json.lock` | `$XDG_CONFIG_HOME/putgd`, normally `~/.config/putgd` | `%LOCALAPPDATA%\putgd` |
| Data: `history.sqlite`, `artifacts/`, `previews/`, `destination-tests/`, editor and recording staging, `resident.lock` | `$XDG_DATA_HOME/putgd`, normally `~/.local/share/putgd` | `%LOCALAPPDATA%\putgd` |

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
| `actions` | Exactly `region`, `window`, `desktop`, `clipboard`, plus optional `edit` (Capture region & edit). An absent `edit` inherits the region route with no shortcut; saved documents include it |
| Each action's `destination` | `upload` by default; `local` disables uploads; `default` inherits `captureDefaults` |
| Each action's `profileId` | A configured profile ID, or `null` until destination setup |
| Each action's `shortcut` | One Qt PortableText combination; empty means unbound; independent of output inheritance |
| Each action's `publicationMode` | Optional `default`, `share` or `private`; `default` uses the destination's sharing preference |
| `captureDefaults` | Optional shared output: `destination` (`upload` or `local`), `profileId` (configured ID or `null`), and `publicationMode` (`default`, `share` or `private`) |
| `profiles` | At most 32 provider destinations; empty on fresh startup. Array order is the display order of Settings → Destinations and the Destination dropdowns only; everything else refers to profiles by `id`. Reorder with `settings.reorderProfiles` (see [cli.md](cli.md)) |
| Each profile's `id`, `label`, `provider` | Unique stable ID, display label, registered adapter ID |
| Each profile's `settings` | Non-secret settings validated by its provider adapter |
| Each profile's `credentials` | `keychain` plus a nonempty `reference`, or `session` plus `null` reference |
| `archive.keepUploads` | `false`; enables local saving for every new action, independently of its upload destination |
| `archive.root` | User Documents folder plus `put.gd`; must be absolute |
| `archive.folders` | `images`, `videos`, `text`, `other`; each absolute or `null` |
| `naming.mode` | `original` by default preserves imported basenames; `generated` uses UUIDs; `template` also renames imported files |
| `naming.fileTemplate` | Optional imported-file pattern, default `{name}.{ext}` |
| `naming.captureTemplate` | Optional screenshot pattern, default `Screen-Capture-{yyyy}-{MM}-{dd}-{HH}-{mm}-{ss}.{ext}` |
| `naming.imageTemplate` / `naming.textTemplate` | Optional clipboard patterns, default timestamped `Image-…` / `Text-…` names |
| `limits.pendingImageBytes` | Optional aggregate retained decoded-image budget, default 67,108,864 bytes (64 MiB); released after persistence/cancellation |
| `naming.recordingTemplate` | Optional screen-recording pattern, default `Screen-Recording-{yyyy}-{MM}-{dd}-{HH}-{mm}-{ss}.{ext}` |
| `limits.recordingAnimationSeconds` / `recordingAnimationEdge` | Optional animated GIF/WebP export budget: retained seconds (default 60) and output long edge (default 1280); see [recording](recording.md) |
| `actions.record` | The screen-recording action. Saved documents carry it as a shortcut-only entry `{shortcut}` that keeps following the region route; a full entry (`destination`, `profileId`, `shortcut`, optional `publicationMode`) routes recordings separately. Absent entries read as shortcut-only with no key |
| `naming.collisionPolicy` | `rename` by default; `skip` or explicit `overwrite` |
| `limits.maxFiles` | 100; maximum clipboard batch file count |
| `limits.pendingOperations` | 8 |
| `limits.concurrentUploads` | 3 for new configurations (1–32): files transferring at once across every operation. A batch's files overlap, a new action starts while an earlier upload continues, and a free slot goes to the operation holding the fewest. A multipart file holds one slot for its whole transfer and may borrow idle slots for extra parallel parts, up to four parts at once, handing each back as soon as another file waits for it. Providers that take one request at a time per account (Proton Drive) stay sequential for that account. Existing configurations keep their saved value (the former default, 1, uploads one file at a time) |
| `limits.maxFileBytes`, `maxBatchBytes`, `pendingArtifactBytes` | `"auto"` each. A byte count (1 byte to 1 TiB), `"auto"` or `null`. `"auto"` and `null` ("No limit") set no byte limit of put.gd's own: imports copy in 1 MiB chunks and uploads stream from disk, so memory does not grow with file size (the S3 worker stays near 39 MB from 60 MiB to 6 GiB). Every import is still bounded, before any byte is copied, by the destination's maximum object size (5 TiB for S3) and by free disk space in the data folder less a system reserve of min(5% of the filesystem, 4 GiB) and never under 1 GiB. GNOME warns of low disk space at 5% free, and 1 GiB keeps logs, the session and updates working on a small disk; 15% (journald's SystemKeepFree rule, used before #213) kept 4 GiB on any disk over 27 GiB, which stopped a 32 GiB VM with 2.36 GB free from recording at all. The reserve applies whatever these limits say, and concurrent imports reserve their bytes so two can't both pass the check. A saved value of exactly 2147483648, the default before `"auto"` existed, reads as `"auto"`; to keep a fixed 2 GiB limit, choose a different value such as 2047 MiB. `settings.get` reports the resolved values and the disk figures under `effectiveLimits`. Settings → Advanced → Uploads shows the same beside each field set to Auto, such as "Auto · no limit (keeps 4 GiB free)" (#149) |
| `limits.clipboardTextBytes` | 16 MiB UTF-8 |
| `limits.imagePixels`, `imageBytes` | 100 million pixels and 512 MiB |
| `retention.failedArtifactDays` | 7; zero disables age expiry, not the pending-byte budget. The resident applies it on its own a minute after start and then hourly (#202), as well as on **Clean artifacts** (`cache.clear`). When cleanup retires a failed item's artifact, it first aborts the item's incomplete multipart upload, best effort (see [s3.md](s3.md)) |
| `retention.recentActivityLimit` | 100 visible recent entries; does not prune history |
| `retention.previewCacheBytes` | 256 MiB, image previews and video posters together; zero disables retained previews and History's previews made on selection (`history.preview`) |
| `retention.historyRetentionDays` | `null` means no automatic record expiry |
| `retention.logDays` | Optional, default 14 (1–365): days to keep persistent log files ([diagnostics.md](diagnostics.md#retention)) |
| `retention.logBytes` | Optional, default 50 MiB (1 MiB–1 GiB): total size of persistent log files; the oldest go first |
| `preferences.uploadClipboardBehavior` | `each_link` (default, including when absent in older configurations) copies every successful upload link individually; `list` copies an ordered list once overlapping uploads settle |
| `preferences.notifications`, `launchAtLogin` | `true`, `false` |
| `preferences.updates` | Optional, default `{"check": true, "channel": "build"}`, both keys optional. `check`: look for a newer version once a day (one request to app.put.gd with nothing identifying). `channel`: `build` follows this installation's channel (Settings stores it when you pick the build's own), or `stable` or `nightly` to watch that one; an AppImage then offers to switch to it ([updates.md](updates.md#switching-channels)). Stable notifies once per new version, remembered per channel; nightly only shows it in Settings. Development, source and Flatpak builds never check ([updates.md](updates.md)) |
| `preferences.unlockCredentialsAtStartup` | Optional, default `true`: read saved keychain S3 credentials once when put.gd starts. A locked keyring shows its own unlock prompt then; uploads and shortcuts never unlock |
| `preferences.appearance` | Optional. `theme`: `system` (default, also when absent) follows the desktop's light or dark preference, `dark` or `light`. `accent` (null) and `overrides` (`{"dark": {}, "light": {}}`) are reserved for custom colours (#197) and must keep those values. See [appearance](appearance.md) |
| `preferences.editorRecentColors` | Optional, written by put.gd: up to 6 lowercase `#rrggbb` custom editor colors, most recent first |
| `preferences.diagnostics.debugLogging` | Optional, default `false`: keep debug records in the persistent log. put.gd sets it back to `false` 24 hours after it was turned on ([diagnostics.md](diagnostics.md#debug-logging)) |
| `preferences.diagnostics.packageFolder` | Optional absolute folder, or `null`: where **Create diagnostics package** and **Save diagnostics for this upload** save packages. Settings writes it when you choose a folder; omitted or `null` means `<archive root>/diagnostics/` ([diagnostics.md](diagnostics.md#diagnostics-package)) |
| `preferences.diagnostics.crashDumps` | Optional, default `false`, Windows only: a crash of `putgd.exe` writes a minidump to `%LOCALAPPDATA%\put.gd\crashdumps\` (the newest five are kept). Dumps stay on the PC and are never put in a package ([diagnostics.md](diagnostics.md#crash-dumps)). The Linux build accepts and ignores it |

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
own `publicationMode`. Its shortcut is always its own setting. Explicit action routes
remain exceptions when the default changes. Documents without `captureDefaults` retain
explicit per-action routing; changing capture output in the GUI adopts the optional shared
default. This extension remains schema version 1; older strict readers that do not support
it reject the new field, so use matching application/CLI/MCP versions.

The first three shortcuts default to `Ctrl+Shift+1`, `Ctrl+Shift+2`, and `Ctrl+Shift+3`.
Clipboard sharing, Capture region & edit and screen recording start unbound, allowing
explicit setup on GNOME. The recording key toggles a recording (start a ready one, stop
a running one) so the keyboard can end a capture without reaching for the bar. Availability and
actual shortcut bindings must be reported by the native adapter; configuration validation
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
can refresh preferences and request shortcut rebinding. Operations use immutable snapshots;
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
must validate. GUI forms autosave finished changes through the same revision check. Dirty drafts survive
external acceptance and require explicit reload or reconciliation; focusing an unchanged
field issues no save. Invalid fields retain local feedback while independently valid fields
may save. A transport/write failure blocks automatic replay until Retry saving is pressed.

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
is limited to 1024 bytes. A separator is added only when a nonempty prefix lacks one.
`storageClass` accepts STANDARD, REDUCED_REDUNDANCY, STANDARD_IA, ONEZONE_IA,
INTELLIGENT_TIERING, GLACIER, DEEP_ARCHIVE or GLACIER_IR. Cold storage may not support
immediate public GET. `aclMode` is public-read or omit; no error changes that choice.
`publicUrlBase` is null or an absolute HTTPS URL with an optional already-encoded
path and no credentials, query or fragment (literal-loopback HTTP is also accepted).
Null derives an object URL from endpoint/bucket/addressing. `addressing` is path,
virtual or auto; virtual rejects dotted buckets and literal IP endpoints, and auto
uses virtual only for an AWS host with an undotted bucket. Path mode is required for
the development dotted bucket. Validation performs no network or secret-store IO.
For S3-compatible services, the endpoint must support conditional writes. Cloudflare
R2 works with its account endpoint, region `auto`, ACL omit and the bucket's r2.dev
or custom domain as `publicUrlBase`. Backblaze B2's S3 API lacks conditional writes,
so Test connection fails there; use the `backblaze-b2` provider instead. See
[S3-compatible services](s3.md#s3-compatible-services).

`backblaze-b2` profiles (#282) hold exactly `apiUrl`, `bucket`, `objectPrefix`,
`linkMode` (`public` or `authorized`), `linkValidityDays` (1–7), `publicUrlBase`
(null or HTTPS, public links only), `connectTimeoutMs` and `requestTimeoutMs`.
Credentials are a keychain or session reference to the application key ID and key.
See [Backblaze B2](b2.md).

`connectTimeoutMs` bounds connection setup. `requestTimeoutMs` maps to the SDK whole
Curl-request timeout (`httpRequestTimeoutMs`). `lowSpeedBytesPerSecond` maps to the
SDK low-speed limit, and `lowSpeedTimeSeconds` maps to its low-speed duration
(`requestTimeoutMs` in SDK terminology). These settings are positive integral values
up to 2147483647. The adapter bounds concurrent SDK resources and caches clients by
effective settings plus explicit credential generation. A changed accepted profile
creates another effective client; pending operations and deletion retain originals.
See [S3 behavior](s3.md) and [credential setup](credentials.md).


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
