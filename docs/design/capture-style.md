# Capture style

[#1893](https://github.com/patrick-hudson/put.gd-desktop/issues/1893) adds **Capture style**:
put.gd places a still capture on a styled canvas when it exports it. The canvas has padding
around the capture, a backdrop in that space, rounded corners on the capture and a soft shadow
under it. The captured pixels are not changed: the capture is placed on a larger image and its
corners are cut (see [Rendering](#rendering) for exactly which pixels are copied as they are).

It is an option, and it is **off by default**. With `preferences.captureStyle` absent, or with
`enabled: false`, no capture is styled, and every capture, upload and local copy is byte for byte
what it was before this feature. An edit is styled only when the user or an agent turns the style
on in that edit (the Style tool, an `editor.apply` `style` command, or a file opened with
`captureStyle: true`); an edit with no style exports exactly what it did before.
`capture_style_integration` checks captures with the option absent and off, and `editor_contract`
checks the editor export with no style.

## Where it applies

| Path | Rule |
| --- | --- |
| Region, window, monitor, desktop, Repeat last region, scrolling captures | Styled when the option is on, just before the capture is stored. This happens before the upload and before the local copy, so both get the styled PNG. Scrolling captures are included because they reach the same step as a PNG |
| Capture region & edit | While the option is on, a new capture (region, window, desktop, scrolling) opens with the style on in the editor's Style tool, and the editor's export draws it. So does **Redact and upload** from the sensitive text prompt, for a capture but not for a clipboard image or a file. The resident does not style an editor's output again |
| Files, the clipboard and Edit again in the editor | Open with the style off, except Edit again of a styled capture whose style-only `-original` still exists, which opens that original with the style on (see [Originals](#originals-and-edit-again)) |
| Clipboard images and text, file uploads, recordings | Never styled |
| Retries | Upload the stored artifact as it was stored |

## Settings

`preferences.captureStyle` is `{enabled, backdrop, padding, corners, shadow}`; see
[configuration](configuration.md). Settings stores the resolved values, never a preset name. A
preset is a full set of values, and Settings shows a preset as chosen when the stored values
equal it, so a later change to a preset never changes saved settings.

| Preset | Backdrop | Padding |
| --- | --- | --- |
| Graphite (selected when the option is first turned on) | Gradient `#383633` to `#1c1c1b` | Medium |
| Amber | Gradient `#f2c98a` to `#c9733a` | Medium |
| Paper | Colour `#efeee9` | Medium |
| Clear | None (transparent PNG) | Small |

Every preset has rounded corners and a shadow. A gradient runs from the canvas's top-left corner to
its bottom-right corner, so its angle follows the canvas's shape (45° below the horizontal on a
square canvas). Clear is never the default: a black shadow on transparency
disappears in dark chat apps, and apps that flatten alpha to black turn it into a black band. A
design council (Gemini Neon, Sol and Opus, 2026-11-22) chose the presets, the sizes and the
layout below.

Settings → Capture → **Capture style** sits after Capture delay. It has a **Style screenshots**
check box. While that is on, it also shows **Look** (the four presets or Custom), **Backdrop**
with its colours or picture (Custom only), **Padding**, **Rounded corners**, **Soft shadow** and a
**Preview**. The preview is a 320 × 200 render of a synthetic window, drawn with the light
theme's tokens, through the same renderer as the export. Changing a field of a preset switches
the Look to Custom.

### Picture backdrops

**Choose picture…** opens the desktop's file chooser for a PNG or JPEG of at most 64 MiB and
100 megapixels. It must be a regular file, not a symbolic link. `putgd-ui` decodes it once, at most
4,096 pixels on its longer side, and saves its own PNG as
`<data>/putgd/backdrops/<sha256>.png`. The setting stores that path. Moving or deleting the
original file changes nothing, and the resident never decodes the picture. The picture covers
the canvas: it is scaled to fill it, centred and cropped, and never stretched or tiled.

If the picture can't be read when a capture is styled (the copy was deleted, or an agent named a
path that is not an image), Graphite's gradient replaces the backdrop; the padding, corners and
shadow stay as set. Its History item says "The backdrop picture couldn't be read, so put.gd used
Graphite.", and the operation's `style` has `pictureMissing: true`. Settings says "The picture
can't be read, so Graphite is used until you choose another." under the preview, and the editor's
Style panel says so too.

The sensitive text check reads the capture before the backdrop is added, so text in a backdrop
picture is never checked. Choose a picture with nothing private in it.

## Geometry

Every size comes from the shorter edge `s` of the capture (after the editor's crop), so a small
region and a full screen both look balanced.

| Part | Rule |
| --- | --- |
| Padding | Small: 4% of `s`, 16–96 px. Medium: 7%, 24–160 px. Large: 11%, 40–256 px |
| Corner radius | 1.2% of `s`, 6–32 px, and never more than `s`/4; 0 with corners off |
| Shadow | Black at 38% opacity, blurred with sigma = padding / 6, moved down by padding / 8. It ends at about 5/8 of the padding, so it is never clipped |

A 1920 × 1080 capture with Medium padding becomes 2072 × 1232.

### Size limits

A styled capture must fit `limits.imagePixels`, `limits.imageBytes / 4` and 32,767 pixels on a
side. When it doesn't fit, the padding steps down a size at a time. When even Small doesn't fit,
the capture is stored as it was captured, and its History item says "Capture style wasn't applied:
the styled image would be larger than put.gd's image limits."

A styled PNG is larger than the capture alone. When the artifact store refuses the styled PNG for
its size (the Largest file, Largest batch or Space for queued files limits, or the destination's
maximum object size) or for free disk space, put.gd stores the capture as it was instead, once. If
that succeeds, its History item says "Capture style wasn't applied: the styled image was too large
to store or upload, so the capture was kept as it was." The private original made for styling is
removed first, so its disk space is free, and no style `-original` is kept. For a capture the
sensitive text check covered, what is stored is the covered PNG, and the original from before
covering is still saved locally under `sensitive.original` with Keep a local copy, as without
a style. If the image-job child can't draw the style for another reason (it fails or times out),
the capture is stored as captured, with "Capture style wasn't applied: put.gd couldn't draw it."
and a warning in the log. A capture that doesn't fit even unstyled fails as it would without a
style.

The editor keeps its styled export within the same image limits and its own 24-megapixel output
admission, stepping the padding down the same way; when no padding fits, it exports without a
frame, and the Style panel says so. An edited PNG over the output's byte limit keeps the edit open
and reports the error, as any edit does; the editor doesn't drop the style by itself.

## Rendering

One renderer, `src/core/CaptureStyleRender` (the library `putgd_capture_style_render`), draws the
export for every path, and the editor canvas draws its frame with the same backdrop and outline
geometry:

1. Fill the canvas with the backdrop (colour, gradient, the covering picture, or transparent).
2. Draw the shadow. A small blurred rounded square, whose size depends only on the sigma and the
   radius, is drawn as nine slices: its corners as they are, its edges stretched along the
   capture. The tile costs the same for a tall scrolling capture as for a small one; filling,
   drawing and encoding the canvas still grow with its size.
3. Fill the capture's rounded outline with the capture as a texture at an integer offset, with
   antialiasing. Every fully opaque pixel inside the outline, away from its antialiased edge, is
   copied unchanged; a translucent pixel (a window's transparent corner, an edited image with
   alpha) is drawn over the backdrop.
4. With a transparent backdrop, clear the hidden colour of fully transparent pixels, as the
   editor's sanitized export does.

The data and geometry (`src/core/CaptureStyle`, the library `putgd_capture_style`) are Qt Core
only, and the resident links them to validate settings and route work. The painting and the
picture decoding are in `putgd_capture_style_render`, which only `putgd-ui` links.

### Captures: the image-job `style` task

The resident never paints. On the acquisition worker it encodes the capture as the PNG the
artifact store would write, then runs `putgd-ui --image-job` with the task `style` and the style
object in the bootstrap (see [the image worker](image-worker.md)). The child decodes that PNG
(PNG only, within the image limits), works out the geometry within those limits, reads the picture
for a picture backdrop, renders, and writes the styled PNG through the usual private files. Its
checkpoint may carry `note: "picture_missing"`. The resident replaces the payload with the styled
PNG and, once the store has accepted it, records `style: {applied: true}` on the operation.

With **Check captures for sensitive text before uploading** also on, the check runs first, on the
capture as it was taken. Recognition and the covers never see a backdrop, so text in a backdrop
picture is never checked (and never flagged) and the cover positions never shift. The covered PNG is then styled
like any capture. After **Redact and upload**, the editor opens with the style on, and its export
is not styled again.

### The editor

The style is part of the document's state, beside the marks and the crop. It is in undo and redo,
and a change of style is one undo step. The export draws it last, around the cropped and marked
result, through the same renderer, so Upload, Save a copy, Copy image and `editor.preview` all
agree. A document that starts styled (a new capture) is not "Edited" until the style changes.

The **Style** tool (S) sits beside Crop, with the `capture-style` glyph. Like Crop, it changes the
document and adds no mark. On the canvas it picks and moves marks like Select. Its panel has
**Style this image**, **Look** (the presets, plus **Saved look** when the look saved in Settings is
not a preset) and **Padding**, with a line such as "960 × 600 px becomes 1044 × 684 px." The
footer reads "Document 960 × 600 px · Styled 1044 × 684 px".

With the style on, the canvas shows the styled frame, the same as the export: the backdrop and
shadow behind the image, and rounded corners over the marks. Fit includes the frame. While the
Crop tool is active the canvas shows the plain image, so the crop edges stay exact. Marks stay
inside the image: a press in the padding starts a mark at the nearest edge, as a press in the
margin always has.

## Originals and Edit again

With **Keep a local copy** on, the capture as it was taken is saved in the archive beside the
styled copy, with `-original` added to its name, the same rule as automatic covers
([checking captures before uploading](text-recognition.md#checking-captures-before-uploading)).
The resident writes it as a private PNG under `<data>/styled/` while the capture is stored, copies
it to the archive, and removes it. Startup clears anything left there. The original is never
uploaded. The operation records it as `style.original`, `{status, paths}`; `status` is `failed`
when it couldn't be saved. A capture the sensitive text check covered keeps no style original: its
only original is the one from before covering, recorded under `sensitive.original`, because the
covered PNG is what was styled.

The History item of a styled capture has `style: {applied: true}`, and `style.original`, the path of
that style-only original, when it was saved. **Edit again** needs the item's local copy, as
always. When `style.original` still exists, Edit again opens it with `captureStyle: true`, so the
editor starts with the style on (even if the option has been turned off since) and the result is
styled once. Otherwise (the original was not kept, or was deleted) it opens the styled copy with
the Style tool off, so frames never stack. A capture the sensitive text check covered and then
styled always opens its styled, covered copy with the Style tool off: Edit again never opens the
original from before covering. Its frame can't be changed in the editor; a new capture can.

## Agents

| Surface | Contract |
| --- | --- |
| `settings.patch`, MCP `patch_configuration` | `preferences.captureStyle` |
| `operation.inspect`, MCP `get_operation` | `style` on a capture the image job styled: `{applied: true}`, `pictureMissing` when Graphite stood in, and `original` (`{status, paths}`) when a style-only original was kept. A covered capture's original is under `sensitive.original`. An edit's export has no `style` member even when it has a frame |
| `editor.open`, `action.invoke` with `action: edit`, MCP `open_editor` | `captureStyle` (MCP `capture_style`): `true` starts an edit of a file with the saved style on; only with `source: file` |
| `editor.apply`, MCP `edit_image` | `{op: style, enabled?, preset?, backdrop?, padding?, corners?, shadow?}` with at least one field. `preset` is `graphite`, `amber`, `paper` or `clear`; it is applied first, sets the whole look and turns the style on, and the other fields then override single parts. A `backdrop` object merges into the current one (a `picture` path already there is kept for kind `picture` and dropped for another kind unless given). `enabled: false` turns the style off. A change is one undo step; a command that leaves the style as it was adds none. A backdrop of kind `picture` needs a `picture` path. Invalid values answer `edit_command_invalid` |
| `editor.inspect`, MCP `get_editor` | `style`: the resolved fields, `preset` (a preset ID or `custom`) and `pictureMissing` when Graphite stands in; `exportSize`: `[width, height]` of the export |
| `editor.view`, MCP `drive_editor` | `{action: tool, tool: style}` chooses the Style tool; style changes go through `editor.apply` / `edit_image` |

## Cost

Styling a capture adds one PNG encode in the resident (the store would do it anyway), a start of
the image-job child, one decode, the render and one encode. The resident holds the encoded
capture and the styled PNG for that moment; the raster lives only in the child. A gradient or
picture backdrop makes the PNG larger than the capture alone, and a photo several times larger.
With the option off, none of this runs.

## Verification

`editor_contract` checks that the export with no style is unchanged, that the pixels inside the
outline are copied exactly, the shadow along the edge, undo and the `style` command.
`capture_style_integration` checks byte-identical output when the option is absent or off, a styled
capture with its `-original`, and the Graphite fallback for a missing picture. `settings_contract`
checks the schema and the geometry. `sensitive_check_integration` checks that a covered capture is
styled after covering. The catalog renders `settings-capture-style-off`, `settings-capture-style`,
`settings-capture-style-custom`, `30q-editor-style` and `30r-editor-style-canvas`. Offscreen results
are not desktop acceptance; desktop evidence is linked from the pull request.
