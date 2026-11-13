# Capture region & edit (image editor)

Issue #51 adds a small editing workflow, **Capture region & edit** (the `edit` action): choose what to share, remove
private information or add a few marks, review it, then send only the finished
image through the normal archive/upload/history pipeline. This document is the
feature contract and records what has been verified. It does not claim
desktop acceptance beyond the evidence listed at the end.

## User workflow

1. Invoke **Capture region & edit** from its hotkey, the tray menu or the main
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

The hotkey is unbound by default. Assign it under Settings → Hotkeys, where
the usual per-desktop availability and conflict reporting applies. Configurations
written before this feature inherit the region output route with no hotkey.

| Tool | Key | Behavior |
| --- | --- | --- |
| Select | V | Pick, move, resize (handles) or delete (Delete) a mark; arrows nudge, Shift×10 |
| Redact | X | Opaque black rectangle; covered source pixels are removed from the result |
| Blur | B | Box blur with a strength slider, or **Pixelate** (Style: Blur / Pixelate): square blocks of the chosen size; both are **a visual effect only** and carry the same warning |
| Crop | C | Drag the area to keep; Apply (Enter) or Cancel (Esc) |
| Arrow | A | Line with a filled head; color and Thin/Medium/Thick width |
| Line | L | Straight line without a head; color and width; Shift keeps it at 0°, 45° or 90° while drawing or dragging an end |
| Rectangle | R | Outline; color and width; Shift draws a square |
| Ellipse | E | Outline; color and width; Shift draws a circle |
| Pen | P | Freehand stroke with light smoothing (a short moving average, then points within half a pixel of the line are dropped; at most 2,000 points); color and width; a click leaves a dot |
| Highlight | H | Marker paper: the box becomes a band of the marker colour and the text keeps the contrast it had, as dark lettering on the band, on light and dark pages alike; photos get a luminance-keeping wash; snaps to text lines (Alt for a free box); click a word, double-click a line, Shift-click adds a word |
| Text | T | One line of bold outlined text; choose any installed font family and a size from 8 to 200 image pixels |
| Step number | N | Click to drop a numbered disc (40 image pixels across by default, 16–256). Numbers count up from 1; **Next number** sets the next one, and following clicks count on from it. Deleting a step leaves the others as they are; undo, redo and agent edits count on from the highest step left. The disc takes the palette colour with a white label and rim, or a near-black one where white would fall under 3:1 (yellow, orange, green, white). Dragging a corner resizes it and keeps it round; a selected step's field edits its number |
| Copy text | G | Reads the text in the image so it can be selected and copied, and lists the QR codes it finds. It is the last tool, after Blur and behind its own separator, and it adds no mark. See [Copy text and Quick redact](#copy-text-and-quick-redact) |

You don't need Select to adjust a mark:

- Any drawing tool picks up what you can see under the pointer, and the cursor
  shows a move arrow there. Drag to move the mark, or click it to select it and
  edit its color, width or strength.
  - A rectangle or ellipse is grabbed by its outline, so you can still draw inside it.
  - An arrow, a line or a pen stroke is grabbed by its line.
  - Redactions, blurs, highlights and steps are grabbed anywhere they cover.
- Hold Ctrl to draw a new mark on top of an existing one.
- With Text, dragging a label moves it, and clicking it edits it.
- A selected text label has four corner handles. Dragging one scales the
  text around the opposite corner, and the Size box follows as you drag.
- Drawing, moving and resizing stop at the edge of the (cropped) image. A
  press in the margin starts the mark on the nearest edge, and a text click in
  the margin opens no label.

### Highlight

The highlight is a pixel filter, not a translucent paint, and it looks like a
marker on paper whatever the page. Inside the box the page becomes a band of
the marker and every pixel keeps the contrast it had with its page, as dark
lettering on that band:

- The marker's **pigment** is the colour as picked, mixed with white until its
  luminance reaches 0.45 (so black text reads on it at 8.7:1 or better) or
  dimmed to 0.69 when it is paler than that (so a white marker still shows on
  a white page). A black or near-black marker (every channel at or below 32)
  skips that and is a fixed mid grey, #959595: a mark, not a redaction, with
  black text at 7:1.
- The **band** is that pigment multiplied onto the page, after lifting the page
  with white to luminance 0.85 if it is darker. On white the band is the
  pigment itself (yellow stays #FACC15), on grey panels it is darker than the
  panel, on dark pages it is the pigment at paper brightness. One continuous
  rule, no light/dark page class and no seam between the two.
- Each pixel's **lettering** is the first point on the band-to-black ramp that
  reaches the contrast the pixel had with its page (a knee below 1.04:1 keeps
  page noise and gradients as band). The mapping depends on the pixel's own
  luminance only, so anti-aliased edges stay smooth, LCD subpixel fringes
  collapse to neutral and no glyph gets heavier. Light text on dark pages
  becomes dark text; coloured text becomes near-neutral dark text.
- A box with no dominant background (a photo, a gradient) gets a **wash** that
  keeps every pixel's luminance: the colour moves 60 % toward the marker and is
  scaled back to its own brightness, so text on a photo or a coloured label
  keeps its exact contrast. Black and white markers have no colour to lend and
  shade or lighten the region instead, keeping contrast ratios (white only as
  far as the brightest pixel in the box allows).

**Tuning the colours.** The whole palette treatment hangs on three constants
in `src/editor/HighlightFilter.h`, and the one that matters is
`pigmentFloor` (0.45): the luminance every chromatic marker darker than that
is lifted to with white (black and near-black markers, every channel at or
below 32, take the fixed grey instead). It decides how pastel the dark swatches come out. Red #EF4444
is a coral at 0.45; at 0.55 it reads as pink (the council's first pick, rejected
as "not red"), at 0.30 it is close to the raw swatch. Lowering it makes the
colours truer and the band darker, so black lettering on it has less headroom:
on the darkest chromatic swatches (red, blue) black text reads at 10:1 on a
white page (the band is the pigment itself; 9.5:1 on an off-white dialog) and
8.7:1 on a dark page at 0.45, and 7.0:1 and 6.1:1 at 0.30 (the dark-page band
is the pigment multiplied onto a page lifted to 0.85). The
black marker bypasses the floor: its pigment is the fixed grey #959595
(luminance 0.30), so its band always sits at those 7.0:1 and 6.1:1 figures. Yellow (luminance 0.6356) is above
the floor at any of these values and never changes. `pigmentCeiling` (0.69) dims pale markers so white still shows
on a white page, and `pageFloor` (0.85) sets how bright a dark page is lifted
before the pigment multiplies it; neither has a reason to move. The editor catalog's sample document carries a yellow highlight, which sits
above the floor at any value below yellow's own luminance (0.6356; keep the
floor at or under 0.635), so a floor change alone leaves the catalog sample
unchanged. Inspect the CI catalog render for changes to the ceiling, page lift
or a floor above yellow, like any UI change.

The background estimate is block-local (16 px blocks, every block sampling a
full window inside the box), so a box across two panels or zebra rows treats
each part correctly. The canvas tile and the export run the same filter on
the same source pixels through the same per-background tables and agree
exactly at full scale. The look was settled by a design council (Sol, Astra,
Opus and Gemini proposals, three review gates on real exports; see
put.gd-evidence `2026-11-07-highlight-council`).

The same ink mask drives **text snapping**. Dragging snaps the box to the text
lines it crosses, trimmed to the ink inside the dragged width, with a small pad;
a drag through the middle of a line grows to the whole line height. Hold Alt
to keep the free box. Clicking highlights the word under the pointer,
double-clicking grows it to the line (stopping at column gaps wider than two
and a half line heights), and Shift-click adds the word under the pointer to
the selected highlight. Subpixel-rendered text (LCD filtering) is handled: the
filter never classifies ink colours, only distances from the background, which
is why the luminance rule replaced a per-ink contrast cap during research
(see put.gd-evidence `2026-11-06-highlight-real-captures`).

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
can leave information that helps reconstruct or infer text; Pixelate shows the
same warning as Blur ("Pixelation may be reversible.") with **Choose Redact tool**.

**Copy image** (Ctrl+Shift+C, beside **Save a copy…**) puts the same sanitized
PNG that Upload and Save a copy produce on the clipboard. It uploads nothing,
creates no History item and keeps editing open; the button reads "Image copied"
briefly, and a failure shows its reason above the footer. See
[Copy image](#copy-image) for the process boundary.

## Sources

`action.invoke {"action":"edit"}` and the hotkey use the native region
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
`command`, `commandResult`, `copyImage` and `copyResult` for this helper only:

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
   Copy image: `copyImage` (copyId, bytes, sha256, width, height) → `copyResult`
   (copyId, ok, or code and message); see [Copy image](#copy-image).
4. `finalizing` when the user or an agent submits; the helper writes the export
   once, `fsync`s it, and sends `checkpoint` (bytes, sha256, width, height). A
   failure checkpoint carries `edit_cancelled`, `source_unsupported`,
   `input_excessive`, `edit_export_failed` or `image_job_io`. Every checkpoint
   returns `recentColors`; the resident saves a changed list to settings.
5. `checkpointAccepted`; the helper exits 0.

Startup is limited to 20 s and finalization to 120 s; editing itself has no
deadline. Each agent command must answer within 8 s, and at most 16 may wait;
a command past its deadline answers `editor_timeout`, and the helper's late
answer to it (a large Save a copy or Copy image still finishing) is dropped
rather than ending the edit.
The resident accepts the handoff only after a normal zero exit, unchanged pinned
input/output identities, the authenticated digest, and a structural check that
the PNG contains only IHDR, PLTE, tRNS, gAMA, cHRM, sRGB, pHYs, IDAT and a final
IEND with valid CRCs and no trailing bytes. Text, EXIF, ICC, time and private
chunks are refused. Those exact bytes become the operation's single immutable
artifact (`image/png`); they are not re-encoded, and retry reuses them.

## Copy image

On Wayland the editor process cannot own the clipboard, so Copy image goes
through the resident's native clipboard adapter, the one that copies links:

1. The helper renders the sanitized PNG from a snapshot, off the GUI thread,
   exactly as Save a copy does, and on the same worker writes it once to a new
   owner-only `copy.png` (0600, `O_EXCL|O_NOFOLLOW`, fsynced) through its
   pinned staging directory descriptor, then sends `copyImage` with a new
   copy ID, its size, digest and dimensions. One copy runs at a time; Upload,
   Cancel and Save a copy wait for it, at most 30 s: without an answer by then
   the copy fails in the window (and to an agent whose `editor.copy` is still
   waiting; one that passed the 8 s command deadline has already had
   `editor_timeout`, and the clipboard may still change after it), and a later
   answer for that copy ID is ignored. A new copy sent while the resident is
   still delivering the earlier one is refused with `editor_busy`.
2. The resident opens and removes `copy.png` with `openat`/`unlinkat` through a
   descriptor of the staging directory whose identity matches the one it
   created, checks owner, mode, size (at most 128 MiB), digest, the
   sanitized-PNG chunk structure and the 24-megapixel admission, and hands the
   bytes to the native adapter as an explicit copy (it supersedes pending
   automatic link copies, like History's Copy image). It answers `copyResult`
   with the same copy ID.
3. Nothing else keeps the bytes: no artifact, History item, archive copy or
   upload input, and the operation stays `editing`. A failed check or an
   unavailable clipboard (`clipboard_unavailable`) is reported to the window and
   the agent; editing continues.

## Sanitized export

Every edit is geometry in source-image pixels; no pixel snapshots are kept for
undo. Export renders once at full resolution into a fresh raster that carries
none of the reader's text, color-space or container data:

1. Copy the crop rectangle of the immutable base.
2. Overwrite every redaction rectangle with opaque black using Source composition
   and no antialiasing. Canvas drags round outward, so a partly covered pixel is
   covered.
3. Blur and pixelate, reading only already-redacted pixels and only inside their
   own rectangles. Pixelation blocks sit on a grid anchored at the mark's own
   top-left corner, so a crop never shifts them; each block is the average of
   its pixels inside the mark.
4. Draw arrows, lines, rectangles, ellipses, pen strokes, step numbers,
   highlights and text.
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
After the helper exits successfully, the resident persists `state=acquiring`,
`phase=queued` before putting the sanitized PNG in the worker queue. The editor
is closed; History says **Waiting to upload…**. `stored` begins only after the
worker commits an immutable artifact. Cancellation while queued still cancels
the submitted operation without reopening the editor ([#559 evidence](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-25-queued-editor-559/README.md)).

A resident restart turns `editing` into `interrupted` with `edit_interrupted`:
it is never resumed or uploaded, retry reports `artifact_missing`, and startup
removes all abandoned `<data>/editing` staging before new work. Cancellation
before submission stores nothing; after submission the normal operation
cancellation and unknown-publication reconciliation rules apply.

A failed upload retries the finalized bytes: the retry neither reacquires the
source nor reopens the editor, and its artifact keeps the submitted digest.
Before an editor starts, a failed write to private staging (a full disk or a
quota) reports `edit_staging_failed`, not `source_unsupported`.
`editor_job_integration` covers a helper that crashes while editing or hangs
while finalizing (the finalize timeout), a non-PNG export and a digest mismatch.
Each fails closed: no image is handed off, staging is empty and the helper is
gone.
`edit_operations_integration` covers the failed-upload retry.

## Why a custom canvas

Issue #51 proposed evaluating kImageAnnotator (the ksnip annotation library)
first, with a small custom Qt canvas as the fallback. put.gd uses the custom
canvas. kImageAnnotator was not built or benchmarked. The decision rests on its
inspected source, license and packaging documentation, and on the custom canvas
meeting every redaction and memory gate in the
[acceptance receipt](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-17-issue51-edit-and-upload.md).

- **Licensing.** kImageAnnotator and kColorPicker are LGPL-3.0. The original
  cross-platform evaluation also considered static-linking obligations; that
  packaging concern was historical. ShareX.ImageEditor, Greenshot and Flameshot
  are GPL-3.0, which this project cannot adopt without relicensing.
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

## Copy text and Quick redact

The Copy text tool (G) reads the text and QR codes in the image and lets the user select and
copy them. Quick redact, a menu in the Redact tool's panel, adds Redact marks over text of five
kinds. Both are specified in [text recognition](text-recognition.md#the-editor). This section is
what the editor itself guarantees.

- Choosing the tool starts recognition for the current revision. Nothing is recognized before
  that, and the editor never recognizes in the background. While the tool is active a new
  recognition revision starts recognition again by itself. With any other tool active it does not.
- Recognition reads the source pixels inside the current crop with every Redact mark already
  painted opaque black. Arrows, text, blur and highlight are not part of its input. **Blur does not
  hide text from it.** The panel says "Reads the original image. Marks you added are not included,
  and Blur does not hide text from it."
- The recognition revision changes only when a Redact mark or the crop changes, including by
  undo and redo. A new revision cancels a running job, clears the result, the selection and the
  code list, and rejects a result that arrives late. A word or code that touches a Redact mark is
  dropped from the result before it can be shown, selected, copied or returned by
  `editor.inspect`.
- Selection works like text in a document: press and drag, Shift+click, double-click for a
  line, Ctrl+A, and Ctrl+C while the canvas has focus. Escape clears the selection, and a second
  Escape returns to Select. Copy selection and Copy all text (codes included) write one plain-text
  clipboard copy through the resident, so a copy survives the editor closing, and a newer clipboard
  change by the user is not overwritten.
- The panel lists each code found with Copy, and Open link for a valid http or https URL. A
  URL's row shows the whole URL so it is in view before Open link is pressed, and Open link opens it
  at once with no dialog. A binary code shows its kind and length only.
- **Quick redact** adds ordinary Redact marks as one undo step and says "Added 3 suggested
  redactions" or "No matches found. Text can still be there.". It always carries "Suggestions only.
  Recognition can miss text.". It never says a page is safe. **Redact is what removes pixels.** If a
  Redact mark or the crop changes while Quick redact waits for recognition, it ends with
  `edit_stale` and adds nothing.
- Recognition runs in `putgd-ocr`, started by `putgd-ui --edit-image` as its own child. The
  editor process does not link Tesseract, Leptonica or ZXing-cpp.

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
| `editor.copy` | `copy_editor_image` | `id`; Copy image: the sanitized PNG on the clipboard; returns `copied`, `bytes`, `sha256`, `width`, `height` |
| `editor.submit` | `submit_editor` | `id`; same as Upload |
| `editor.cancel` | `cancel_editor` | `id`; discard without confirmation |
| `editor.recognize` | `recognize_editor_text` | `id`; starts recognition for the current revision, as choosing Copy text does. Returns `state` and `result`. `editor.inspect` then has a `text` member: `state`, `result` (the identity of this recognition result), `words`, `codes`, `truncated` and `selection`, in source pixels |
| `editor.selectText` | `select_editor_text` | `id`, `result`, and `from` with `to` (word indexes in reading order), or `all`, or `none` |
| `editor.copyText` | `copy_editor_text` | `id`, `result`, optional `all` (default: the current selection); returns `copyStatus` and the character count |
| `editor.copyCode`, `editor.openCode` | `copy_editor_code`, `open_editor_code` | `id`, `result`, `index` into the codes. Open is refused for anything but a valid http or https URL |
| `editor.raise` | `raise_editor` | `id`, optional `activationToken`; asks the desktop to bring the window to front and answers what it did: `raised: true` once the window is active, otherwise after 1.5 s `raised: false` with `reason: activation_refused` and a message (#911) |

Commands: `add {shape}`, `update {shapeId, shape}`, `remove {shapeId}`,
`crop {rect}`, `resetCrop`, `undo`, `redo`, `redactText {categories}`. Rectangles are `[x, y, width, height]`.
`redactText` is Quick redact: `categories` is 1 to 5 of `email`, `phone`, `ip`, `card` and `key`. It
must be the only command in its batch, or the batch is refused with `edit_command_invalid` before any
recognition. It answers when it is done, not at once, and returns the created shape IDs and counts per
category. A `result` that is no longer current is refused with `edit_stale` by every method that takes
one, before anything is copied.
`editor.view` also takes `blurStyle {style: blur|pixelate}` (the Blur tool's
Style buttons; it restyles a selected blur) and `stepNumber {number}` (Next
number, or the selected step's Number); its state reports `nextStepNumber`,
`style.blurStyle`, `style.stepSize`, `copying` and `lastCopy`.

| Shape kind | Geometry | Optional style |
| --- | --- | --- |
| `redact` | `rect` (clamped to the image) | none; always opaque black |
| `blur` | `rect` | `strength` 2–64 (the block size when pixelated), `style` `blur` (default) or `pixelate` |
| `arrow` | `from`, `to` `[x, y]` | `color` `#RRGGBB`, `width` 1–64 |
| `line` | `from`, `to` `[x, y]` (different points) | `color`, `width` |
| `rectangle` | `rect` | `color`, `width` |
| `ellipse` | `rect` | `color`, `width` |
| `pen` | `points` `[[x, y], …]`, 1–2000, drawn in order | `color`, `width`; `inspect` also reports its extent as `rect` |
| `step` | `at` `[x, y]`, the centre | `number` 1–999 (default: one more than the highest step), `size` 16–256 (diameter, default 40), `color` |
| `highlight` | `rect`, or `at` `[x, y]` with `snap` `word`/`line` | `color`; `snap` `none` (default, the rect is used as given), `text` (the rect snaps to the text lines it crosses), `word` or `line` (the mark is the word or line at `at`; `edit_command_invalid` "No text at that point" when there is none) |
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
unavailable. On GNOME the extension binds the
`edit` hotkey and prepares a native region selection; KDE and Cinnamon use
their existing generic hotkey and region routes. KDE Wayland has no window
capture, so `source: window` fails there as it does for ordinary captures.

## Verification

Reproduce with the dev preset:

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev -R '^(editor_contract|editor_job_integration|edit_operations_integration|native_tray_integration|gui_settings_[a-z]+)$'
PUTGD_CLI="$PWD/build/dev/putgd-cli" uv run --project tools/mcp --locked python -m unittest discover -s tests/integration -p test_mcp.py -v
```

`editor_contract` covers redaction invariance, tiny/edge masks, transparency,
metadata canaries, the PNG chunk inspector, snapping that never traces redacted
text, and new marks that never reveal redacted pixels in any order. `editor_job_integration`
drives the real product helper offscreen with agent commands, preview, save and
submit, and checks the failures above. `edit_operations_integration`
checks that an uploaded edit leaves no unredacted pixels in any retained raster,
that a failed upload retries only the edited bytes, that cancellation stores
nothing, and that an interrupted edit recovers without uploading. Highlight
paint, handles, keyboard editing, undo bounds, the drawing tools and the editor
window went in the second round of [removed tests](../testing/removed-tests.md#second-round). Synthetic screenshots of the themed window are produced by
`putgd-ui-catalog` (`20-`–`29-editor-*.png`; the step, pen, ellipse, line,
pixelate, Copy image and narrow-toolbar states are `30-`–`30k-editor-*.png`).
`editor_job_integration` and `edit_operations_integration` check Copy image
against the real helper and resident. Offscreen results are not
desktop acceptance. Graphical, memory and MCP evidence is in the
[2026-10-17 receipt](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-17-issue51-edit-and-upload.md), and the
lab drivers are described in the [VM lab guide](../lab/vm-lab/input-and-evidence.md#editor-acceptance-drivers).
