# Editor round 3 replay evidence

All 51 editor-owned interactions were replayed on each guest. These are **Boat development** results; visual comparisons remain flagged for review and do not establish physical/native release acceptance.

| Guest | Passed | App failure | Blocked |
| --- | ---: | ---: | ---: |
| Mint 22.3 / Cinnamon X11 v2 | 39 | 1 (#806) | 11 |
| Ubuntu26.04 / GNOME50 Wayland v3 | 22 | 0 | 29 (#805) |

Cinnamon blocks: #671 owns cancel/discard confirmation, save-copy, recovery chooser and window-close matching (5 rows); #663 owns preparation-failure, native custom-colour selection and cursor-shape witness (3); #810 owns line-width and recent-colour selectors (2); #718 owns the edge-drag/foreground block (1). The raw compact result is failed, classified separately as an input/WM block with its original verdict retained. Ctrl+1 after dismissing Zoom is the app failure #806, with original before/menu/failure frames in the sibling zoom packet.

GNOME passes its first 22 rows, then all remaining 29 stop with `fresh accessibility probe timed out` under current core #805. Its saved crop draft and failure frame show a separate Ubuntu error dialog over the editor. Whitelisted metadata identifies a gjs-console Signal5 crash; causality is not established. Both overlay-only extensions were ACTIVE, and v3 idle policy passed. No timeout, ownership or foreground guard was changed. The raw receipt preserves every outcome.

The corrected Cinnamon Help and destination rechecks pass. Saved-copy overflow opens a runtime QMenu, Copy path matches clipboard/readback exactly, and failure Details opens/closes; the recovery chooser then hits #671. Geometry steppers and both pan gestures pass independently. #789 has the complete classification.

## Exact identities

- Core tested: `7bc0ed71f1b907232509fba1e6248be14fcd783b`, including #767 and #785.
- Cinnamon full C2: recipes `40d659207e3ef77e886cf88ea0f91aafb16717d5`, run `0ed53e7d-23ea-4c4f-a194-72127dfde611`. Help C4: recipe-equivalent source `4a7b6df34de83500dc175c0bdf17ae6854a9d446`, run `82f1b682-7f67-4001-905c-4ab550f4153d`. Destination/recovery C5: recipes `96e522660289bc5d39144bb3025a44c119d0a57b`, run `d0270029-7c3b-4afe-b89f-d1d37412d12f`. Clean app `e003bc7983e0a567611121cda65fc1670395bebf`; DEB SHA256 `af961793cb4aab248820eb9a5230c7c4250abf46c062e6ec3c2da21e48512f60`. Boat `bx_wj5ckzft`.
- GNOME G1: recipes `96e522660289bc5d39144bb3025a44c119d0a57b`; all 225 staged source inputs match their SHA256 manifest. Run `2e040898-e526-40e5-9ba6-ddb0ead1fd27`, Boat `bx_rbbq5jv9`. Clean **native guest build** `b5613bfc3a6c8f85310a698e495ce1c2134f1cf2`, official pinned SDK1.11.907, distro Qt6.10.2. Build flags, clean source status and binary hashes are in [gnome-build.json](gnome-build.json). No 24.04 host DEB was used.
- Both outputs: 1920×1080, isolated local synthetic destinations, real QMP input and saved visual witnesses.

## Raw receipts and visuals

- [Cinnamon full raw receipt](cinnamon-full-raw.json), [Help recheck](cinnamon-help-raw.json), [destination/recovery recheck](cinnamon-final-recheck-raw.json).
- [GNOME full raw receipt](gnome-full-raw.json), [active extensions](gnome-extensions.txt), [idle/session preflight](gnome-preflight.json), [read-only Mutter witness](gnome-witness.json).
- [Help expanded](cinnamon-help.png), [destination disclosure](cinnamon-destination.png), [save failure Details](cinnamon-recovery-details.png), [unlocked GNOME](gnome-active.png), [GNOME crop/probe failure](gnome-crop-timeout.png), [Ubuntu error witness](gnome-environment-error.png).

Every frame above is original synthetic content. Full raw frames, QMP/session metadata, build/provision logs, source manifests, tested artifacts and failure attempts are retained privately under `work/editor-r3/` in worktree `t3-309cae63`, with verified archive hashes and a restore index. The lane coverage stores exact per-row receipt and source identities.

## Retirement

All five Cinnamon overlays were retired through guarded cleanup after export; [retirement receipts](cinnamon-retirement.json) and [completed sandbox deletion](cinnamon-deletion.json) are retained. GNOME's canonical-base guard refusal (#790) is preserved without a workaround, followed by verified sandbox deletion; see the [raw refusal](gnome-cleanup-refusal.txt) and [completed deletion](gnome-deletion.json). The immutable bases and named snapshots were not modified.
