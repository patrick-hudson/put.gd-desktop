#!/usr/bin/env python3
"""The #418 merged-head re-check picture (rule 16): f44, 786ed9fc's CI AppImage, History after the region capture and
the CVE-2025-10729 SVG through Upload clipboard. annotate_rck.py FRAME OUT"""
import sys
import annotate418 as a
from PIL import Image, ImageDraw, ImageFont

src, dst = sys.argv[1], sys.argv[2]
card = a.selected_card(src)
boxes = [(card, 'CVE-2025-10729 SVG', False),
         ([1050, 268, 1582, 566], 'renders on Qt 6.12.0, no crash', False),
         ([544, 412, 1014, 510], 'A: region capture saved', True),
         ([314, 946, 428, 980], 'build', False)]
img = Image.open(src).convert('RGB'); d = ImageDraw.Draw(img); font = ImageFont.truetype(a.FONT, 26)
for b, label, below in boxes:
    x0, y0, x1, y1 = b
    for i in range(5): d.rectangle((x0 - 3 - i, y0 - 3 - i, x1 + 3 + i, y1 + 3 + i), outline=a.ACCENT)
    tw = d.textlength(label, font=font); ly = y1 + 12 if below else y0 - 44
    lx = min(max(x0 - 8, 4), img.width - tw - 24)
    d.rectangle((lx, ly, lx + tw + 16, ly + 36), fill=a.ACCENT); d.text((lx + 8, ly + 3), label, font=font, fill=(255, 255, 255))
img.save(dst + '.full.png')
a.draw(dst + '.full.png', dst, ('f44 Fedora 44 GNOME 50.0 Wayland · put.gd 26.1023.1101-dev+gaa047473ad2e, Qt 6.12.0 (CI AppImage of merged head 786ed9fc, 9918e7ea)',
                               'Rule-16 re-check: region capture (A), then the CVE-2025-10729 SVG through Upload clipboard, opened in History'), [])
print(card)
