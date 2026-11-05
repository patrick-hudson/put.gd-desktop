#!/usr/bin/env python3
"""Annotated full-desktop pictures for the rerun README (nightly 0.1.1-nightly.942).

annotate_rerun.py EVROOT OUTDIR [DESK ...]
EVROOT holds what the runs exported: <desk>/ (physical run directories), <desk>-F/ (real-browser F runs),
<desk>-upd/results/<run>/frames/ (live in-app updates) and the VM trees. Boxes come from the rectangles the
drivers recorded (tray menu, Settings, editor, chooser, Review), from the #341 menu box, or are measured on the
frame (the Updates group, the open Channel list, the browser's address bar). The uploaded image in a browser frame
is found by its saturated colours. Each picture is captioned with host, desktop and build, scaled to 1280 px wide
and written as an 8-bit palette PNG.
"""
import json, sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

FONT = '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'
ACCENT = (255, 64, 129)
BUILD = 'put.gd nightly 0.1.1-nightly.942 (3594d1bd)'
NAMES = {'f44': 'f44 Fedora 44 GNOME 50.0 Wayland', 'd69': 'd69 Debian 13 Plasma 6.3.6 Wayland', 'k72': 'k72 Kubuntu 26.04 Plasma 6.6.6 Wayland',
         'm74': 'm74 Mint 22.3 Cinnamon 6.6.9 X11', 'ubuntu-vm': 'Ubuntu 26.04 GNOME 50.1 Wayland (VM on d69)',
         'aurora-vm': 'Aurora 44 Plasma 6.7.4 Wayland (VM on f44)'}
RUN = {'f44': 'f44', 'd69': 'd69', 'k72': 'k72', 'm74': 'm74', 'ubuntu-vm': 'ubuntu-vm/runs/ubuntu-vm', 'aurora-vm': 'aurora-vm/runs/aurora-vm'}
TRAYRUN = {'aurora-vm': 'aurora-vm/runs/aurora-vm2', 'd69': 'd69-F'}  # retaken: Aurora after the Klipper mix-up, d69 without the emoji popup
# Measured on the frames (full-resolution pixels).
UPDATES = {'m74': [538, 562, 1571, 771], 'k72': [539, 571, 1573, 782], 'd69': [539, 576, 1573, 785], 'f44': [573, 583, 1524, 778],
           'ubuntu-vm': [306, 467, 1233, 684], 'aurora-vm': [238, 423, 1233, 640]}
DROPDOWN = {'m74': [655, 677, 1555, 757], 'k72': [656, 690, 1557, 770], 'd69': [656, 694, 1557, 773], 'f44': [656, 738, 1557, 817],
            'ubuntu-vm': [423, 588, 1217, 670], 'aurora-vm': [356, 544, 1217, 624]}
DROPSRC = {'aurora-vm': 'aurora-vm3/runs/aurora-vm3/full/aurora-vm-channel-dropdown-open.png'}
URLBAR = {'d69': [548, 83, 1380, 110], 'k72': [565, 78, 1323, 105], 'm74': [247, 85, 1008, 116], 'f44': [565, 118, 1323, 149], 'ubuntu-vm': [270, 80, 1010, 112], 'aurora-vm': [243, 48, 1005, 80]}
BROWSER = {'d69': 'd69-F/full/d69-F-F-browser.png', 'k72': 'k72-F/full/k72-F-F-browser.png', 'm74': 'm74-F/full/m74-F-F-browser.png', 'f44': 'f44-F/full/f44-F-F-browser.png',
           'ubuntu-vm': 'ubuntu-vm/runs/ubuntu-vm-f2/full/ubuntu-vm-F-browser.png', 'aurora-vm': 'aurora-vm/runs/aurora-vm-f/full/aurora-vm-F-browser.png'}
