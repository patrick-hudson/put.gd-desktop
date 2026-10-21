# Capture region & edit (image editor)

Issue #51 adds a small editing workflow, **Capture region & edit** (the `edit` action): choose what to share, remove
private information or add a few marks, review it, then send only the finished
image through the normal archive/upload/history pipeline. This document is the
implementation contract and records what has been verified. It does not claim
desktop acceptance beyond the evidence listed at the end.

## User workflow

1. Invoke **Capture region & edit** from its global shortcut, the tray menu or the main
   window rail. By default the existing native region selector runs first; drag
   the area to edit. Escape cancels without creating an editor. To edit the
   clipboard image or an existing PNG/JPEG instead:
   - History's **Edit image** menu (**Edit clipboard image**, **Edit an image
     file…**) is the visible entry point.
   - On the rail, right-click **Capture region & edit** for the same menu, or
     drop one local PNG or JPEG file on the button. The rail keeps five
     matching buttons, so these are accelerators, not the only route.
   - The tray's **Edit image** submenu (**From clipboard**, **From a file…**)
     works too. Its file entry opens the main window's file picker, because the
     resident shows no dialogs of its own.
   - **Edit again** in a History item's inspector or **⋯** menu opens that item's
     retained local copy. It needs **Keep a local copy** to have been on when
     the item was captured. The 256 px history preview is never edited, and
     put.gd does not download an upload back from its destination. The result
     is a new history item; the original is left unchanged.
2. The editor opens with the selection fitted to the window. The result goes
   to the output configured for the edit action when the edit started.
3. Edit, then press **Upload** or Ctrl+Enter. Plain Enter never uploads: it
   finishes text entry or applies a pending crop.
4. Only the sanitized PNG becomes an artifact, history preview, local archive
   copy and upload. Normal progress, retry and link copying follow.
5. **Cancel**, closing the window (after confirming unsaved edits) or cancelling
   the operation stores and uploads nothing. A second edit invocation
   raises the open editor and reports `editor_active` with its operation ID.

The shortcut is unbound by default. Assign it under Settings → Shortcuts, where
the usual per-desktop availability and conflict reporting applies. Configurations
written before this feature inherit the region output route with no shortcut.

| Tool | Key | Behavior |
| --- | --- | --- |
| Select | V | Pick, move, resize (handles) or delete (Delete) a mark; arrows nudge, Shift×10 |
| Redact | X | Opaque black rectangle; covered source pixels are removed from the result |
| Blur | B | Box blur with a strength slider; **a visual effect only** |
| Crop | C | Drag the area to keep; Apply (Enter) or Cancel (Esc) |
| Arrow | A | Line with a filled head; color and Thin/Medium/Thick width |
| Rectangle | R | Outline; color and width |
| Highlight | H | Translucent colored marker, visually distinct from Redact |
| Text | T | One line of bold outlined text; choose any installed font family and a size from 8 to 200 image pixels |

You don't need Select to adjust a mark:

- Any drawing tool picks up what you can see under the pointer, and the cursor
  shows a move arrow there. Drag to move the mark, or click it to select it and
  edit its color, width or strength.
  - A rectangle is grabbed by its outline, so you can still draw inside it.
  - An arrow is grabbed by its line.
  - Redactions, blurs and highlights are grabbed anywhere they cover.
- Hold Ctrl to draw a new mark on top of an existing one.
- With Text, dragging a label moves it, and clicking it edits it.
- A selected text label has four corner handles. Dragging one scales the
  text around the opposite corner, and the Size box follows as you drag.

Next to the seven palette colors, the rainbow button opens a color picker for
any custom color. The last six custom colors you picked appear beside it as
quick picks, and put.gd keeps them between editing sessions
(`preferences.editorRecentColors`).

Choosing a different tool deselects the current mark, so its style controls set
the new tool's style and never restyle an earlier mark.

