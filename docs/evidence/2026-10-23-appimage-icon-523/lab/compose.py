"""Annotated before/after pictures for #523 (title-bar and task-manager icons)."""
import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

S = Path(sys.argv[1])
OUT = Path(sys.argv[2])
OUT.mkdir(parents=True, exist_ok=True)
FONT = '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf'
BOLD = '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'
f = lambda size, bold=False: ImageFont.truetype(BOLD if bold else FONT, size)
RED, GREEN, INK, PAPER = (220, 38, 38), (22, 140, 60), (20, 20, 24), (250, 250, 250)

B1058 = '26.1023.1058-nightly (commit 05991d9d, Qt 6.8.3), release asset SHA-256 ce707fcf…'
B1101 = '26.1023.1101-dev+gaa047473ad2e (commit aa047473, Qt 6.12.0), PR #497 CI run 40026118521, SHA-256 9918e7ea…'

DESKTOPS = {
    'k72': dict(title='k72 — Kubuntu 26.04, Plasma 6.6.6 / KWin 6.6.6 Wayland (physical, 1920×1080)',
                boxes=[('dashboard', (38, 40, 72, 70)), ('editor', (999, 330, 1033, 360)),
                       ('task manager', (366, 1026, 432, 1080))]),
    'd69': dict(title='d69 — Debian 13, Plasma 6.3.6 / KWin 6.3.6 Wayland (physical, 1920×1080)',
                boxes=[('dashboard', (38, 40, 72, 70)), ('editor', (999, 330, 1033, 360)),
                       ('task manager', (262, 1026, 318, 1078))]),
    'neon': dict(title='neon User 6.8.1 VM — Plasma 6.8.1 / KWin 6.8.1 Wayland, golden neon-user-6.8.1-20261023-9ef808278853, overlay neon-i523a on f44 (1280×800)',
                 boxes=[('dashboard', (18, 20, 52, 50)), ('editor', (446, 188, 480, 218)),
                        ('task manager', (272, 752, 330, 800))]),
}


def panel(frame, boxes, colour, width):
    im = Image.open(frame).convert('RGB')
    d = ImageDraw.Draw(im)
    for _label, (x0, y0, x1, y1) in boxes:
        for i in range(4):
            d.rectangle((x0 - i, y0 - i, x1 + i, y1 + i), outline=colour)
    scale = width / im.width
    small = im.resize((width, round(im.height * scale)), Image.LANCZOS)
    return Image.open(frame).convert('RGB'), small


def insets(original, boxes, colour, label_font):
    tiles = []
    for label, (x0, y0, x1, y1) in boxes:
        crop = original.crop((x0, y0, x1, y1))
        zoom = 4 if label != 'task manager' else 3
        crop = crop.resize((crop.width * zoom, crop.height * zoom), Image.NEAREST)
        tile = Image.new('RGB', (crop.width + 8, crop.height + 34), PAPER)
        tile.paste(crop, (4, 4))
        ImageDraw.Draw(tile).rectangle((1, 1, crop.width + 6, crop.height + 6), outline=colour, width=3)
        ImageDraw.Draw(tile).text((4, crop.height + 10), label, fill=INK, font=label_font)
        tiles.append(tile)
    return tiles


