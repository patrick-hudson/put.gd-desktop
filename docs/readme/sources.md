# README assets

Everything under `docs/readme/` exists only for the repository README. Nothing
here is packaged, exported in the curated source archive or used by a test.

## Generated artwork

| File | What it is |
| --- | --- |
| `logo.svg` | The application icon from `packaging/linux/putgd.svg` (the IconArt arrow-and-tray glyph) with a soft glow, on a padded viewBox |
| `banner-dark.svg`, `banner-light.svg` | Hero banner: the icon tile, wordmark, tagline and capability pills. The light file is the dark file with its palette swapped; GitHub picks one through `<picture>` and `prefers-color-scheme` |
| `flow.svg` | The four-step workflow animation. Pure SMIL on a shared nine-second timeline, no scripts, no external fonts; the tray colours are the real ones from `src/ui/TrayController.cpp` (`#9bbcff` preparing, `#62d69b` uploading) |

## Screenshots

All screenshots are crops of retained acceptance evidence. The evidence files
are the originals; do not edit them. Re-crop from the source when a window
changes.

| File | Source evidence | Crop box (left, top, right, bottom) |
| --- | --- | --- |
| `settings-cinnamon.png` | `docs/evidence/issue40-redesign/cinnamon-general.png` | 0, 0, 1280, 760 |
| `destinations-cinnamon.png` | `docs/evidence/issue40-redesign/cinnamon-destination.png` | 0, 0, 1280, 760 |
| `history-cinnamon.png` | `docs/evidence/issue40-redesign/cinnamon-history.png` | 0, 0, 1280, 760 |
| `settings-gnome.png` | `docs/evidence/issue40-redesign/gnome-settings.png` | 67, 32, 1280, 800 |
| `settings-kde.png` | `docs/evidence/issue40-redesign/kde-settings.png` | 0, 0, 1280, 745 |
| `editor-gnome.png` | `docs/evidence/issue51-edit-and-upload/gnome-vm-agent-edits.png` | 220, 88, 1124, 742 |
| `editor-kde.png` | `docs/evidence/issue51-edit-and-upload/kde-vm-editor-open.png` | 190, 48, 1090, 697 |
| `tray-menu-cinnamon.png` | `docs/evidence/issue36-tray-label/cinnamon-menu.png` | 960, 500, 1280, 800 |
| `desktops-strip.png` | the three `settings-*.png` crops above | each scaled to 416 px wide, 16 px gaps, 14 px rounded corners, top-aligned to the shortest panel |

The crops were produced with Pillow:

```python
from PIL import Image, ImageDraw
Image.open(SOURCE).convert("RGB").crop(BOX).save(TARGET, optimize=True)
# desktops-strip.png
W, gap = 1280, 16; pw = (W - 2 * gap) // 3
panels = [Image.open(f).convert("RGB") for f in ("settings-cinnamon.png", "settings-gnome.png", "settings-kde.png")]
panels = [p.resize((pw, round(p.height * pw / p.width)), Image.LANCZOS) for p in panels]
ph = min(p.height for p in panels)
strip = Image.new("RGBA", (W, ph), (0, 0, 0, 0))
for i, p in enumerate(panels):
    mask = Image.new("L", (pw, ph), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, pw - 1, ph - 1), radius=14, fill=255)
    strip.paste(p.crop((0, 0, pw, ph)), (i * (pw + gap), 0), mask)
strip.save("desktops-strip.png", optimize=True)
```

The lab sessions behind these images are identified in their evidence
directories; the README does not claim any acceptance result from them.
