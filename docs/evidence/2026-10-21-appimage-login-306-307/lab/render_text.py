#!/usr/bin/env python3
"""render_text.py IN.txt OUT.png TITLE ['REGEX=>label' ...]: a captured terminal log as a picture;
lines matching each REGEX get a box and a labelled arrow in the right-hand column."""
import re, sys
from PIL import Image, ImageDraw, ImageFont
src, dst, title, *marks = sys.argv[1:]
MONO = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf', 14)
BOLD = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf', 15)
lines = open(src).read().rstrip('\n').split('\n')
pad, line_h, band = 14, 19, 36
probe = ImageDraw.Draw(Image.new('RGB', (1, 1)))
text_w = int(max(probe.textlength(l, font=MONO) for l in lines)) + 2 * pad
rules = [(re.compile(m.partition('=>')[0]), m.partition('=>')[2]) for m in marks]
column = int(max([probe.textlength(l, font=BOLD) for _, l in rules if l] + [0])) + 70 if rules else 0
width = max(text_w + column + pad, int(probe.textlength(title, font=BOLD)) + 2 * pad)
height = band + len(lines) * line_h + 2 * pad
img = Image.new('RGB', (width, height), (24, 27, 33)); d = ImageDraw.Draw(img)
d.text((pad, 10), title, font=BOLD, fill=(231, 233, 238))
d.rectangle((pad - 6, band, text_w, height - pad + 6), fill=(13, 15, 19))
ACCENT = (255, 64, 129)
used = set()
for i, line in enumerate(lines):
    y = band + pad // 2 + i * line_h
    d.text((pad, y), line, font=MONO, fill=(214, 218, 224))
    for k, (rx, label) in enumerate(rules):
        if rx.search(line):
            x1 = pad + int(d.textlength(line, font=MONO)) + 4
            for j in range(2): d.rectangle((pad - 4 - j, y - 2 - j, x1 + j, y + line_h - 3 + j), outline=ACCENT)
            if label and k not in used:
                used.add(k); cy = y + line_h // 2 - 2; lx = text_w + 30
                d.line((x1 + 3, cy, lx, cy), fill=ACCENT, width=3)
                d.polygon([(x1 + 3, cy), (x1 + 15, cy - 7), (x1 + 15, cy + 7)], fill=ACCENT)
                tw = d.textlength(label, font=BOLD)
                d.rectangle((lx, cy - 12, lx + tw + 10, cy + 12), fill=ACCENT); d.text((lx + 5, cy - 10), label, font=BOLD, fill=(255, 255, 255))
img.save(dst, optimize=True); print(dst, img.size)