def compose(name, before, after, heading, verdicts, footers, out, boxes=None):
    spec = DESKTOPS[name]
    width = 940
    cols = []
    for side, (frame, colour, verdict, build) in enumerate(((before, RED, verdicts[0], footers[0]), (after, GREEN, verdicts[1], footers[1]))):
        chosen = boxes[side] if boxes else spec['boxes']
        original, small = panel(S / 'frames' / frame, chosen, colour, width)
        tiles = insets(original, chosen, colour, f(16))
        cols.append((small, tiles, colour, verdict, build))
    inset_h = max(t.height for c in cols for t in c[1])
    top = 96
    col_h = 44 + cols[0][0].height + 12 + inset_h + 12 + 64
    canvas = Image.new('RGB', (2 * width + 60, top + col_h + 96), PAPER)
    d = ImageDraw.Draw(canvas)
    d.text((20, 16), heading, fill=INK, font=f(26, True))
    d.text((20, 54), spec['title'], fill=INK, font=f(18))
    for i, (small, tiles, colour, verdict, build) in enumerate(cols):
        x = 20 + i * (width + 20)
        d.rectangle((x - 6, top - 6, x + width + 6, top + col_h), outline=colour, width=4)
        d.text((x, top + 4), verdict, fill=colour, font=f(22, True))
        canvas.paste(small, (x, top + 44))
        tx = x
        ty = top + 44 + small.height + 12
        for t in tiles:
            canvas.paste(t, (tx, ty))
            tx += t.width + 16
        y = ty + inset_h + 10
        for line in build:
            d.text((x, y), line, fill=INK, font=f(15))
            y += 21
    d.text((20, top + col_h + 14), 'Boxes: the windows\' title-bar icons (dashboard, editor) and the task-manager entry; the strip under each frame enlarges them. '
           'Only put.gd windows are open.', fill=INK, font=f(15))
    d.text((20, top + col_h + 38), 'Both put.gd windows (dashboard putgd-ui, editor putgd-ui --edit-image) report app_id/desktopFileName putgd-ui '
           'in every run (KWin script). Taken 2026-10-24 UTC by the #523 session.', fill=INK, font=f(15))
    d.text((20, top + col_h + 62), 'Entries AppRun wrote: putgd-ui.desktop / putgd-record.desktop (Icon = the mounted putgd.svg) and the KWin '
           'permission entries.', fill=INK, font=f(15))
    canvas.save(OUT / out, optimize=True)
    print(out, canvas.size)


LAB = 'private XDG_DATA_HOME (as lab.py guest_session): AppRun\'s putgd-ui.desktop is invisible to KWin'
for name in ('k72', 'd69', 'neon'):
    compose(name, f'{name}-1058-lab.png', f'{name}-q612-lab.png',
            '#523 before/after: Qt 6.8.3 AppImage shows KWin\'s generic "W"; the Qt 6.12 AppImage shows the put.gd icon',
            ('BEFORE — 1058 (Qt 6.8.3): generic Wayland "W" in both title bars', 'AFTER — Qt 6.12.0: put.gd icon in both title bars'),
            ([B1058, LAB, 'Task manager already shows put.gd.'],
             [B1101, LAB, 'Qt 6.12 sends the icon itself (xdg-toplevel-icon-v1, offered by this KWin).']),
            f'01-{name}-before-after.png' if name == 'k72' else f'0{2 if name == "d69" else 3}-{name}-before-after.png')
for name in ('k72', 'd69', 'neon'):
    compose(name, f'{name}-1058-lab.png', f'{name}-1058-real.png',
            '#523 control: the same 1058 AppImage with the real ~/.local/share shows the put.gd icon',
            ('1058, private XDG_DATA_HOME (the #523 lab setup): "W"', '1058, real ~/.local/share (a user\'s launch): put.gd icon'),
            ([B1058, LAB, ''],
             [B1058, 'XDG_DATA_HOME=~/.local/share: AppRun\'s NoDisplay putgd-ui.desktop (since #101) names the icon;',
              'entries removed on quit, applications/ empty before and after.']),
            f'0{4 + ["k72", "d69", "neon"].index(name)}-{name}-control-real-data-home.png')
compose('neon', 'neon-1058-lab.png', 'neon-labfix-1058.png',
        '#523 lab fix: lab.py guest_session points the private data home\'s applications/ at the session\'s',
        ('Before the fix: 1058 with lab.py\'s private data home, "W"', 'With the fix: lab.py install --appimage + start, put.gd icon'),
        ([B1058, LAB, ''],
         [B1058, 'lab.py at commit 01d2b9f9 (the lab PR): ~/putgd-lab/data/applications ->',
          '~/.local/share/applications; put.gd data stays in ~/putgd-lab/data/putgd. Capture permission intact.']),
        '07-neon-lab-fix.png',
        boxes=(DESKTOPS['neon']['boxes'], [('dashboard', (0, 0, 34, 30)), ('task manager', (272, 752, 330, 800))]))
