#!/usr/bin/env python3
"""Annotated pictures for the KDE neon goldens (docs/neon-vm.md).

annotate.py EVIDENCE_DIR
Reads raw/*.png (1280x800 QMP screendumps of the guests, no host desktop),
boxes the callouts measured on those frames, adds a two-line footer naming the
golden, the Plasma/KWin and Qt versions and the put.gd build, and writes the
results to pictures/.
"""
import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

BOLD = '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'
REGULAR = '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf'
ACCENT = (46, 204, 113)
FOOTER = (24, 26, 32)
UNSTABLE = 'neon-unstable-noble-20261023-d613510c07bd'
USER = 'neon-user-6.8.1-20261023-9ef808278853'
BUILD = 'put.gd 26.1023.1058-nightly AppImage'
CAPTURE = 'region, window, desktop: available via KWin ScreenShot2 v5'

PICTURES = [
    ('01-unstable-autologin.png', '01-unstable-plasma-master-autologin.png',
     [((1090, 686, 1278, 732), 'autologin reached Plasma master, no greeter', 'above')],
     f'{UNSTABLE} (KDE neon Unstable, git master)',
     'Plasma master / KWin 6.8.80 · Qt 6.11.1 · no put.gd installed · plasmalogin autologin into the Wayland session'),
    ('02-unstable-appimage.png', '02-unstable-appimage-capture-available.png',
     [((10, 131, 200, 277), CAPTURE, 'right-top'),
      ((12, 694, 134, 716), 'build 26.1023.1058-nightly', 'right'),
      ((998, 756, 1026, 786), 'put.gd tray icon', 'above')],
     f'{BUILD} on {UNSTABLE}',
     'KDE neon Unstable · Plasma master / KWin 6.8.80 · Qt 6.11.1 · smoke run neon-a955b23: installs, starts, captures available'),
    ('03-user-6.8.1-appimage.png', '03-user-6.8.1-appimage-capture-available.png',
     [((10, 131, 200, 277), CAPTURE, 'right-top'),
      ((350, 684, 852, 708), 'build 26.1023.1058-nightly', 'above'),
      ((998, 756, 1026, 786), 'put.gd tray icon', 'above')],
     f'{BUILD} on {USER}',
     'KDE neon User · Plasma / KWin 6.8.1 · Qt 6.11.1 · smoke run neon-abd768f: installs, starts, captures available'),
]


def tag(draw, font, box, label, where, width):
    x0, y0, x1, y1 = box
    tw = draw.textlength(label, font=font)
    if where == 'right':
        lx, ly = x1 + 10, (y0 + y1) // 2 - 14
    elif where == 'right-top':
        lx, ly = x1 + 10, y0 - 43
    else:
        lx, ly = x0, y0 - 34
    lx = max(4, min(lx, width - tw - 20))
    draw.rectangle((lx, ly, lx + tw + 16, ly + 28), fill=ACCENT)
    draw.text((lx + 8, ly + 4), label, font=font, fill=(255, 255, 255))


def main():
    root = Path(sys.argv[1])
    (root / 'pictures').mkdir(exist_ok=True)
    label_font, line1, line2 = ImageFont.truetype(BOLD, 17), ImageFont.truetype(BOLD, 17), ImageFont.truetype(REGULAR, 15)
    for src, dst, boxes, first, second in PICTURES:
        frame = Image.open(root / 'raw' / src).convert('RGB')
        out = Image.new('RGB', (frame.width, frame.height + 56), FOOTER)
        out.paste(frame, (0, 0))
        draw = ImageDraw.Draw(out)
        for box, _, _ in boxes:
            draw.rectangle(box, outline=ACCENT, width=3)
        for box, label, where in boxes:
            tag(draw, label_font, box, label, where, out.width)
        draw.text((14, frame.height + 8), first, font=line1, fill=(255, 255, 255))
        draw.text((14, frame.height + 31), second, font=line2, fill=(200, 204, 212))
        out.save(root / 'pictures' / dst, optimize=True)
        print(dst)


if __name__ == '__main__':
    main()
