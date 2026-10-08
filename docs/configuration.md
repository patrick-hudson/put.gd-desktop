# Configuration

The resident app owns one `config.json` under Qt's `AppConfigLocation`. GUI and CLI
report the exact path. No separate YAML file or preferences database exists.
The schema is [config.schema.json](../schemas/config.schema.json). The shared
`SettingsService` is the authoritative validator, including registered provider rules,
absolute paths, shortcut syntax, and cross-profile references.

The resident JSON CLI uses this service; full settings GUI/native integration belongs
to its subsequent implementation units. Headless evidence is not desktop acceptance.

The entire document is required and unknown fields are rejected. Schema version 1
has no predecessor to migrate. Unsupported versions stay untouched for explicit recovery.
Maximum configuration size is 1 MiB. Missing or invalid configuration on first startup
means setup; it never selects the development destination. An invalid later edit preserves
the last accepted in-memory settings and the bad file so it can be corrected.

| Field | Meaning and default |
| --- | --- |
| `schemaVersion` | `1` |
| `actions` | Exactly `region`, `window`, `desktop`, `clipboard` |
| Each action's `destination` | `upload` by default; `local` saves and copies a local result |
| Each action's `profileId` | A configured profile ID, or `null` until destination setup |
| Each action's `shortcut` | One Qt PortableText combination; empty means unbound |
| `profiles` | At most 32 provider destinations; empty on fresh startup |
| Each profile's `id`, `label`, `provider` | Unique stable ID, display label, registered adapter ID |
| Each profile's `settings` | Non-secret settings validated by its provider adapter |
| Each profile's `credentials` | `keychain` plus a nonempty `reference`, or `session` plus `null` reference |
| `archive.keepUploads` | `false`; enables a local copy before remote publication |
| `archive.root` | User Documents folder plus `put.gd`; must be absolute |
| `archive.folders` | `images`, `videos`, `text`, `other`; each absolute or `null` |
| `naming.mode` | `generated` by default; `original` preserves regular-file basenames |
| `naming.collisionPolicy` | `rename` by default; `skip` or explicit `overwrite` |
| `limits.maxFiles` | 100; maximum clipboard batch file count |
| `limits.pendingOperations` | 8 |
| `limits.concurrentUploads` | 1 |
| `limits.maxFileBytes`, `maxBatchBytes`, `pendingArtifactBytes` | 2 GiB each |
| `limits.clipboardTextBytes` | 16 MiB UTF-8 |
| `limits.imagePixels`, `imageBytes` | 100 million pixels and 512 MiB |
| `retention.failedArtifactDays` | 7; zero disables age expiry, not the pending-byte budget |
| `retention.recentActivityLimit` | 100 visible recent entries; does not prune history |
| `retention.previewCacheBytes` | 256 MiB; zero disables retained previews |
| `retention.historyRetentionDays` | `null` means no automatic record expiry |
| `preferences.notifications`, `launchAtLogin` | `true`, `false` |

The first three shortcuts default to `Ctrl+Shift+1`, `Ctrl+Shift+2`, and `Ctrl+Shift+3`.
Clipboard sharing starts unbound, allowing explicit setup on GNOME. Availability and
actual shortcut bindings must be reported by the native adapter; configuration validation
does not claim a successful grab or resolve conflicts by taking another app's binding.

Unset archive overrides resolve to `<root>/<category>`. Video routing handles existing
regular files; configuration defines no recording actions. Filesystem availability is checked
when saving output, so an unavailable archive folder does not prevent accepting unrelated
configuration or block a requested upload.

The service watches both the file and parent directory, debounces notifications for 75 ms,
and re-arms after atomic replacements. If watching fails, it rescans every two seconds.
Content SHA-256 revisions deduplicate accepted changes. Metadata distinguishes the
accepted revision from observed file bytes and reports validation/watch status.
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
must validate. GUI forms retain dirty drafts across external acceptance and require reload
or rebase before saving; that GUI behavior is implemented in the settings unit.

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
