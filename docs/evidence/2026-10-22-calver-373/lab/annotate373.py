"""Annotate the #373 k72 frames: a red box around what changed, a label naming it, and a footer naming the
desktop and the exact lab build (PR #390 head d03e80da plus the lab commits 31665552 = N and 91d4cc49 = N+1)."""
import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

R = Path(sys.argv[1])            # the copied k72 results: R/<run>/frames
OUT = Path(sys.argv[2])
FONT = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf', 22)
SMALL = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf', 17)
RED = (220, 30, 40)
K72 = 'k72 Kubuntu 26.04 Plasma 6.6 Wayland'
N, N1, OLD = ('lab 26.1022.949-nightly (31665552, image 728b60e0)', 'lab 26.1022.950-nightly (91d4cc49, image 9f9bfe2b)',
              'published 0.1.1-nightly.925 (image 49c6dfaa)')
LAB = 'PR #390 d03e80da + lab commit, pkg.put.gd/lab'

# (output name, source frame, [(box, label, label side)], build shown)
PICTURES = [
    ('k72-1-calver-offer.png', 'r949/frames/check.png',
     [((650, 734, 1562, 826), 'A calendar version offers the next one: 26.1022.950-nightly', 'below'),
      ((312, 866, 452, 924), 'This build: 26.1022.949-nightly', 'right')], N),
    ('k72-2-updated.png', 'r949/frames/restarted.png',
     [((650, 736, 1462, 814), 'After the restart: 26.1022.950-nightly, updated from 949 by zsync', 'below'),
      ((312, 908, 430, 930), '26.1022.950-nightly', 'right')], 'lab 26.1022.950-nightly (91d4cc49, image 9f9bfe2b), updated in place from 949'),
    ('k72-3-old-build-cannot-read-calver.png', 'r925/frames/invalid-feed.png',
     [((650, 734, 1452, 782), 'A pre-#373 nightly cannot read a calendar version: the check fails, nothing changes', 'below'),
      ((312, 908, 410, 930), '0.1.1-nightly.925', 'right')], OLD + ' against the lab feed'),
]


def label(draw, box, text, side, size):
    w = draw.textlength(text, font=FONT); h = FONT.size + 10
    x0, y0, x1, y1 = box
    if side == 'above': x, y = x0, y0 - h - 6
    elif side == 'right': x, y = x1 + 10, y0 - 4
    else: x, y = x0, y1 + 6
    x = max(4, min(x, size[0] - w - 14)); y = max(4, y)
    draw.rectangle((x, y, x + w + 12, y + h), fill=RED)
    draw.text((x + 6, y + 4), text, fill='white', font=FONT)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    for name, source, marks, build in PICTURES:
        image = Image.open(R / source).convert('RGB')
        draw = ImageDraw.Draw(image)
        for box, text, side in marks:
            draw.rectangle(box, outline=RED, width=4)
            label(draw, box, text, side, image.size)
        footer = f'{K72} - {build} - {LAB}'
        w = draw.textlength(footer, font=SMALL)
        draw.rectangle((0, image.size[1] - 28, w + 16, image.size[1]), fill=(20, 20, 20))
        draw.text((8, image.size[1] - 24), footer, fill='white', font=SMALL)
        image.save(OUT / name, optimize=True)
        print(name, image.size)


if __name__ == '__main__':
    main()
