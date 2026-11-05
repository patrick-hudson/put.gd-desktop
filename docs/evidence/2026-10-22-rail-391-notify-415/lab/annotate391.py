#!/usr/bin/env python3
"""Annotate a picture: red boxes with labels, and a caption band (build, desktop, what changed) above it.

    annotate391.py IN OUT --caption "line 1|line 2" [--box x0,y0,x1,y1,"label"]... [--crop x0,y0,x1,y1] [--scale 2]
"""
import argparse
from PIL import Image, ImageDraw, ImageFont

RED = (230, 30, 40)


def font(size, bold=False):
    for path in (f'/usr/share/fonts/truetype/dejavu/DejaVuSans{"-Bold" if bold else ""}.ttf',
                 f'/usr/share/fonts/dejavu-sans-fonts/DejaVuSans{"-Bold" if bold else ""}.ttf'):
        try: return ImageFont.truetype(path, size)
        except OSError: pass
    return ImageFont.load_default()


def main():
    p = argparse.ArgumentParser(); p.add_argument('src'); p.add_argument('out'); p.add_argument('--caption', required=True)
    p.add_argument('--box', action='append', default=[]); p.add_argument('--crop'); p.add_argument('--scale', type=float, default=1)
    a = p.parse_args()
    im = Image.open(a.src).convert('RGB')
    boxes = []
    for spec in a.box:
        *nums, label = spec.split(',', 4); boxes.append(([int(v) for v in nums], label))
    if a.crop:
        cx0, cy0, cx1, cy1 = (int(v) for v in a.crop.split(',')); im = im.crop((cx0, cy0, cx1, cy1))
        boxes = [([b[0] - cx0, b[1] - cy0, b[2] - cx0, b[3] - cy0], l) for b, l in boxes]
    if a.scale != 1:
        im = im.resize((round(im.width * a.scale), round(im.height * a.scale)), Image.LANCZOS)
        boxes = [([round(v * a.scale) for v in b], l) for b, l in boxes]
    d = ImageDraw.Draw(im); f = font(max(13, round(14 * min(a.scale, 1.6))), bold=True)
    for (x0, y0, x1, y1), label in boxes:
        d.rectangle([x0, y0, x1, y1], outline=RED, width=3)
        if label:
            tw, th = d.textbbox((0, 0), label, font=f)[2:]
            if x1 + tw + 18 < im.width: tx, ty = x1 + 8, max(0, (y0 + y1) // 2 - th // 2 - 3); d.line([x1, (y0 + y1) // 2, tx, (y0 + y1) // 2], fill=RED, width=3)
            else: ty = y0 - th - 8 if y0 - th - 8 > 0 else y1 + 4; tx = min(max(0, x0), im.width - tw - 8)
            d.rectangle([tx, ty, tx + tw + 8, ty + th + 6], fill=RED); d.text((tx + 4, ty + 2), label, fill='white', font=f)
    lines = a.caption.split('|'); cf = font(16, bold=True); sf = font(14)
    heights = [d.textbbox((0, 0), l, font=cf if i == 0 else sf)[3] + 6 for i, l in enumerate(lines)]
    band = sum(heights) + 14
    out = Image.new('RGB', (im.width, im.height + band), (250, 250, 250)); out.paste(im, (0, band))
    dd = ImageDraw.Draw(out); y = 7
    for i, l in enumerate(lines): dd.text((10, y), l, fill=(20, 20, 20), font=cf if i == 0 else sf); y += heights[i]
    dd.line([0, band - 1, out.width, band - 1], fill=(160, 160, 160), width=1)
    out.save(a.out, optimize=True)


if __name__ == '__main__':
    main()
