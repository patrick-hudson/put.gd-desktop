# Capture style

[#1893](https://github.com/patrick-hudson/put.gd-desktop/issues/1893) adds **Capture style**:
put.gd places a still capture on a styled canvas when it exports it. The canvas has padding
around the capture, a backdrop in that space, rounded corners on the capture and a soft shadow
under it. The captured pixels are never changed. Only the corners are cut, and the capture sits
on a larger image.

It is an option, and it is **off by default**. With `preferences.captureStyle` absent, or with
`enabled: false`, no styling code runs anywhere. Every capture, upload, local copy and editor
export is byte for byte what it was before this feature. `capture_style_integration` checks that
for both forms, and `editor_contract` checks it for the editor export.

## Where it applies

| Path | Rule |
| --- | --- |
| Region, window, monitor, desktop, Repeat last region, scrolling captures | Styled when the option is on, just before the capture is stored. This happens before the upload and before the local copy, so both get the styled PNG. Scrolling captures are included because they reach the same step as a PNG |
| Capture region & edit | A new capture (region, window, desktop, scrolling) opens with the style on, as does **Redact and upload** from the sensitive text prompt for a capture (not for a clipboard image or a file) in the editor's Style tool, and the editor's export draws it. The resident does not style an editor's output again |
| Files, the clipboard and Edit again in the editor | Open with the style off. Edit again of a styled capture opens the `-original` kept beside it, with the style on (see [Originals](#originals-and-edit-again)) |
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

Every preset has rounded corners and a shadow. Gradients always run at 135°, from the top-left
corner to the bottom-right one. Clear is never the default: a black shadow on transparency
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

**Choose picture…** opens the desktop's file chooser for a PNG or JPEG of at most 64 MB. `putgd-ui`
decodes it once, at most 4,096 pixels on its longer side, and saves its own PNG as
`<data>/putgd/backdrops/<sha256>.png`. The setting stores that path. Moving or deleting the
original file changes nothing, and the resident never decodes the picture. The picture covers
the canvas: it is scaled to fill it, centred and cropped, and never stretched or tiled.

If the picture can't be read when a capture is styled (the copy was deleted, or an agent named a
path that is not an image), the capture is styled with Graphite instead. Its History item says
"The backdrop picture couldn't be read, so put.gd used Graphite.", and the operation's `style`
has `pictureMissing: true`. Settings shows the same note under the preview.

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

The styled image must fit `limits.imagePixels` and `limits.imageBytes / 4`, and the editor's
24-megapixel output admission for an edit. When it doesn't fit, the padding steps down a size at a
time. When even Small doesn't fit, the capture is stored as it was captured, and its History item
says "Capture style wasn't applied: the styled image would be larger than put.gd's image limits."
A styled PNG is larger than the capture alone. When only the style would pass the Largest file limit
or the destination's maximum object size, the capture goes out as it was, with "Capture style wasn't
applied: the styled image would be larger than this destination or put.gd's file size limit
accepts." The waiting bytes are counted when the decision is made, before the store admits the file. A
cleanup that frees space while the style is drawn can leave a capture unstyled that would have fit.
An import that adds bytes meanwhile, or a disk filled to within the styled PNG's size, can make the
store refuse the styled capture where the unstyled one would have fitted. Then the capture fails as
any capture that doesn't fit does. These races are documented, not engineered around. A style problem never fails or blocks a capture in any other case. If the style can't be drawn for another reason
(the image-job child fails or times out), the capture is stored as captured, with "Capture style
wasn't applied: put.gd couldn't draw it." and a warning in the log.

## Rendering

One renderer, `src/editor/CaptureStyleRender`, draws for every path:

1. Fill the canvas with the backdrop (colour, gradient, the covering picture, or transparent).
2. Draw the shadow. A small blurred rounded square, whose size depends only on the sigma and the
   radius, is drawn as nine slices: its corners as they are, its edges stretched along the
   capture. A tall scrolling capture costs no more than a small one.
3. Fill the capture's rounded outline with the capture as a texture at an integer offset, with
   antialiasing. Every pixel inside the outline is copied unchanged.
4. With a transparent backdrop, clear the hidden colour of fully transparent pixels, as the
   editor's sanitized export does.

The data and geometry (`src/core/CaptureStyle`) are Qt Core only, and the resident links them to
validate settings and route work. The painting and the picture decoding are in `putgd_editor`,
which only `putgd-ui` links.

### Captures: the image-job `style` task

The resident never paints. On the acquisition worker it encodes the capture as the PNG the
artifact store would write, then runs `putgd-ui --image-job` with the task `style` and the style
object in the bootstrap (see [the image worker](image-worker.md)). The child decodes that PNG
(PNG only, within the image limits), works out the geometry within those limits, reads the picture
for a picture backdrop, renders, and writes the styled PNG through the usual private files. Its
checkpoint may carry `note: "picture_missing"`. The resident replaces the payload with the styled
PNG and records `style: {applied: true}` on the operation.

With **Check captures for sensitive text before uploading** also on, the check runs first, on the
capture as it was taken. Recognition and the covers never see a backdrop, so text in a backdrop
picture can never be flagged and the cover positions never shift. The covered PNG is then styled
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
uploaded. `operation.style.original` is `{status, paths}`, as `sensitive.original` is. A covered
capture keeps its original from before covering, which is also from before styling.

History's **Edit again** on a styled item opens that `-original`, with `captureStyle: true`, so the
editor starts with the style on (even if the option has been turned off since) and the result is
styled once. Without a kept original (Keep a
local copy was off), Edit again opens the styled copy, and the Style tool starts off, so frames
never stack. A capture the sensitive text check covered and then styled is the same: its kept original
is from before covering, and Edit again never opens it, so it opens the styled copy with the Style
tool off. Its frame can't be changed in the editor; a new capture can.

## Agents

| Surface | Contract |
| --- | --- |
| `settings.patch`, MCP `patch_configuration` | `preferences.captureStyle` |
| `operation.inspect`, MCP `get_operation` | `style`: `{applied: true}`, `pictureMissing` when Graphite stood in, `original` with Keep a local copy |
| `editor.open`, `action.invoke` with `action: edit`, MCP `open_editor` | `captureStyle` (MCP `capture_style`): `true` starts an edit of a file with the saved style on; only with `source: file` |
| `editor.apply`, MCP `edit_image` | `{op: style, enabled?, preset?, backdrop?, padding?, corners?, shadow?}`: a preset sets its whole look and turns the style on, the other fields change one part, `enabled: false` turns it off. One undo step. A backdrop of kind `picture` needs `picture`. Invalid values answer `edit_command_invalid` |
| `editor.inspect`, MCP `get_editor` | `style` (the resolved values, `preset` or `custom`, and `pictureMissing` when Graphite stands in) and `exportSize` |
| `editor.view`, MCP `drive_editor` | `{action: tool, tool: style}` chooses the Style tool |

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