Undo (Ctrl+Z) and redo (Ctrl+Shift+Z or Ctrl+Y) are bounded. Ctrl+0 fits the
window, Ctrl+1 shows 100% for checking text and redaction edges, Ctrl+wheel
zooms, the wheel or Space/middle-drag pans. **Save a copy…** (Ctrl+S) writes the
same sanitized export as Upload to a user-chosen file; it never overwrites the
private source. The editor runs on Qt's built-in `generic` platform theme, which
keeps the desktop's integration libraries out of the image process. Its file
dialog is nevertheless the desktop's own: Save a copy asks the XDG portal's
FileChooser over D-Bus, so the chooser runs in the portal backend's process, and
Qt's widget dialog, themed like the editor, is only the fallback when no portal
answers ([file-dialogs.md](file-dialogs.md)). `editor.fileDialog` opens,
inspects or cancels it as the button does. The
helper also clears its desktop identity variables (`XDG_CURRENT_DESKTOP` and
similar), because Qt would otherwise still load the desktop's theme plugin; on
Plasma that plugin brings in Qt Quick and the GPU driver (about 85 MiB).

Use **Redact** for passwords and other private information. Blur and pixelation
can leave information that helps reconstruct or infer text.

## Sources

`action.invoke {"action":"edit"}` and the shortcut use the native region
selector. The UI entries above and agents (`editor.open`, or `action.invoke`
with `source`/`path`) open the other sources:

| Source | Behavior |
| --- | --- |
| `region` (default) | Existing native region selection on the current compositor |
| `window`, `desktop` | Existing native capture where the adapter supports it |
| `clipboard` | An image offer, or exactly one copied local image file |
| `file` | One absolute path to a static PNG or JPEG, read by signature; animated PNG is refused |

Inputs larger than **24 megapixels** are rejected before staging with
`edit_input_too_large`. Output is always PNG.

## Process and privacy boundaries

The resident never draws or decodes the editor image. It stages the input in a
new owner-only `<data>/editing/pge-XXXXXX/` directory (`input.img` 0400, empty
`output.png` 0600) and starts `putgd-ui --edit-image`. A capture is staged as a
fast-compressed PNG and the resident releases its raster; a file is copied
verbatim after a signature check, so a PNG or JPEG is decoded exactly once, in
the helper, after its pixel admission check and with its EXIF orientation applied. Data storage is
used rather than `/tmp`, which may be RAM-backed. The helper owns pixels and UI
only: no settings, providers, credentials or history. Closing it frees its image
regardless of other open put.gd windows. Nothing is preloaded or kept warm.

The helper uses the existing UiControl framing (version, strict sequence, size
and rate limits) with a separate payload role `image-edit`, a UUID nonce and
generation `1`. UiControl's known frame types gained `editing`, `finalizing`,
`command` and `commandResult` for this helper only:

1. `bootstrap`: role, nonce, generation, build, input (`input.img`) and output descriptors
   (path, device, inode, bytes, sha256), limits (imagePixels, outputBytes) and a
   destination label that `editor.inspect` reports to agents, and `recentColors`
   (up to six `#rrggbb`) from settings. Files are opened
   through a pinned directory descriptor with `O_NOFOLLOW`, identity and digest
   checks.
2. `ready` with the matching build, then `activate`; the helper decodes with an
   allocation limit and shows the window, then sends `editing`.
3. Agent commands: `command` (requestId, command, params) → `commandResult`
   (ok with result, or code and message).
4. `finalizing` when the user or an agent submits; the helper writes the export
   once, `fsync`s it, and sends `checkpoint` (bytes, sha256, width, height). A
   failure checkpoint carries `edit_cancelled`, `source_unsupported`,
   `input_excessive`, `edit_export_failed` or `image_job_io`. Every checkpoint
   returns `recentColors`; the resident saves a changed list to settings.
5. `checkpointAccepted`; the helper exits 0.

Startup is limited to 20 s and finalization to 120 s; editing itself has no
deadline. Each agent command must answer within 8 s, and at most 16 may wait.
The resident accepts the handoff only after a normal zero exit, unchanged pinned
input/output identities, the authenticated digest, and a structural check that
the PNG contains only IHDR, PLTE, tRNS, gAMA, cHRM, sRGB, pHYs, IDAT and a final
IEND with valid CRCs and no trailing bytes. Text, EXIF, ICC, time and private
chunks are refused. Those exact bytes become the operation's single immutable
artifact (`image/png`); they are not re-encoded, and retry reuses them.

## Sanitized export

Every edit is geometry in source-image pixels; no pixel snapshots are kept for
undo. Export renders once at full resolution into a fresh raster that carries
none of the reader's text, color-space or container data:

