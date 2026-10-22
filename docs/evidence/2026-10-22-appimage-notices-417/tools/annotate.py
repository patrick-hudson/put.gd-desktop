#!/usr/bin/env python3
"""Annotated pictures for the #417/#421 evidence. argv: SCRATCH OUTDIR"""
import json
import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

scratch, out = Path(sys.argv[1]), Path(sys.argv[2])
out.mkdir(parents=True, exist_ok=True)
MONO = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", 15)
SANS = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 16)
BOLD = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 17)
RED, GREEN, INK, PAPER, BAR = (214, 40, 40), (22, 140, 70), (30, 30, 30), (250, 250, 248), (36, 41, 51)
BEFORE = "968: putgd-26.1022.968-nightly-x86_64.AppImage (dc17d5c6…, nightly ddc7981e)"
AFTER = "987: putgd-26.1022.987-dev+gd471cb1a1792-x86_64.AppImage (a7b8c92b…, CI run 39840154884, PR #424 d471cb1a)"


def banner(image, lines, colour=BAR):
    height = 12 + 24 * len(lines)
    canvas = Image.new("RGB", (max(image.width, 1180), image.height + height), PAPER)
    draw = ImageDraw.Draw(canvas)
    draw.rectangle([0, 0, canvas.width, height], fill=colour)
    for i, line in enumerate(lines):
        draw.text((12, 8 + 24 * i), line, font=BOLD if i == 0 else SANS, fill=(255, 255, 255))
    canvas.paste(image, (0, height))
    return canvas, height


def text_panel(title, lines, width, marks):
    """Render text lines; marks = {line index: (colour, note)} draws a box and a note."""
    line_height = 21
    image = Image.new("RGB", (width, 50 + line_height * len(lines) + 10), PAPER)
    draw = ImageDraw.Draw(image)
    draw.text((12, 12), title, font=BOLD, fill=INK)
    for i, line in enumerate(lines):
        y = 46 + line_height * i
        draw.text((14, y), line, font=MONO, fill=INK)
        if i in marks:
            colour, note = marks[i]
            length = draw.textlength(line, font=MONO)
            draw.rectangle([8, y - 3, 20 + length, y + line_height - 2], outline=colour, width=3)
            if note:
                draw.text((28 + length, y), "◀ " + note, font=SANS, fill=colour)
    return image


def side_by_side(left, right, header):
    gap = 16
    canvas = Image.new("RGB", (left.width + right.width + gap, max(left.height, right.height)), (210, 210, 210))
    canvas.paste(left, (0, 0))
    canvas.paste(right, (left.width + gap, 0))
    return banner(canvas, header)[0]


# 1. runtime-notices.txt, before and after.
old = (scratch / "after-doc/runtime-notices-968.txt").read_text().splitlines()[:12]
new = (scratch / "after-doc/runtime-notices.txt").read_text().splitlines()[:24]
old_marks = {i: (RED, "native text, false for the AppImage") for i, line in enumerate(old) if "No compiler or Qt runtime" in line}
old_marks.update({i: (RED, "") for i, line in enumerate(old) if "dynamically linked distribution packages" in line})
new_marks = {0: (GREEN, "the AppImage's own notice"),
             next(i for i, l in enumerate(new) if "Qt 6.8.3" in l): (GREEN, "what it bundles"),
             next(i for i, l in enumerate(new) if l.startswith("This file and bundled-components.json")): (GREEN, "inventory")}
left = text_panel("BEFORE  968  usr/share/doc/putgd/runtime-notices.txt (first 12 lines)", old, 900, old_marks)
right = text_panel("AFTER  987  usr/share/doc/putgd/runtime-notices.txt (first 24 lines of 1,019)", new, 1120, new_marks)
side_by_side(left, right, ["#417 item 1: the AppImage's runtime-notices.txt", BEFORE, AFTER]).save(out / "notices-before-after.png")