LINK = {'d69': 'https://put.gd/acceptance/afa2/d69/Screen-Capture-2026-10-22-02-08-16.png',
        'k72': 'https://put.gd/acceptance/afa2/k72/Screen-Capture-2026-10-22-02-04-39.png',
        'm74': 'https://put.gd/acceptance/afa2/m74/Screen-Capture-2026-10-22-01-44-36.png',
        'f44': 'https://put.gd/acceptance/afa2/f44/Screen-Capture-2026-10-22-01-48-59.png',
        'ubuntu-vm': 'https://put.gd/acceptance/afa2/ubuntu-vm/Screen-Capture-2026-10-22-01-32-51.png',
        'aurora-vm': 'https://put.gd/acceptance/afa2/aurora-vm/Screen-Capture-2026-10-22-01-36-18.png'}


def box(rep):
    if not rep: return None
    if rep.get('inner'): return rep['inner']
    comps = rep.get('components') or []
    return comps[0]['box'] if comps else None


def image_box(path, bar):
    """The uploaded test image in a browser frame: the bounding box of strongly saturated pixels in the page area
    under the address bar (as wide as the bar plus a margin), so the desktop around the window doesn't count."""
    im = Image.open(path).convert('RGB'); w, h = im.size; px = im.load(); xs = []; ys = []
    for y in range(bar[3] + 20, min(h - 60, bar[3] + 640), 2):
        for x in range(max(bar[0] - 120, 0), min(bar[2] + 120, w), 2):
            r, g, b = px[x, y]
            if max(r, g, b) - min(r, g, b) > 90: xs.append(x); ys.append(y)
    return [min(xs), min(ys), max(xs) + 1, max(ys) + 1] if xs else None


def specs(root, desk):
    R = json.loads((root / RUN[desk] / 'results.json').read_text())
    T = json.loads((root / TRAYRUN.get(desk, RUN[desk]) / 'results.json').read_text())
    full = lambda run, name: root / run / 'full' / name
    win = next((v for k, v in R.items() if k.startswith('window:')), {})
    tray = next((v for k, v in T.items() if k.startswith('tray:')), {})
    edit = R.get('edit', {}); rec = R.get('record', {})
    out = [
        ('tray-menu', full(TRAYRUN.get(desk, RUN[desk]), f'{desk}-tray-menu.png'), "B: put.gd's tray menu, opened with a real click", [(box(tray), 'put.gd tray menu')]),
        ('settings-window', full(RUN[desk], f'{desk}-settings-window.png'), 'B: Settings from the resident (putgd-ui), with the put.gd window icon', [(box(win.get('window')), 'Settings')]),
        ('348-first-open', full(RUN[desk], f'{desk}-348-first-open.png'), "#348: one ui.show updates on a never-opened Settings lands on Updates", [(UPDATES.get(desk), 'Updates, in view')]),
        ('344-channel', root / DROPSRC[desk] if desk in DROPSRC else full(RUN[desk], f'{desk}-channel-dropdown-open.png'),
         '#344: Channel offers exactly Stable and Nightly; Nightly is preselected on a fresh profile', [(DROPDOWN.get(desk), 'Stable / Nightly')]),
        ('editor', full(RUN[desk], 'editor.png'), 'D: a capture in Edit & upload, with a blur and a solid redaction', [(box(edit.get('window')), 'editor')]),
        ('save-copy-dialog', full(RUN[desk], 'save-copy-dialog.png'), "D: Save a copy… uses the desktop's own chooser through the XDG portal", [(box(edit.get('dialogShot')), 'portal chooser')]),
        ('review', full(RUN[desk], 'review.png'), 'E: the Review window after recording (trim and a timed blur)', [(box(rec.get('reviewShot')), 'Review')]),
    ]
    if desk in BROWSER:
        src = root / BROWSER[desk]
        out.append(('F-browser', src, f'F: the link-ready notification, clicked, opened the default browser on {LINK[desk]}',
                    [(URLBAR[desk], 'address'), (image_box(src, URLBAR[desk]), 'uploaded image')]))
    return out