1. Copy the crop rectangle of the immutable base.
2. Overwrite every redaction rectangle with opaque black using Source composition
   and no antialiasing. Canvas drags round outward, so a partly covered pixel is
   covered.
3. Blur, reading only already-redacted pixels and only inside its own rectangle.
4. Draw arrows, rectangles, highlights and text.
5. Clear the hidden color of fully transparent pixels; encode PNG.

Two inputs that differ only inside a mask therefore export byte-identical PNGs,
with blur overlapping or adjacent to the mask, annotations over it and crop
through it (`editor_contract`). Previews, archives, uploads, Save a copy and the
agent preview all use this one render path. Source color spaces are converted to
sRGB on load.

The guarantee covers selected pixels and exported metadata. It cannot remove
information outside a mask, erase an original file, recall earlier uploads, or
guarantee forensic erasure from disks, swap or backups.

## Operation states and recovery

`edit` operations move through `accepted → acquiring → editing → acquiring`
(handoff) `→ stored → running → completed`. `editing` is persisted and is an
active state; the operation has no items until the finalized PNG is committed.
A resident restart turns `editing` into `interrupted` with `edit_interrupted`:
it is never resumed or uploaded, retry reports `artifact_missing`, and startup
removes all abandoned `<data>/editing` staging before new work. Cancellation
before submission stores nothing; after submission the normal operation
cancellation and unknown-publication reconciliation rules apply.

A failed upload retries the finalized bytes: the retry neither reacquires the
source nor reopens the editor, and its artifact keeps the submitted digest.
Before an editor starts, a failed write to private staging (a full disk or a
quota) reports `edit_staging_failed`, not `source_unsupported`.
`editor_job_integration` covers a helper killed while editing, a helper that
never becomes ready or hangs while finalizing (startup and finalize timeouts),
a non-PNG export, a digest mismatch, and a full disk. Each fails closed: no image
is handed off, staging is empty and the helper is gone.
`edit_operations_integration` covers the failed-upload retry.

## Why a custom canvas

Issue #51 proposed evaluating kImageAnnotator (the ksnip annotation library)
first, with a small custom Qt canvas as the fallback. put.gd uses the custom
canvas. kImageAnnotator was not built or benchmarked. The decision rests on its
inspected source, license and packaging documentation, and on the custom canvas
meeting every redaction and memory gate in the
[acceptance receipt](evidence/2026-10-17-issue51-edit-and-upload.md).

- **Licensing.** kImageAnnotator and kColorPicker are LGPL-3.0, and their
  Windows builds are documented as static only. Statically linking LGPL code
  into this MIT-licensed app would add relinking obligations to the Windows
  package. ShareX.ImageEditor, Greenshot and Flameshot are GPL-3.0, which this
  project cannot adopt without relicensing.
- **Memory.** kImageAnnotator's export renders a full ARGB image, crop undo
  keeps both the original and the cropped pixmap, and its undo stack has no byte
  accounting. Here the document holds one immutable base raster, undo is
  geometry-only and bounded (200 steps, 4 MiB), and export renders once, at
  finalization.
- **Redaction guarantee.** Redaction geometry is separate from annotations and
  overwrites source pixels before anything else renders, so one short export
  path holds the guarantee. With a third-party scene, every library update would
  need that path re-audited.
- **Dependencies.** Nothing new to package; the Qt 6.4.2 floor and distro Qt
  linking are unchanged.

The cost is owning selection handles, text entry, coordinate mapping and
accessibility, so the tool set stays deliberately small. Greenshot, ShareX and
Flameshot informed the interaction design; no code was copied from them.

## Agent and MCP control

Every editor capability is available through session methods, the production
`putgd-cli` and the optional MCP helper. Changes appear live in the open window.

| Session method | MCP tool | Parameters / result |
| --- | --- | --- |
| `editor.open` | `open_editor` | optional `source`, `path`; returns `operationId` |
| `editor.inspect` | `get_editor` | `id`; size, `crop`, `shapes`, undo/redo, `phase`, destination |
| `editor.apply` | `edit_image` | `id`, `commands` (1–100); atomic: all or nothing |
| `editor.preview` | `preview_editor` | `id`, optional `maxEdge` 64–1600; sanitized PNG (base64 / MCP image) |
| `editor.saveCopy` | `save_editor_copy` | `id`, absolute `.png` `path`, optional `overwrite` |
| `editor.submit` | `submit_editor` | `id`; same as Upload |
| `editor.cancel` | `cancel_editor` | `id`; discard without confirmation |
| `editor.raise` | `raise_editor` | `id`; bring the window to front |

