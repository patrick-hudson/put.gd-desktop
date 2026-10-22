#!/usr/bin/env python3
"""pair.py BEFORE AFTER OUT TITLE BEFORE_LABEL AFTER_LABEL BEFORE_BOX AFTER_Y NOTE

Before and after crops side by side on a dark band, each captioned with its build;
a box around the removed row in the before crop, and a marker line where it was in
the after crop. Boxes are crop coordinates: x0,y0,x1,y1; AFTER_Y is one y.
"""
import sys
from PIL import Image, ImageDraw, ImageFont
before, after, dst, title, lb, la, box, ay, note = sys.argv[1:]
BOLD = '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'
REG = '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf'
ft, fl, fn = ImageFont.truetype(BOLD, 16), ImageFont.truetype(BOLD, 13), ImageFont.truetype(BOLD, 14)
a, b = Image.open(before).convert('RGB'), Image.open(after).convert('RGB')
pad, top, gap, side = 18, 96, 40, 230
h = max(a.height, b.height)
W = pad + a.width + gap + b.width + side + pad
W = max(W, int(ImageDraw.Draw(Image.new('RGB', (1, 1))).textlength(title, font=ft)) + 2 * pad)
H = top + h + pad + 26
out = Image.new('RGB', (W, H), (24, 27, 33)); d = ImageDraw.Draw(out)
d.text((pad, 12), title, font=ft, fill=(231, 233, 238))
ax, bx = pad, pad + a.width + gap
for x, label, colour in ((ax, lb, (255, 170, 170)), (bx, la, (150, 230, 170))):
    head, _, rest = label.partition(': ')
    d.text((x, 44), head, font=fl, fill=colour); d.text((x, 62), rest, font=fl, fill=colour)
out.paste(a, (ax, top)); out.paste(b, (bx, top))
ACC = (255, 64, 129)
x0, y0, x1, y1 = (int(v) for v in box.split(','))
for i in range(3): d.rectangle((ax + x0 - i, top + y0 - i, ax + x1 + i, top + y1 + i), outline=ACC)
y = top + int(ay)
d.line((bx - 6, y, bx + b.width + 6, y), fill=ACC, width=3)
# arrow from the side column to the marker, with the note
lx = bx + b.width + 30
d.line((bx + b.width + 8, y, lx, y), fill=ACC, width=3)
d.polygon([(bx + b.width + 8, y), (bx + b.width + 20, y - 7), (bx + b.width + 20, y + 7)], fill=ACC)
words, lines, cur = note.split(), [], ''
for w in words:
    t = (cur + ' ' + w).strip()
    if d.textlength(t, font=fn) > side - 20: lines.append(cur); cur = w
    else: cur = t
lines.append(cur)
for i, l in enumerate(lines):
    d.rectangle((lx, y - 12 + i * 22, lx + d.textlength(l, font=fn) + 10, y + 10 + i * 22), fill=ACC)
    d.text((lx + 5, y - 10 + i * 22), l, font=fn, fill=(255, 255, 255))
out.save(dst, optimize=True); print(dst, out.size)