def extras(root):
    m = json.loads((root / 'm74/341-desktop-tray.json').read_text())
    w = json.loads((root / 'm74/341-window-tray.json').read_text())
    return [
        ('m74', '341-desktop-capture', root / 'm74/out/m74-341-desktop-traymenu.png',
         "#341: Ctrl+Shift+3 with put.gd's tray menu open; the desktop capture itself holds the menu", [(m['menuBox'], 'tray menu, in the capture')]),
        ('m74', '341-region-notice', root / 'm74/full/m74-341-region-notice-notice.png',
         '#341: Ctrl+Shift+1 while the menu stays open: selection gives up after 3 s with a notice', []),
        ('m74', '341-window-capture', root / 'm74/out/m74-341-window-traymenu.png',
         '#341: Ctrl+Shift+2 with the tray menu open captures the focused window (584×232); the panel menu is outside it', []),
        ('f44', 'update-zsync', root / 'f44-upd/results/f44-live942b/frames/install-zsync.png',
         'J (#343, live): 942 finds 947 on app.put.gd and installs it through appimageupdatetool (zsync)', [([541, 785, 1571, 934], 'installing 947')]),
        ('f44', 'update-restarted', root / 'f44-upd/results/f44-live942b/frames/restarted.png',
         'J (#343, live): after Restart put.gd, 947 runs and Settings says "Updated from 0.1.1-nightly.942 to 0.1.1-nightly.947"', [([541, 785, 1571, 864], 'updated 942 → 947')]),
        ('aurora-vm', '391-rail-overlap', root / 'aurora-391-rail-overlap-942.png',
         'Finding #391: at 1280×800 the rail\'s "Settings" item is drawn over the "Capture output" footer', [([0, 540, 210, 720], '#391 overlap')]),
    ]


def draw(src, dst, caption, boxes):
    img = Image.open(src).convert('RGB'); d = ImageDraw.Draw(img)
    big = img.width >= 1600
    font = ImageFont.truetype(FONT, 26 if big else 18)
    for b, label in boxes:
        if not b: continue
        x0, y0, x1, y1 = b
        for i in range(5 if big else 3): d.rectangle((x0 - 3 - i, y0 - 3 - i, x1 + 3 + i, y1 + 3 + i), outline=ACCENT)
        tw = d.textlength(label, font=font); th = 36 if big else 26
        ly = y0 - th - 8 if y0 > th + 12 else y1 + 10
        lx = min(max(x0 - 8, 4), img.width - tw - 24)
        d.rectangle((lx, ly, lx + tw + 16, ly + th), fill=ACCENT); d.text((lx + 8, ly + 3), label, font=font, fill=(255, 255, 255))
    if img.width > 1280:
        img = img.resize((1280, round(img.height * 1280 / img.width)), Image.LANCZOS)
    band = 56; W = max(img.width, 1280)  # a small capture (a window) sits on a full-width canvas so the caption fits
    out = Image.new('RGB', (W, img.height + band), (24, 27, 33)); out.paste(img, ((W - img.width) // 2, band))
    f = ImageFont.truetype(FONT, 15); dd = ImageDraw.Draw(out)
    dd.text((12, 8), caption[0], font=f, fill=(231, 233, 238)); dd.text((12, 31), caption[1], font=f, fill=(154, 161, 173))
    out.quantize(colors=256, method=Image.Quantize.MEDIANCUT).save(dst, optimize=True)


def main():
    root, outdir = Path(sys.argv[1]), Path(sys.argv[2]); desks = sys.argv[3:] or list(NAMES)
    outdir.mkdir(parents=True, exist_ok=True)
    jobs = [(d, *s) for d in desks for s in specs(root, d)] + [e for e in extras(root) if e[0] in desks]
    for desk, name, src, what, boxes in jobs:
        if not Path(src).exists(): print('missing', desk, name, src); continue
        dst = outdir / f'{desk}-{name}.png'
        draw(src, dst, (f'{NAMES[desk]} · {BUILD}', what), boxes)
        print(dst.name, dst.stat().st_size, [b for b, _ in boxes])


if __name__ == '__main__':
    main()
