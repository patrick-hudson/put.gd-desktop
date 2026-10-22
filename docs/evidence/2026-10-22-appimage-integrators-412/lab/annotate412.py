"""Annotate the #412 frames from the Mint 22.3 guest's VNC: a red box around what shows the result, a label
naming it, and a footer naming the guest, the exact builds (published v26.1022.968-nightly updated in-app to the
published v26.1022.983-nightly) and the integrator.
  annotate412.py FRAMES OUT"""
import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

FRAMES, OUT = Path(sys.argv[1]), Path(sys.argv[2])
FONT = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf', 20)
SMALL = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf', 15)
RED = (220, 30, 40)
GUEST = 'Mint 22.3 Cinnamon 6.6.4 X11 VM on d69 (cinnamon-x11 4a21566d)'
BUILDS = 'put.gd v26.1022.968-nightly (ddc7981e, image dc17d5c6) -> v26.1022.983-nightly (5146b374, image c348ee5e), both as published'
GL, AIL = 'Gear Lever 4.6.2 (Flatpak)', 'AppImageLauncher 3.0.0-beta-3 (96cb937)'
INSTALLED = ((350, 250, 1130, 292), 'Same path, new version: installed 983, 968 kept as the hidden backup', 'below')
UPDATED = [((350, 160, 1110, 200), 'Up to date: 983 is the newest nightly', 'above'),
           ((350, 212, 1005, 238), 'Updated from 968 to 983, in place', 'below'),
           ((14, 703, 140, 724), 'Running 983', 'right')]
MENU = ((337, 352, 628, 394), 'The only put.gd entry, unchanged; clicking it starts 983', 'below')

PICTURES = [
    ('gl-1-installed.png', '06-gl-progress.png', [INSTALLED], GL),
    ('gl-2-updated.png', '08-gl-updated-row.png', UPDATED, GL),
    ('gl-3-menu-entry.png', '09-gl-menu.png', [(MENU[0], "One entry, Gear Lever's put.gd (5643e5), unchanged; it starts 983", 'below')], GL),
    ('gl-4-record.png', '12-gl-details.png',
     [((195, 432, 605, 476), "Gear Lever's own check: no update (it reads 983)", 'right'),
      ((127, 597, 673, 647), 'Its record: the same path', 'right')], GL),
    ('ail-1-installed.png', '16-ail-installed.png', [INSTALLED], AIL),
    ('ail-2-updated.png', '17-ail-updated-row.png', UPDATED, AIL),
    ('ail-3-menu-entry.png', '18-ail-menu-after.png', [(MENU[0], "One entry, AppImageLauncher's put.gd, unchanged; it starts 983", 'below')], AIL),
    ('ail-4-updater.png', '19-ail-updater.png',
     [((420, 272, 1246, 322), "AppImageLauncher's updater: no update for the same file (it holds 983)", 'below')], AIL),
]


def label(draw, box, text, side, size):
    w = draw.textlength(text, font=FONT); h = FONT.size + 10
    x0, y0, x1, y1 = box
    if side == 'above': x, y = x0, y0 - h - 6
    elif side == 'right': x, y = x1 + 10, y0
    else: x, y = x0, y1 + 6
    if x + w + 14 > size[0]: x, y = x0, y1 + 6
    x = max(4, min(x, size[0] - w - 14)); y = max(4, y)
    draw.rectangle((x, y, x + w + 12, y + h), fill=RED)
    draw.text((x + 6, y + 4), text, fill='white', font=FONT)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    for name, source, marks, tool in PICTURES:
        image = Image.open(FRAMES / source).convert('RGB')
        draw = ImageDraw.Draw(image)
        for box, text, side in marks:
            draw.rectangle(box, outline=RED, width=4)
            label(draw, box, text, side, image.size)
        lines = [f'{GUEST} - {tool}', BUILDS]
        top = image.size[1] - 22 * len(lines) - 6
        draw.rectangle((0, top, max(draw.textlength(line, font=SMALL) for line in lines) + 16, image.size[1]), fill=(20, 20, 20))
        for i, line in enumerate(lines):
            draw.text((8, top + 4 + 22 * i), line, fill='white', font=SMALL)
        image.save(OUT / name, optimize=True)
        print(name, image.size)


if __name__ == '__main__':
    main()
