#!/usr/bin/env python3
"""Annotated #326 evidence pictures: full-desktop frames with the change boxed and labelled."""
import json, sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

EV = Path(sys.argv[1]); OUT = Path(sys.argv[2]); OUT.mkdir(parents=True, exist_ok=True)
FONT = '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'
RED, GREEN, WHITE, INK = (230, 40, 60), (20, 160, 80), (255, 255, 255), (20, 20, 24)


def font(size): return ImageFont.truetype(FONT, size)


def wrap(draw, text, f, width):
    lines = []
    for para in text.split('\n'):
        line = ''
        for word in para.split(' '):
            trial = (line + ' ' + word).strip()
            if draw.textlength(trial, font=f) <= width: line = trial
            else: lines.append(line); line = word
        lines.append(line)
    return lines


def callout(img, box, text, colour, at, width=520, size=None):
    """A box around the change and a caption at `at` (x, y) joined to it by a line."""
    d = ImageDraw.Draw(img); s = img.width / 1920; size = size or max(15, int(26 * s)); f = font(size)
    pad = int(10 * s); x0, y0, x1, y1 = box
    d.rounded_rectangle([x0 - pad, y0 - pad, x1 + pad, y1 + pad], radius=int(12 * s), outline=colour, width=max(4, int(7 * s)))
    lines = wrap(d, text, f, int(width * s)); lh = int(size * 1.3)
    cw = max(d.textlength(l, font=f) for l in lines) + 2 * pad; ch = lh * len(lines) + 2 * pad
    cx, cy = at
    cx = min(max(cx, 8), img.width - cw - 8); cy = min(max(cy, 8), img.height - ch - 8)
    # The joining line from the caption's nearest edge to the box's nearest point.
    bx = min(max(cx + cw / 2, x0 - pad), x1 + pad); by = min(max(cy + ch / 2, y0 - pad), y1 + pad)
    ex = min(max(bx, cx), cx + cw); ey = min(max(by, cy), cy + ch)
    d.line([ex, ey, bx, by], fill=colour, width=max(3, int(6 * s)))
    d.rounded_rectangle([cx, cy, cx + cw, cy + ch], radius=int(10 * s), fill=colour)
    for i, l in enumerate(lines): d.text((cx + pad, cy + pad + i * lh), l, font=f, fill=WHITE)


def titled(img, title):
    s = img.width / 1920; f = font(max(16, int(30 * s))); h = int(56 * s)
    out = Image.new('RGB', (img.width, img.height + h), INK); out.paste(img, (0, h))
    ImageDraw.Draw(out).text((int(16 * s), int(12 * s)), title, font=f, fill=WHITE); return out


def notified(run):
    n = json.loads((EV / run / 'notify.json').read_text())['notification']
    x0, y0, x1, y1 = n['banner']['components'][0]['box']; return [x0, y0, x1, y1]


def pair(name, left, right, lbox, rbox, ltext, rtext, title, lcolour=RED, rcolour=GREEN, lat=None, rat=None):
    a, b = Image.open(left).convert('RGB'), Image.open(right).convert('RGB')
    callout(a, lbox, ltext, lcolour, lat or (lbox[0] - 560, lbox[1] - 220))
    callout(b, rbox, rtext, rcolour, rat or (rbox[0] - 560, rbox[1] - 220))
    gap = 8; out = Image.new('RGB', (a.width, a.height + b.height + gap), INK)
    out.paste(a, (0, 0)); out.paste(b, (0, a.height + gap))
    titled(out, title).save(OUT / name, optimize=True); print(OUT / name)


def single(name, src, box, text, title, colour=GREEN, at=None):
    a = Image.open(src).convert('RGB'); callout(a, box, text, colour, at or (box[0] - 560, box[1] - 220))
    titled(a, title).save(OUT / name, optimize=True); print(OUT / name)


vm = EV / 'vm'
pair('ubuntu-before-842.png', vm / 'b842x/full/before-842-notification.png', vm / 'b842x/full/before-842-after-click.png',
     notified('vm/b842x'), notified('vm/b842x'),
     '1. Click the link-ready notification (nightly 842 AppImage)',
     '2. Nothing opens. The resident ran gio with the image\'s LD_LIBRARY_PATH and APPDIR; the host gio failed: "undefined symbol: g_unix_mount_entry_get_options", exit 127',
     'BEFORE #326 - Ubuntu 26.04 GNOME 50 VM, nightly 842 AppImage', rcolour=RED, lat=(560, 200), rat=(300, 260))
pair('ubuntu-after-click.png', vm / 'a330/full/after-330-notification.png', vm / 'a330/full/after-330-link-opened.png',
     notified('vm/a330'), [487, 284, 859, 548],
     '1. Click the link-ready notification', '2. The link opens in the default handler (XDG portal OpenURI; no image paths in its environment)',
     'AFTER #326 - Ubuntu 26.04 GNOME 50 VM, PR #330 AppImage', lat=(560, 200), rat=(80, 600))
single('ubuntu-fallback-gio.png', vm / 'a330/full/after-330-fallback-gio.png', [487, 284, 859, 548],
       'Portal stopped: put.gd falls back to the host gio, with LD_LIBRARY_PATH unset - the link still opens (exit 0)',
       'AFTER #326 - fallback launcher, Ubuntu 26.04 GNOME VM', at=(60, 600))
single('ubuntu-show-in-folder.png', vm / 'a330/full/vm-show-in-folder.png', [228, 141, 1118, 691],
       'Show in folder: Files opens with the diagnostics package selected (FileManager1.ShowItems)', 'AFTER #326 - Ubuntu 26.04 GNOME VM', at=(700, 700))
spec = {
    'k72': ('Kubuntu 26.04 Plasma 6.6 Wayland (k72)', [1552, 873, 1884, 990], [580, 224, 1340, 802]),
    'k72deb': ('DEB regression - Kubuntu 26.04 Plasma 6.6 (k72), #330 ubuntu26.04 package', [1552, 873, 1884, 990], None),
    'd69': ('Debian 13 Plasma Wayland (d69)', [774, 386, 1146, 650], [855, 429, 1615, 1007]),
    'm74': ('Linux Mint 22.3 Cinnamon X11 (m74)', [683, 455, 1237, 618], [50, 50, 850, 632]),
}
for run, (where, opened, folder) in spec.items():
    full = EV / run / 'full'; nb = notified(run)
    pair(f'{run}-click.png', full / f'{run}-click-notification.png', full / f'{run}-link-opened.png', nb, opened,
         '1. Click the link-ready notification', '2. The link opens in the default handler (portal OpenURI, clean environment)',
         f'AFTER #326 - {where}', lat=(nb[0] - 700, nb[1] - 330), rat=(opened[0] - 700, opened[1] - 330))
    if folder:
        single(f'{run}-show-in-folder.png', full / f'{run}-show-in-folder.png', folder,
               'Show in folder: the file manager opens with the diagnostics package selected (FileManager1.ShowItems)',
               f'AFTER #326 - {where}', at=(folder[2] + 40, folder[1] + 80) if folder[2] < 1300 else (folder[0] - 600, folder[1] + 80))
