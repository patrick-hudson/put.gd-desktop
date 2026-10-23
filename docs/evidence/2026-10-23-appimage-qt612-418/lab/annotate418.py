#!/usr/bin/env python3
"""Annotated full-desktop pictures for the #418 Qt 6.12 AppImage pass.

annotate418.py EVROOT OUTDIR [DESK ...]
EVROOT/<desk>/ holds what each desktop's runs exported: runs/<desk> (A-K on the PR's CI AppImage), runs/<desk>-F
(real-browser F), runs/<desk>-labh (H's update parts on the lab nightly) and upd/results/<desk>-lab1058/frames
(1058 -> lab nightly). Boxes come from the rectangles the drivers recorded (tray menu, Settings, editor, chooser,
Review), or are found on the frame: the selected History card (the theme's `selected` colour), the update status
(the `success` colour, plus the progress bar), the browser's address bar and the uploaded image (saturated pixels).
Each picture is captioned with the desktop and the exact build, scaled to 1280 px wide and saved as a palette PNG.
"""
import json, sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

FONT = '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'
ACCENT = (255, 64, 129)
CI = 'put.gd 26.1023.1068-dev+g6a325a722afd, Qt 6.12.0 (PR #497 CI AppImage, 4c1bfa9e)'
LAB = 'put.gd 26.1023.1069-nightly, Qt 6.12.0 (lab nightly of PR #497, f253c1ec)'
LABH = 'put.gd 26.1023.1068-nightly, Qt 6.12.0 (lab nightly of PR #497, 7bbda043)'
NAMES = {'f44': 'f44 Fedora 44 GNOME 50.0 Wayland', 'd69': 'd69 Debian 13 Plasma 6.3.6 Wayland', 'k72': 'k72 Kubuntu 26.04 Plasma 6.6.6 Wayland',
         'm74': 'm74 Mint 22.3 Cinnamon 6.6.9 X11', 'ubuntu-vm': 'Ubuntu 26.04 GNOME 50.1 Wayland (VM on d69)',
         'aurora-vm': 'Aurora 45 Plasma Wayland (VM on f44)'}
# Firefox's address bar, measured on each browser frame (full-resolution pixels).
URLBAR = {'f44': [560, 120, 1320, 146], 'd69': [552, 84, 1372, 110], 'k72': [570, 78, 1330, 104], 'm74': [250, 88, 1012, 114],
          'ubuntu-vm': [272, 82, 1008, 112], 'aurora-vm': [214, 48, 998, 80]}
# Aurora's F was rerun in a fresh overlay (run aurora-vm-F2) once Patrick allowed accepting Firefox's first-run terms.
FRUN = {'aurora-vm': 'aurora-vm-F2'}
# GNOME's portal chooser: the recorded rectangle covers only its middle, so it's measured on the frame.
CHOOSER = {'f44': [516, 280, 1404, 828], 'ubuntu-vm': [228, 141, 1118, 691]}
# The B Settings frame: f44's settings-window frame was taken in GNOME's overview (the switcher check), so its
# picture is the theme check's frame of the same window.
SETTINGS = {'f44': 'settings-system.png'}
# Update frames in the dark theme (the Aurora guest follows its dark desktop): measured on the frame.
UPDATEBOX = {('aurora-vm', 'install-zsync'): [346, 470, 1000, 628]}
SELECTED = (0xdc, 0xe7, 0xfa)   # RedesignTheme light `selected`
SUCCESS = (0x14, 0x6c, 0x42)    # light `success`
ACCENTUI = (0x1f, 0x64, 0xd6)   # light `accent` (the install progress bar)


def box(rep):
    if not rep: return None
    if rep.get('inner'): return rep['inner']
    comps = rep.get('components') or []
    return comps[0]['box'] if comps else None


def colour_box(path, colour, tol=6, area=None, step=2):
    im = Image.open(path).convert('RGB'); w, h = im.size; px = im.load(); xs = []; ys = []
    x0, y0, x1, y1 = area or (0, 0, w, h)
    for y in range(max(y0, 0), min(y1, h), step):
        for x in range(max(x0, 0), min(x1, w), step):
            r, g, b = px[x, y]
            if abs(r - colour[0]) <= tol and abs(g - colour[1]) <= tol and abs(b - colour[2]) <= tol: xs.append(x); ys.append(y)
    return [min(xs), min(ys), max(xs) + 2, max(ys) + 2] if len(xs) > 20 else None


