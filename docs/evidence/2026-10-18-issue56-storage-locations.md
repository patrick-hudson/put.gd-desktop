# Single-name configuration and data folders (issue #56) — 2026-10-18

Candidate `d37a9751a4de65d04a0a4271c7363425ff58d3e0` (tree
`2df8c546badf4d477d6dc170618ccc0852197cd9`, build
`0.1.1-dev.342+gd37a9751a4de`, not dirty) on
`claude/issue-56-config-paths`, cut from `nightly` at `70722c3`. The behavior
is described in [configuration](../configuration.md#locations).

## Suites

| Candidate | Where | Result |
| --- | --- | --- |
| `d7366b8` | Mint 22.3 workstation, dev preset (GCC 13.2, Qt 6.4.2) | 72/73: `history_contract` failed |
| `d37a975` | Mint 22.3 workstation, dev preset | 73/73 (671 s) |

The first full run found a regression in `d7366b8`: reading a history record's
`previewPath` through the non-const `QJsonObject::operator[]` inserted
`"previewPath": null` into schema-1 records that never had one. That broke
`populatedLegacyMigrationPreservesReceipts`. `d37a975` reads through `value()`.
The failing log is kept privately with the native evidence.

`storage_locations_integration` (12 cases) covers:

- a fresh install;
- an upgrade of a profile written by real services. The profile holds accepted
  settings, a failed upload with its retained artifact and preview, an
  acquisition interrupted before its items were journaled, a crashed resident's
  journal lock, and the recording helper's sibling folder;
- a live earlier journal lock: `storage_busy`, and nothing moves;
- both roots in use: both trees are byte-identical afterwards and both legacy
  paths are reported;
- per-root decisions;
- a stop before each rename of the move, followed by a normal start;
- a staged entry that already exists in the new root;
- a linked legacy root;
- the Windows-style shared root;
- the real `putgd` and `putgd-cli` binaries, first on an upgraded and then on a
  retained layout.

After the upgrade, both queued uploads retry and upload from the new folder
without reading the source again.

Two deliberate mutations were run and reverted:

- Returning journal paths unchanged fails the upgrade case: the artifact path
  still names `d/putgd/putgd`.
- Skipping the staged-move resume fails the interruption case once the earlier
  tree has been renamed to staging.

The restored source passes all 12 cases.

## Native Cinnamon check (workstation desktop)

Mint 22.3 Cinnamon/X11 on the workstation's own `:0` session. The check used a
private synthetic profile: its own `XDG_CONFIG_HOME`, `XDG_DATA_HOME` and
`XDG_CACHE_HOME`, plus a short private `XDG_RUNTIME_DIR` under `/run/user/1000`.
The socket paths were 48 bytes, checked with
`tools/diagnostics/check_socket_paths.py`. The user's own resident and profile
were not touched.

1. The installed pre-fix build `0.1.1-dev.336+g625b7ff38bb0` created the earlier
   layout inside a private Xvfb with no session bus. It used a local-only
   configuration with every shortcut cleared. Two clipboard imports from the
   synthetic desktop fixture followed (text and a 96×64 image). The build wrote
   `c/putgd/putgd/config.json` and `d/putgd/putgd/{history.sqlite,artifacts,previews}`,
   and journaled the image preview as `…/d/putgd/putgd/artifacts/../previews/<id>.png`.
2. The candidate started on the Cinnamon desktop as the `cinnamon-core-x11`
   resident.
   - Diagnostics reported `locations.config` and `locations.data` as `migrated`
     at `c/putgd` and `d/putgd`.
   - `settings.get` reported `c/putgd/config.json`.
   - Both history records remained, and the image preview resolved to the moved
     file.
   - Both folders were byte-identical to the earlier trees (journal lock
     excluded). The earlier folders and the staging folder were gone, and both
     roots were mode 0700.
   - No shortcut was bound or requested.
3. Settings → Advanced showed the new configuration path and the diagnostics
   `locations` block. History showed both captures with the image thumbnail.
   `ui.quit` ended the resident with exit 0.

| Visual | Shows |
| --- | --- |
| [Advanced: config file](2026-10-18-issue56-storage-locations-raw/cinnamon-settings-advanced-config-file.png) | `…/profile/c/putgd/config.json` |
| [Advanced: diagnostics](2026-10-18-issue56-storage-locations-raw/cinnamon-settings-diagnostics-locations.png) | `journalReady`, `locations` both `migrated` |
| [History](2026-10-18-issue56-storage-locations-raw/cinnamon-history-migrated.png) | both migrated captures with the moved preview |

The [raw directory](2026-10-18-issue56-storage-locations-raw/) holds the run log,
diagnostics, settings metadata, history records and the before/after file hashes.
Visuals are window captures of put.gd's own window only. They come from
attempt 4, on `d37a975`.

All four attempts are kept privately under `work/i56-native/` with
`SHA256SUMS`, together with the driver scripts and the failing `d7366b8` suite
log.

- **Attempts 1–3 (`d7366b8`).** Each migrated correctly and captured Settings.
  - Attempt 1 then stopped because pyatspi aborted the driver: this session has
    no AT-SPI bus. Its test resident was quit through its own socket.
  - Attempt 2 replaced AT-SPI with synthetic clicks sent to put.gd's window.
    Synthetic wheel events did not scroll. History opens in the same window, so
    attempt 2's History capture missed.
  - Attempt 3 paged with scrollbar clicks and passed completely.
- **Attempt 4 (`d37a975`).** It repeated attempt 3 on the final candidate and
  passed.

## Not covered

This change had no GNOME or KDE native runs, no build-server runs and no
package upgrade runs. The upgrade path through installed packages remains part
of issue #44. Windows was exercised only through the shared-root unit case.
