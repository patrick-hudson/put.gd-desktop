#!/usr/bin/env python3
"""alias.py OUT TITLE ROW... where ROW is 'caption|from.png|step|to.png' (from may be '-').

Each row: an optional starting window, the step that was taken, the window it led to.
Window crops are scaled to one width; the History title and rail item of each result
are boxed (positions are fractions of the put.gd main window, the same on every desktop).
"""
import sys
from PIL import Image, ImageDraw, ImageFont
dst, title, *rows = sys.argv[1:]
BOLD = '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'
MONO = '/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf'
ft, fc, fm = ImageFont.truetype(BOLD, 18), ImageFont.truetype(BOLD, 15), ImageFont.truetype(MONO, 13)
TW = 440; pad = 18; arrow = 210; ACC = (255, 64, 129)
def load(p):
    im = Image.open(p).convert('RGB'); s = TW / im.width
    return im.resize((TW, round(im.height * s)), Image.LANCZOS), s, im
def selected_row(im):
    # The rail's selected item is a pale blue rounded row (about 220,231,250) spanning most of
    # the rail: keep rows where over 120 of the left 220 px have that colour.
    def blue(px): r, g, b = px; return abs(r - 218) < 9 and abs(g - 231) < 5 and abs(b - 250) < 5
    rows = [y for y in range(im.height) if sum(blue(im.getpixel((x, y))) for x in range(0, min(220, im.width))) > 120]
    if not rows: return None
    run = [rows[0]]
    for y in rows[1:]:
        if y == run[-1] + 1: run.append(y)
        elif len(run) < 20: run = [y]
        else: break
    xs = [x for x in range(0, 220) if blue(im.getpixel((x, run[len(run) // 2])))]
    return (min(xs), run[0], max(xs), run[-1])
parts = []
for r in rows:
    cap, a, step, b = r.split('|')
    parts.append((cap, load(a) if a != '-' else None, step, load(b)))
th = max(p[3][0].height for p in parts)
W = pad + TW + arrow + TW + pad
W = max(W, int(ImageDraw.Draw(Image.new('RGB', (1, 1))).textlength(title, font=ft)) + 2 * pad)
H = 50 + len(parts) * (th + 40 + pad)
out = Image.new('RGB', (W, H), (24, 27, 33)); d = ImageDraw.Draw(out)
d.text((pad, 14), title, font=ft, fill=(231, 233, 238))
y = 50
for cap, a, step, (b, s, b_orig) in parts:
    d.text((pad, y), cap, font=fc, fill=(231, 233, 238)); y += 26
    if a: out.paste(a[0], (pad, y))
    else: d.text((pad + 20, y + th // 2 - 10), '(no window open)', font=fc, fill=(150, 155, 165))
    bx = pad + TW + arrow; out.paste(b, (bx, y))
    # boxes on the History result: the rail's selected row (found by its colour) and the page title above it
    row = selected_row(b_orig)
    boxes = [row, (row[2] + 38, row[1] - 392, row[2] + 134, row[1] - 352)] if row else []
    for (x0, y0, x1, y1) in boxes:
        for i in range(3): d.rectangle((bx + x0 * s - i, y + y0 * s - i, bx + x1 * s + i, y + y1 * s + i), outline=ACC)
    ay = y + th // 2; ax0, ax1 = pad + TW + 10, bx - 10
    d.line((ax0, ay, ax1, ay), fill=ACC, width=4); d.polygon([(ax1 + 4, ay), (ax1 - 10, ay - 8), (ax1 - 10, ay + 8)], fill=ACC)
    lines = step.split('\\n')
    for i, l in enumerate(lines):
        tw = d.textlength(l, font=fm); ty = ay - 18 - (len(lines) - i) * 18 + 18
        d.rectangle((ax0 + (ax1 - ax0 - tw) / 2 - 4, ty - 2, ax0 + (ax1 - ax0 + tw) / 2 + 4, ty + 15), fill=(40, 44, 54))
        d.text((ax0 + (ax1 - ax0 - tw) / 2, ty), l, font=fm, fill=(255, 210, 225))
    y += th + 14 + pad
out.save(dst, optimize=True); print(dst, out.size)