def selected_card(path):
    """The selected History card: the first block of rows, 60 to 130 tall, holding at least 100 pixels of the
    `selected` colour each (the rail's selected item lies left of the list)."""
    im = Image.open(path).convert('RGB'); w, h = im.size; px = im.load(); rows = []
    lo = 520 if w >= 1600 else 300   # the list starts right of the rail (1920 px desktops; 1280 px VMs)
    for y in range(h):
        xs = [x for x in range(lo, min(1300, w)) if abs(px[x, y][0] - SELECTED[0]) <= 3 and abs(px[x, y][1] - SELECTED[1]) <= 3
              and abs(px[x, y][2] - SELECTED[2]) <= 3]
        rows.append((min(xs), max(xs)) if len(xs) >= 100 else None)
    y = 0
    while y < h:
        if rows[y]:
            top = y
            # A line of text can thin a row out; gaps of up to 6 rows stay inside the card.
            while y < h and (rows[y] or any(rows[y + 1:y + 7])): y += 1
            if 60 <= y - top <= 130:
                return [min(r[0] for r in rows[top:y] if r), top, max(r[1] for r in rows[top:y] if r) + 1, y]
        y += 1
    return None


BG = (0xf5, 0xf6, 0xf8)   # light `bg`, the main window's background


def window_box(path):
    """The main window: the bounding box of its own background colour (the recorded rectangles can be offset),
    right of the desktop icons on the 1920 px desktops."""
    w, h = Image.open(path).size
    return colour_box(path, BG, tol=1, area=(280 if w >= 1600 else 0, 0, w, h))


def channel_box(path):
    """The Updates group's Updates, Channel and Status rows, found from its one checked checkbox (the accent
    colour) and bounded by the window's right edge."""
    win = window_box(path)
    if not win: return None
    # The tab underline is the accent colour too: take the first checkbox-sized cluster below it.
    for y0 in range(win[1] + 120, win[3] - 30, 10):
        check = colour_box(path, ACCENTUI, tol=8, area=(win[0] + 150, y0, win[2], y0 + 30), step=1)
        if check and 10 <= check[2] - check[0] <= 40 and check[3] - check[1] >= 10:
            return [check[0] - 90, check[1] - 12, win[2] - 30, check[1] + 150]
    return None


def update_box(path):
    """The Updates group's status: the success-coloured text, with the progress bar below it while installing."""
    text = colour_box(path, SUCCESS, tol=20)
    if not text: return None
    bar = colour_box(path, ACCENTUI, tol=8, area=(text[0] - 40, text[1], text[2] + 40, text[3] + 140))
    if bar and bar[2] - bar[0] > 150: text = [min(text[0], bar[0]), text[1], max(text[2], bar[2]), max(text[3], bar[3])]
    return [text[0] - 12, text[1] - 10, text[2] + 12, text[3] + 10]


def image_box(path, bar):
    im = Image.open(path).convert('RGB'); w, h = im.size; px = im.load(); xs = []; ys = []
    for y in range(bar[3] + 60, min(h - 60, bar[3] + 700), 2):
        for x in range(max(bar[0], 0), min(bar[2], w), 2):
            r, g, b = px[x, y]
            if max(r, g, b) - min(r, g, b) > 90: xs.append(x); ys.append(y)
    return [min(xs), min(ys), max(xs) + 1, max(ys) + 1] if xs else None


def url_bar(path):
    """Firefox's address bar: the row band just under the tab strip holding the URL text, measured as the widest run
    of the toolbar field colour in the top 200 rows."""
    im = Image.open(path).convert('RGB'); w, h = im.size; px = im.load(); best = None
    for y in range(30, 200):
        run = start = None; longest = (0, 0, 0)
        for x in range(w):
            c = px[x, y]
            if c == px[min(x + 1, w - 1), y] and 230 <= sum(c) / 3 <= 250:
                if start is None: start = x
            else:
                if start is not None and x - start > longest[0]: longest = (x - start, start, x)
                start = None
        if longest[0] > 500 and (best is None or longest[0] > best[0]): best = (longest[0], longest[1], y, longest[2])
    if not best: return None
    _, x0, y, x1 = best
    return [x0, y - 14, x1, y + 14]


def results(root, desk, run):
    p = root / desk / 'runs' / run / 'results.json'
    return json.loads(p.read_text()) if p.exists() else {}


