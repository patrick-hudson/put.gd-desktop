#!/usr/bin/env python3
"""Annotated full-desktop pictures for the evidence README.

annotate_all.py EVROOT OUTDIR DESK [DESK ...]
For each desktop, boxes put.gd's surface on six full-desktop frames (tray menu,
Settings, editor, native Save a copy… chooser, Review window, link-ready
notification) using the rectangles the driver recorded when it cropped them,
adds a caption, scales to 1280 px wide and writes an 8-bit palette PNG.
"""
import json, sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

FONT = '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'
ACCENT = (255, 64, 129)
NAMES = {'m74': 'Linux Mint 22.3 Cinnamon 6.6.9 X11', 'k72': 'Kubuntu 26.04 Plasma 6.6.6 Wayland', 'd69': 'Debian 13 Plasma 6.3.6 Wayland',
         'f44': 'Fedora 44 GNOME 50.0 Wayland', 'vm': 'Ubuntu 26.04 GNOME 50.1 Wayland (VM)', 'aurora': 'Aurora 44 Plasma 6.7.4 Wayland (VM)'}


BASES = {'m74': 'after-quit', 'vm': 'after-quit', 'k72': 'start-desktop', 'd69': 'start-desktop', 'f44': 'start-desktop', 'aurora': 'after-quit'}

# Measured by eye where the diff merges the chooser with its editor (the editor repaints behind a modal chooser).
OVERRIDES = {('k72', 'save-copy-dialog'): [474, 225, 1440, 797], ('vm', 'save-copy-dialog'): [226, 140, 1120, 692]}

def diff_boxes(src, base, top=36, bottom=60, block=8, keep_panel=False):
    """Changed regions of src against a clean frame of the same desktop, as boxes (largest first)."""
    from PIL import ImageChops
    a = Image.open(src).convert('RGB'); b = Image.open(base).convert('RGB')
    if a.size != b.size: return []
    m = ImageChops.difference(a, b).convert('L').point(lambda v: 255 if v > 28 else 0)
    w, h = m.size; gw, gh = w // block, h // block
    small = m.resize((gw, gh), Image.BOX).load()
    cells = {(x, y) for y in range(gh) for x in range(gw) if small[x, y] > 40 and (keep_panel or top // block <= y < (h - bottom) // block)}
    seen, comps = set(), []
    for c in cells:
        if c in seen: continue
        stack = [c]; seen.add(c); pts = []
        while stack:
            x, y = stack.pop(); pts.append((x, y))
            for nx in range(x - 2, x + 3):
                for ny in range(y - 2, y + 3):
                    if (nx, ny) in cells and (nx, ny) not in seen: seen.add((nx, ny)); stack.append((nx, ny))
        xs = [q[0] for q in pts]; ys = [q[1] for q in pts]
        comps.append((len(pts), [min(xs) * block, min(ys) * block, (max(xs) + 1) * block, (max(ys) + 1) * block]))
    return [box for n, box in sorted(comps, reverse=True) if n >= 12]

def inner(rep):
    if not rep: return None
    if rep.get('inner'): return rep['inner']
    comps = rep.get('components') or []
    return comps[0]['box'] if comps else None


def shots(res, rec):
    edit = res.get('edit', {})
    notify = next((v for k, v in res.items() if k.startswith('notify:')), {}).get('notification', {})
    window = res.get('window:settings', {})
    return [
        ('tray-menu', 'Tray icon and menu on the panel', [(inner(res.get('tray:tray-menu')), 'put.gd tray menu')]),
        ('settings-window', 'Settings, opened by the resident (putgd-ui)', [(inner(window.get('window')), 'Settings'), (inner(window.get('titlebar')), 'title bar')]),
        ('editor', 'A capture in Edit & upload, with a blur and a solid redaction', [(inner(edit.get('window')), 'editor')]),
        ('save-copy-dialog', "Save a copy…: the desktop's own chooser through the XDG portal", [(inner(edit.get('dialogShot')), 'portal chooser')]),
        ('review', 'Review window after recording: trim and a timed blur', [(inner(rec.get('reviewShot')), 'Review')]),
        ('notify-notification', 'The link-ready notification', [(inner(notify.get('banner')), 'notification')]),
    ]


def draw(src, dst, caption, boxes):
    img = Image.open(src).convert('RGB'); d = ImageDraw.Draw(img)
    font = ImageFont.truetype(FONT, 26)
    for box, label in boxes:
        if not box: continue
        x0, y0, x1, y1 = box
        for i in range(5): d.rectangle((x0 - 3 - i, y0 - 3 - i, x1 + 3 + i, y1 + 3 + i), outline=ACCENT)
        tw = d.textlength(label, font=font)
        ly = y0 - 44 if y0 > 60 else y1 + 10
        lx = min(max(x0 - 8, 4), img.width - tw - 24)
        d.rectangle((lx, ly, lx + tw + 16, ly + 36), fill=ACCENT); d.text((lx + 8, ly + 3), label, font=font, fill=(255, 255, 255))
    scale = 1280 / img.width
    img = img.resize((1280, round(img.height * scale)), Image.LANCZOS)
    band = 40; out = Image.new('RGB', (img.width, img.height + band), (24, 27, 33)); out.paste(img, (0, band))
    ImageDraw.Draw(out).text((12, 10), caption, font=ImageFont.truetype(FONT, 18), fill=(231, 233, 238))
    out.quantize(colors=256, method=Image.Quantize.MEDIANCUT).save(dst, optimize=True)
    return dst


def main():
    root, outdir, desks = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3:]
    outdir.mkdir(parents=True, exist_ok=True)
    for desk in desks:
        res = json.loads((root / desk / 'results/results.json').read_text())
        recp = root / desk / 'results/out/record.json'
        rec = json.loads(recp.read_text()) if recp.exists() else res.get('record', {})
        base = root / desk / 'full' / f"{BASES.get(desk, 'start-desktop')}.png"
        for name, what, boxes in shots(res, rec):
            src = root / desk / 'full' / f'{name}.png'
            if not src.exists(): print('missing', desk, name); continue
            ref = root / desk / 'full' / 'editor.png' if name == 'save-copy-dialog' else base
            if ref.exists():
                found = diff_boxes(src, ref, keep_panel=False)
                if found:
                    label = boxes[0][1]
                    boxes = [(found[0], label)]
            if (desk, name) in OVERRIDES: boxes = [(OVERRIDES[(desk, name)], boxes[0][1])]
            dst = outdir / f'{desk}-{name}.png'
            draw(src, dst, f'{NAMES.get(desk, desk)}: {what}', boxes)
            print(dst.name, dst.stat().st_size, [b for b, _ in boxes])


if __name__ == '__main__':
    main()