Commands: `add {shape}`, `update {shapeId, shape}`, `remove {shapeId}`,
`crop {rect}`, `resetCrop`, `undo`, `redo`. Rectangles are `[x, y, width, height]`.

| Shape kind | Geometry | Optional style |
| --- | --- | --- |
| `redact` | `rect` (clamped to the image) | none; always opaque black |
| `blur` | `rect` | `strength` 2–64 |
| `arrow` | `from`, `to` `[x, y]` | `color` `#RRGGBB`, `width` 1–64 |
| `rectangle` | `rect` | `color`, `width` |
| `highlight` | `rect` | `color` |
| `text` | `at` `[x, y]`, `text` (one line, ≤500) | `color`, `fontPixels` 8–200, `font` (family name ≤100 printable characters; `""` is the default sans-serif) |

```sh
putgd-cli editor.open '{"source":"file","path":"/home/me/Pictures/screen.png"}'
putgd-cli editor.apply '{"id":"OPERATION-ID","commands":[{"op":"add","shape":{"kind":"redact","rect":[40,30,80,20]}}]}'
putgd-cli editor.submit '{"id":"OPERATION-ID"}'
```

Commands sent while the window is still opening return `editor_starting`; retry
them. After submission or discard they return `editor_finalizing`. A preview
contains real image pixels: it is returned only to the explicitly requesting
same-user client and never appears in diagnostics.

## Memory

The editor process exists only while an edit is open. Approximate raster cost is
R = width × height × 4 bytes (31.6 MiB for 4K, 91.6 MiB for 24 MP).

| Owner | Bound |
| --- | --- |
| Resident | Acquired raster until staged, then none; finalized PNG during handoff |
| Helper steady state | One base raster R, Qt backing store, ≤64 MiB display blur tiles (each ≤1.5 MP) |
| Undo/redo | Geometry snapshots; ≤200 steps and ≤4 MiB, oldest step evicted first |
| Export peak | Base R + crop-sized render ≤R + PNG encoder buffers |

The provisional target from #51 is at most 3R + 96 MiB incremental peak across the
process family. It has not been measured yet; see verification below.

## Platforms

Editing, export, commands and the resident integration are portable C++20/Qt
Widgets. The helper transport uses Unix file descriptors; on other platforms
`EditorJob` reports `editor_unavailable` and capabilities report the editor as
unavailable. No Windows support is claimed. On GNOME the extension binds the
`edit` shortcut and prepares a native region selection; KDE and Cinnamon use
their existing generic shortcut and region routes. KDE Wayland has no window
capture, so `source: window` fails there as it does for ordinary captures.

## Verification

Reproduce with the dev preset:

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev -R '^(editor_contract|editor_job_integration|edit_operations_integration|native_tray_integration|gui_settings)$'
PUTGD_CLI="$PWD/build/dev/putgd-cli" uv run --project tools/mcp --locked python -m unittest discover -s tests/integration -p test_mcp.py -v
```

`editor_contract` covers redaction invariance, tiny/edge masks, transparency,
metadata canaries, the PNG chunk inspector, bounded history, gestures, the
command contract and canvas interaction. `editor_job_integration` drives the real
product helper offscreen with agent commands, preview, save, submit, cancel,
termination, oversize admission and a missing helper. `edit_operations_integration`
checks that an uploaded edit leaves no unredacted pixels in any retained raster,
that cancellation stores nothing, file sources, single-session raising and
restart recovery. Synthetic screenshots of the themed window are produced by
`putgd-redesign-preview` (`20-`–`25-editor-*.png`). Offscreen results are not
desktop acceptance. Graphical, memory and MCP evidence is in the
[2026-10-17 receipt](evidence/2026-10-17-issue51-edit-and-upload.md), and the
lab drivers are described in the [VM lab guide](vm-lab.md#editor-acceptance-drivers).