def specs(root, desk):
    R = results(root, desk, desk); F = results(root, desk, desk + '-F'); H = results(root, desk, desk + '-labh')
    full = lambda run, name: root / desk / 'runs' / run / 'full' / name
    win = next((v for k, v in R.items() if k.startswith('window:')), {})
    tray = next((v for k, v in R.items() if k.startswith('tray:')), {})
    edit = R.get('edit', {}); rec = R.get('record', {})
    frames = root / desk / 'upd' / 'results' / f'{desk}-lab1058' / 'frames'
    clip = sorted((root / desk / 'runs' / desk / 'full').glob(f'{desk}-clipfile*-*.png'))
    # The frame of the CVE-2025-10729 SVG: the last clipfile frame (the samples ran with it last), or its own rerun.
    clip10729 = full(desk, f'{desk}-clipfile-10729-0.png')
    if not clip10729.exists():
        order = (R.get('clipfile') or {}).get('entries') or []
        idx = next((i for i, e in enumerate(order) if e['sample'].startswith('cve-2025-10729')), None)
        clip10729 = full(desk, f'{desk}-clipfile-{idx}.png') if idx is not None else None
    frun = FRUN.get(desk, desk + '-F')
    browser = full(frun, f'{frun}-browser.png') if desk in FRUN else full(frun, f'{desk}-F-browser2.png')
    if not browser.exists(): browser = full(frun, f'{desk}-F-browser.png')
    out = [
        ('tray-menu', CI, full(desk, f'{desk}-tray-menu.png'), "B: put.gd's tray menu, opened with a real click", [(box(tray), 'put.gd tray menu')]),
        ('settings-window', CI, full(desk, SETTINGS.get(desk, f'{desk}-settings-window.png')), 'B: Settings from the resident (putgd-ui on the native Qt 6.12 platform plugin)',
         [(window_box(full(desk, SETTINGS.get(desk, f'{desk}-settings-window.png'))), 'Settings')]),
        ('editor', CI, full(desk, 'editor.png'), 'D: a capture in Edit & upload, with a blur and a solid redaction', [(box(edit.get('window')), 'editor')]),
        ('save-copy-dialog', CI, full(desk, 'save-copy-dialog.png'), "D: Save a copy… uses the desktop's own chooser through the XDG portal", [(CHOOSER.get(desk) or box(edit.get('dialogShot')), 'portal chooser')]),
        ('review', CI, full(desk, 'review.png'), 'E: the Review window after recording (trim and a timed blur)', [(box(rec.get('reviewShot')), 'Review')]),
    ]
    if clip10729 and clip10729.exists():
        out.append(('cve-svg', CI, clip10729, 'Safety: CVE-2025-10729\'s SVG (crashes Qt 6.8.3) uploaded through Upload clipboard; History opens it, no crash',
                    [(selected_card(clip10729), 'the CVE SVG, selected')]))
    if browser.exists():
        bar = URLBAR.get(desk) or url_bar(browser)
        link = (F.get('notify:' + desk + '-F-notify') or {}).get('operation', {}).get('items', [{}])[0].get('link') if F else None
        out.append(('F-browser', CI, browser, 'F: the link-ready notification, clicked, opened the default browser (temporary profile) on the live upload',
                    [(bar, 'address'), (image_box(browser, bar) if bar else None, 'uploaded image')]))
    for name, what in (('install-zsync', 'J: 26.1023.1058-nightly (Qt 6.8.3) finds the lab nightly and installs it by zsync'),
                       ('restarted', 'J: after Restart put.gd the lab nightly runs; Settings says it was updated from 1058 by zsync')):
        if (frames / f'{name}.png').exists():
            found = UPDATEBOX.get((desk, name)) or update_box(frames / f'{name}.png')
            out.append(('update-' + name, LAB, frames / f'{name}.png', what, [(found, 'updated 1058 → 1069' if name == 'restarted' else 'installing 1069')]))
    stable = full(desk + '-labh', f'{desk}-labh-344-stable.png')
    if stable.exists():
        out.append(('344-stable', LABH, stable, 'H (#344): Channel set to Stable answers "no stable release yet" against the live feed', [(channel_box(stable), 'Channel: Stable')]))
    return out


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
    band = 56; W = max(img.width, 1280)
    out = Image.new('RGB', (W, img.height + band), (24, 27, 33)); out.paste(img, ((W - img.width) // 2, band))
    f = ImageFont.truetype(FONT, 15); dd = ImageDraw.Draw(out)
    dd.text((12, 8), caption[0], font=f, fill=(231, 233, 238)); dd.text((12, 31), caption[1], font=f, fill=(154, 161, 173))
    out.quantize(colors=256, method=Image.Quantize.MEDIANCUT).save(dst, optimize=True)


def main():
    root, outdir = Path(sys.argv[1]), Path(sys.argv[2]); desks = sys.argv[3:] or list(NAMES)
    outdir.mkdir(parents=True, exist_ok=True)
    for desk in desks:
        for name, build, src, what, boxes in specs(root, desk):
            if not Path(src).exists(): print('missing', desk, name, src); continue
            dst = outdir / f'{desk}-{name}.png'
            draw(src, dst, (f'{NAMES[desk]} · {build}', what), boxes)
            print(dst.name, dst.stat().st_size, [b for b, _ in boxes])


if __name__ == '__main__':
    main()
