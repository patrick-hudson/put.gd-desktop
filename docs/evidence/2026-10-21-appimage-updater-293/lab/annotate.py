"""Annotate the #293 phase 3 acceptance frames: a red box around what changed and a label naming it and the build."""
import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

R = Path('/tmp/claude-1000/-home-patrick-work-put-gd--claude-worktrees-focused-mclean-595322/dd81c141-d160-4c73-8934-746abda5948f/scratchpad/results2')
OUT = Path(__file__).resolve().parent / 'out'
FONT = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf', 22)
SMALL = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf', 18)
RED = (220, 30, 40)
BUILD = 'lab 0.1.1-nightly.886 -> 887, #343 head 04aff235'

# (output name, source frame, [(box, label, label side)], footer)
PICTURES = [
    ('f44-1-download-and-install.png', 'f44/f44-a/frames/check.png',
     [((645, 782, 1570, 872), 'Update found: Download and install (AppImage only)', 'above')], 'f44 Fedora 44 GNOME Wayland'),
    ('f44-2-zsync-progress.png', 'f44/f44-a/frames/install-zsync.png',
     [((645, 832, 1570, 935), 'zsync through the host appimageupdatetool, with progress', 'above')], 'f44 Fedora 44 GNOME Wayland'),
    ('f44-3-installed-restart.png', 'f44/f44-a/frames/install-result.png',
     [((645, 832, 1570, 935), 'Installed and verified: Restart put.gd; old image kept as hidden backup', 'above')], 'f44 Fedora 44 GNOME Wayland'),
    ('f44-4-updated.png', 'f44/f44-a/frames/restarted.png',
     [((645, 782, 1570, 862), 'After the restart: 887 runs, up to date, "Updated from 886 to 887"', 'above'),
      ((308, 952, 430, 980), '887', 'right')], 'f44 Fedora 44 GNOME Wayland'),
    ('k72-forced-zsync-crash-fallback.png', 'k72/k72-c/frames/install-result.png',
     [((645, 782, 1570, 875), 'zsync crashed (forced): full download, verified, installed', 'above'),
      ((1550, 858, 1886, 992), 'the forced crash', 'above')], 'k72 Kubuntu 26.04 Plasma 6.6 Wayland'),
    ('d69-updated.png', 'd69/d69-a/frames/restarted.png',
     [((645, 740, 1570, 818), 'After the restart: up to date, "Updated from 886 to 887"', 'above')], 'd69 Debian 13 Plasma Wayland'),
    ('m74-tampered-download-refused.png', 'm74/m74-t/frames/tamper-image.png',
     [((395, 776, 1320, 810), 'Tampered download refused: nothing was changed', 'above')], 'm74 Linux Mint 22.3 Cinnamon X11'),
    ('aurora-updated.png', 'aurora/aurora-pic/frames/restarted.png',
     [((245, 592, 1232, 670), 'After the restart: up to date, "Updated from 886 to 887"', 'above')], 'Aurora 44 Plasma 6.7 Wayland (VM on f44)'),
]


def label(draw, box, text, side, font=FONT):
    w = draw.textlength(text, font=font); h = font.size + 10
    x0, y0, x1, y1 = box
    if side == 'above': x, y = x0, y0 - h - 6
    elif side == 'right': x, y = x1 + 10, y0
    else: x, y = x0, y1 + 6
    x = max(4, min(x, draw.im.size[0] - w - 14))
    draw.rectangle((x, y, x + w + 12, y + h), fill=RED)
    draw.text((x + 6, y + 4), text, fill='white', font=font)


def main():
    OUT.mkdir(exist_ok=True)
    for name, source, marks, where in PICTURES:
        image = Image.open(R / source).convert('RGB')
        draw = ImageDraw.Draw(image)
        for box, text, side in marks:
            draw.rectangle(box, outline=RED, width=4)
            label(draw, box, text, side)
        footer = f'{where} - {BUILD}'
        w = draw.textlength(footer, font=SMALL)
        draw.rectangle((0, image.size[1] - 30, w + 16, image.size[1]), fill=(20, 20, 20))
        draw.text((8, image.size[1] - 26), footer, fill='white', font=SMALL)
        image.save(OUT / name, optimize=True)
        print(name, image.size)


if __name__ == '__main__':
    main()