# 2. Qt's licence texts, before and after.
before = json.loads((scratch / "before-968.json").read_text())["licenceTexts"]
after = json.loads((scratch / "after-987.json").read_text())["licenceTexts"]
rows_old, rows_new, mo, mn = [], [], {}, {}
for name in ("qtbase", "qtwayland", "qtsvg", "qtimageformats", "qttranslations", "qtdeclarative", "layershell"):
    for rows, data, marks, side in ((rows_old, before, mo, "old"), (rows_new, after, mn, "new")):
        entry = data.get(name)
        if entry is None:
            rows.append(f"{name}/  (not shipped)")
            if name == "qtdeclarative" and side == "new":
                marks[len(rows) - 1] = (GREEN, "not bundled: dropped")
            if name == "qttranslations" and side == "old":
                marks[len(rows) - 1] = (RED, ".qm files bundled, no licence")
            continue
        texts = entry["LICENSES"]
        rows.append(f"{name}/LICENSES/  {len(texts)} text{'s' if len(texts) != 1 else ''}" + (f", {entry['testExampleFiles']} test/example files" if entry["testExampleFiles"] else ""))
        if side == "old" and not entry["hasLGPL3"] and name != "qttranslations":
            marks[len(rows) - 1] = (RED, "no LGPL-3.0-only.txt" + (", Commercial only" if entry["hasCommercial"] else ""))
        if side == "new" and entry["hasLGPL3"]:
            marks[len(rows) - 1] = (GREEN, "LGPL-3.0-only.txt present")
        key = ["LGPL-3.0-only.txt", "GPL-2.0-only.txt", "GPL-3.0-only.txt", "Libpng.txt", "IJG.txt", "blessing.txt"]
        shown = (texts[:6] if side == "old" else [k for k in key if k in texts]) if name == "qtbase" else []
        if name == "qttranslations" and side == "new":
            shown = texts
        for text in shown:
            rows.append("    " + text)
            if "Qt-Commercial" in text:
                marks[len(rows) - 1] = (RED, "doesn't apply")
            if text == "LGPL-3.0-only.txt":
                marks[len(rows) - 1] = (GREEN, "")
        if name == "qtbase" and len(texts) > len(shown):
            rows.append(f"    … {len(texts) - len(shown)} more (Apache-2.0, MIT, MPL-2.0, Unicode-3.0, …); Libpng, IJG, blessing cover libpng, libjpeg, SQLite" if side == "new" else f"    … {len(texts) - len(shown)} more")
left = text_panel("BEFORE  968  usr/share/doc/putgd/bundled-licenses/", rows_old, 900, mo)
right = text_panel("AFTER  987  usr/share/doc/putgd/bundled-licenses/", rows_new, 900, mn)
side_by_side(left, right, ["#417 item 2: Qt's and LayerShellQt's licence texts", BEFORE, AFTER]).save(out / "licence-texts-before-after.png")

# 3. Smoke pictures with the build named.
tray = Image.open(scratch / "smoke/crops/f44-987-tray-menu.png").convert("RGB")
canvas, top = banner(tray, ["f44 smoke: tray menu after a real click",
                            "f44 Fedora 44 GNOME 50.0 Wayland (physical), 2026-10-22T21:44Z",
                            AFTER])
draw = ImageDraw.Draw(canvas)
draw.rectangle([2, top + 2, tray.width - 3, canvas.height - 3], outline=GREEN, width=3)
draw.text((tray.width + 16, top + 20), "◀ put.gd's own StatusNotifierItem menu,\n   opened with uinput on the top-bar icon\n   (1708,19); idle RSS 29.24 MiB after 180 s", font=SANS, fill=GREEN)
canvas.save(out / "f44-987-tray-menu.png")
shot = Image.open(scratch / "smoke/out/capture-region-kept.png").convert("RGB")
canvas, top = banner(shot, ["f44 smoke: region capture (1120×680) of the magenta lab cover",
                            "f44 Fedora 44 GNOME 50.0 Wayland, 2026-10-22T21:46Z; uploaded to the loopback S3 fixture; cover check ok (0.81)",
                            AFTER])
canvas.save(out / "f44-987-region-capture.png")
print(sorted(p.name for p in out.iterdir()))
