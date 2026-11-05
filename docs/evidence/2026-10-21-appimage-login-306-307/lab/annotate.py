#!/usr/bin/env python3
"""annotate.py IN OUT CAPTION 'x0,y0,x1,y1:label' ... — boxes on the picture, labels in a right-hand column with arrows."""
import sys
from PIL import Image, ImageDraw, ImageFont
src, dst, caption, *boxes = sys.argv[1:]
FONT = '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'
img = Image.open(src).convert('RGB'); pad = 14; band = 36
font = ImageFont.truetype(FONT, 16); cap = ImageFont.truetype(FONT, 15)
probe = ImageDraw.Draw(img)
labels = [b.partition(':')[2] for b in boxes]
column = max([probe.textlength(l, font=font) for l in labels if l] + [0])
column = int(column + 60) if column else 0
width = max(img.width + 2 * pad + column, int(probe.textlength(caption, font=cap)) + 2 * pad)
out = Image.new('RGB', (width, img.height + band + 2 * pad), (24, 27, 33))
out.paste(img, (pad, band + pad)); d = ImageDraw.Draw(out)
d.text((pad, 10), caption, font=cap, fill=(231, 233, 238))
ACCENT = (255, 64, 129)
for spec in boxes:
    coords, _, label = spec.partition(':')
    x0, y0, x1, y1 = (int(v) for v in coords.split(','))
    x0 += pad; x1 += pad; y0 += band + pad; y1 += band + pad
    for i in range(3): d.rectangle((x0 - i, y0 - i, x1 + i, y1 + i), outline=ACCENT)
    if label:
        lx = img.width + pad + 40; cy = (y0 + y1) // 2
        d.line((x1 + 3, cy, lx, cy), fill=ACCENT, width=3)
        d.polygon([(x1 + 3, cy), (x1 + 15, cy - 7), (x1 + 15, cy + 7)], fill=ACCENT)
        tw = d.textlength(label, font=font)
        d.rectangle((lx, cy - 12, lx + tw + 10, cy + 12), fill=ACCENT); d.text((lx + 5, cy - 10), label, font=font, fill=(255, 255, 255))
out.save(dst, optimize=True)
print(dst, out.size)
