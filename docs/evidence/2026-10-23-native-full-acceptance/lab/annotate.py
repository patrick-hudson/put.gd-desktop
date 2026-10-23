#!/usr/bin/env python3
"""annotate.py SPEC.json OUTDIR: annotated full-desktop pictures for the native acceptance README.

SPEC is a list of {"src", "out", "desktop", "caption", "boxes": [[x0, y0, x1, y1, "label"], ...]} with boxes in the
frame's own pixels. Each picture gets its boxes and labels in the accent colour and a footer naming the build and
the desktop, is scaled to at most 1280 px wide and saved as an 8-bit palette PNG.
"""
import json, sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

FONT = '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'
ACCENT = (255, 64, 129)
BUILD = 'put.gd 26.1023.1058-nightly (05991d9d)'


def font(size):
    try: return ImageFont.truetype(FONT, size)
    except OSError: return ImageFont.load_default()


def annotate(spec, outdir):
    im = Image.open(spec['src']).convert('RGB')
    w, h = im.size
    d = ImageDraw.Draw(im)
    f = font(max(14, w // 80))
    for x0, y0, x1, y1, *label in spec.get('boxes', []):
        for i in range(4): d.rectangle([x0 - i, y0 - i, x1 + i, y1 + i], outline=ACCENT)
        if label and label[0]:
            tw = d.textlength(label[0], font=f); th = f.size + 8
            ly = y0 - th - 6 if y0 - th - 6 > 0 else y1 + 6
            lx = min(max(x0, 0), w - tw - 12)
            d.rectangle([lx, ly, lx + tw + 12, ly + th], fill=ACCENT)
            d.text((lx + 6, ly + 3), label[0], fill=(255, 255, 255), font=f)
    foot = font(max(14, w // 90)); fh = foot.size * 2 + 22
    out = Image.new('RGB', (w, h + fh), (17, 19, 23)); out.paste(im, (0, 0))
    d = ImageDraw.Draw(out)
    d.text((12, h + 8), f"{BUILD} · {spec['desktop']}", fill=(255, 255, 255), font=foot)
    d.text((12, h + 12 + foot.size), spec['caption'], fill=(200, 205, 214), font=foot)
    if out.width > 1280: out = out.resize((1280, round(out.height * 1280 / out.width)), Image.LANCZOS)
    path = Path(outdir) / spec['out']
    out.quantize(colors=256, method=Image.Quantize.MEDIANCUT).save(path, optimize=True)
    return str(path)


if __name__ == '__main__':
    specs = json.loads(Path(sys.argv[1]).read_text())
    Path(sys.argv[2]).mkdir(parents=True, exist_ok=True)
    for s in specs: print(annotate(s, sys.argv[2]))
