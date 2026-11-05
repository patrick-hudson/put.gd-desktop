# Tier 1 Aurora capture and Cinnamon editor rerun

[Code PR #965](https://github.com/patrick-hudson/put.gd-desktop/pull/965), head `0d742f20c81a8e7649b6d5787a0d513cd96bb49e`. Related issues #961 and #958 remain open.

Boat development evidence only, synthetic loopback destinations, no compilation. Both guests installed SHA256SUMS-verified published `v26.1030.3203-nightly`, app commit `cc9d9410d6e982dd93d58bcfda82f112c44a82f6`, tree `47d8ee85fb481fde7f557aff8f1c01e378f17ad3`, catalog CI41111404666. Exact packages, installed binary hashes, guest/run identities and driver file hashes accompany each receipt.

| Requested Tier 1 row | Result | Evidence |
| --- | --- | --- |
| Aurora region → upload | P | [receipt](aurora/receipt.json), [independent pattern](aurora/region-pattern.png), [600×280 uploaded crop](aurora/region-uploaded.png); pixel tolerance 0 |
| Aurora window + desktop → upload | P / P | [receipt](aurora/receipt.json), [1280×800 uploaded image](aurora/desktop-uploaded.png); both pixel tolerance 0 |
| Aurora Edit & upload | P | [four real marks](aurora/editor.png), [uploaded PNG](aurora/edited-uploaded.png), unchanged pixel oracle |
| Aurora copied link → Firefox | P | [browser content](aurora/browser.png), MIME restoration and navigation in [receipt](aurora/receipt.json) |
| Aurora history → remote delete | P | [saved deletion result](aurora/delete.png), exact IDs/effects and independent HTTP 404 in [receipt](aurora/receipt.json) |
| Cinnamon Edit & upload | P | [four real marks](cinnamon/editor.png), [uploaded PNG](cinnamon/edited-uploaded.png), [receipt](cinnamon/receipt.json), [pixel bounds](cinnamon/pixel-bounds.json) |

Aurora recording is **not in scope**, under #822/#959. Other Cinnamon rows were not requested for this rerun. No physical/native release acceptance is claimed.

## #961 cause and fix

AppRun's restricted entry was in the resident's private `XDG_DATA_HOME`, while KWin discovers the desktop session's real applications directory. Each restart changes the mounted executable. KWin's first per-connection ScreenShot2 refusal makes late registration ineffective. The driver now stages AppRun's exact running `/proc/<pid>/exe` match after every launch and refreshes the real service cache before recipes can capture. Every launch/relaunch returns through this hook before its first capture; late reuse of an already-refused resident is avoided by the fresh resident lifecycle.

The Aurora receipt records 16 launches and 32 exact mirrored filenames, including the recorder entry when present. [Cleanup proof](aurora/mirror-cleanup-proof.json) verifies all 32 are absent; inode/hash guards prevent deleting replaced or unrelated files. The native Secret Service owner, collections and default alias fingerprints are [unchanged](aurora/secret-service-after.json). No KWin permission store, permission, ownership or geometry guard changed. `uupd.timer` was masked. Guest: Aurora 45, Plasma/KWin 6.7.5, Qt 6.11.2, native Wayland 1280×800. Driver reference `56dc5e07e97b7e27f550d6d1fb8244b6ff227a96`; [source manifest](aurora/source-identity.json).

## #958 cause and fix

The immediate first motion raced delivery of the X11 press. With a held real press, official MCP `get_editor` inspected an arrow already created at `(29,30)` and a rectangle at `(350,30,1,1)` before any drag motion; zoom stayed 98%. The arrow remained anchored at `(29,30)` after motion. The app uses the press position (`src/editor/EditorCanvas.cpp:340` and `:386`), so no app fix was made.

The driver adds a 100 ms hold only for X11 editor drags. The uninstrumented rerun and the [held-press MCP diagnostic](cinnamon/d2-receipt.json) produce the same PNG SHA-256: `1073ffd040ca8061dc142a7f8e91348272a864b9a859cf78d05beadd494994da`. The unchanged oracle passes; actual arrow bounds `(27,28,250,94)`, rectangle `(350,30,590,110)`, text `(37,224,116,252)`, redaction `(360,189,551,281)`. Guest: Mint 22.3 Cinnamon/X11 with distro Qt 6.4.2, boat `bx_kz4vqmha`, run `7ecf6b5f-0fc2-4d51-aad4-e2baa839ffb4`. Final driver [manifest](cinnamon/source-identity.json) matches the changed files at code head.

The first diagnostic [failed before a shape drag](cinnamon/d1-receipt.json): its one-off SSH bridge dropped the MCP helper's JSON stdin. That failure and [frame](cinnamon/diagnostic-setup-failed.png) are retained. The corrected bridge was checked read-only before the successful held-press diagnostic. No pixel tolerance changed.

## Validation and retirement

[50 focused Python tests](focused-python.log) pass, including interrupted-hold release and modified/symlinked mirror cleanup refusals. Initial CI exposed a missing standalone-test import path and a startup fake that needed the new staging boundary mocked; both were corrected in a test-only follow-up. The exact CI entrypoints now pass [4 mirror tests](ci-entrypoint-appimage.log) and [93 driver tests](ci-entrypoint-driver.log). Production driver files are unchanged from the live-tested `9a1e527c` revision; the failed CI run is [retained](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/41159333429). Every final graphical receipt passes its evidence audit and private cleanup. Saved frames and functional PNGs are retained in checksum-verified private archives: Aurora 271 members, Cinnamon 226 members. The initial Aurora fork stalled during hydration before any VM boot; its deletion is recorded. Setup import-path and capacity refusals are preserved privately; Cinnamon's original failed capacity check and corrected check accompany the packet. Only generated compiler objects, static archives, ELF caches and ccache in its disposable snapshot copy were retired; test source/results and all goldens were preserved.

Both overlays were stopped and retired through guarded cleanup after archive verification. Cinnamon's first cleanup correctly [refused](cinnamon/cinnamon-cleanup-refused.json) an external golden root; the documented `--images-root` preserved that immutable path and [completed cleanup](cinnamon/cinnamon-cleanup.json). Aurora [cleanup](aurora/aurora-cleanup.json) completed too. [Session retirement](session-cleanup.json) confirms three sandbox deletions, at most two concurrent, owned tunnel stops and wall-tile removal. [Wall observations](wall-proof.json) and saved forwarded VNC visuals identify the live guests. Private full source/evidence archives are retained under `work/campaigns/surface-driver-20261029/tier1-aurora-editor-424f1689`.
