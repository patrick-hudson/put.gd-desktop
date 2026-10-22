#!/usr/bin/env python3
"""#333 before/after picture: the KDE selector at the drag plus the PNG put.gd saved.

annotate333.py FRAME RESULT_PNG OUT --drag x0,y0,x1,y1 --desk TEXT --build TEXT [--map TEXT] [--input TEXT]

FRAME is a full-desktop VNC frame taken while the button is still down; RESULT_PNG
is the capture put.gd saved after the release. The picture boxes the selector,
magnifies the drag's end corner 10x with the intended pixel edges marked, insets
the saved PNG with its measured size, and captions the desktop and exact build.
"""
import argparse
from PIL import Image, ImageDraw, ImageFont

BOLD = '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'
REG = '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf'
ACCENT, EDGE, GOOD, BAD = (255, 64, 129), (0, 229, 255), (76, 195, 138), (240, 122, 106)
WIDTH = 1280


def main():
    p = argparse.ArgumentParser(); p.add_argument('frame'); p.add_argument('result'); p.add_argument('out')
    p.add_argument('--drag', required=True); p.add_argument('--desk', required=True); p.add_argument('--build', required=True); p.add_argument('--map', default=''); p.add_argument('--input', default='real uinput drag')
    a = p.parse_args()
    x0, y0, x1, y1 = [int(v) for v in a.drag.split(',')]
    want = (x1 - x0, y1 - y0)
    frame = Image.open(a.frame).convert('RGB'); result = Image.open(a.result)
    got = result.size; ok = got == want
    s = WIDTH / frame.width
    pic = frame.resize((WIDTH, round(frame.height * s)), Image.LANCZOS)
    d = ImageDraw.Draw(pic)
    f = lambda size, bold=True: ImageFont.truetype(BOLD if bold else REG, size)
    # Box around the selector outline (outside it, so the outline itself stays visible).
    d.rectangle([x0 * s - 6, y0 * s - 6, x1 * s + 6, y1 * s + 6], outline=ACCENT, width=3)
    label = f'selector at the drag: ({x0},{y0}) → ({x1},{y1}) = {want[0]}×{want[1]}'
    lx = min(x0 * s - 6, WIDTH - d.textlength(label, font=f(17)) - 330)
    d.text((lx, y0 * s - 30), label, font=f(17), fill=ACCENT, stroke_width=3, stroke_fill=(0, 0, 0))
    small = (x1 - x0) * s < 400
    # 10x zooms of the two cursor-free corners; cyan marks the drag's end edges
    # (pixel boundaries x1 and y1). The outline is painted inside the selection,
    # so its outer black pixel is the selection's last pixel.
    r, z = 6, 12
    def inset(cx, cy, vertical, zx, zy, title):
        c = frame.crop((cx - r, cy - r, cx + r, cy + r)).resize((2 * r * z, 2 * r * z), Image.NEAREST)
        cd = ImageDraw.Draw(c)
        for i in range(0, 2 * r * z + 1, z):
            cd.line([(i, 0), (i, 2 * r * z)], fill=(70, 70, 70)); cd.line([(0, i), (2 * r * z, i)], fill=(70, 70, 70))
        if vertical: cd.line([(r * z, 0), (r * z, 2 * r * z)], fill=EDGE, width=2)
        else: cd.line([(0, r * z), (2 * r * z, r * z)], fill=EDGE, width=2)
        pic.paste(c, (zx, zy)); d.rectangle([zx - 2, zy - 2, zx + 2 * r * z + 1, zy + 2 * r * z + 1], outline=ACCENT, width=3)
        # The outline's outermost black pixel along the middle of that edge.
        if vertical: last = next(x for x in range(x1 + 4, x1 - 8, -1) if frame.getpixel((x, (y0 + y1) // 2)) == (0, 0, 0))
        else: last = next(y for y in range(y1 + 4, y1 - 8, -1) if frame.getpixel(((x0 + x1) // 2, y)) == (0, 0, 0))
        edge = x1 if vertical else y1; inside = last == edge - 1
        d.text((zx, zy - 24), f"cyan = {'x' if vertical else 'y'} {edge}; outline ends at {last}", font=f(15, False), fill=EDGE if inside else BAD,
               stroke_width=3, stroke_fill=(0, 0, 0))
        d.text((zx, zy - 46), title + (' (inside)' if inside else ' (1 px past)'), font=f(16), fill=ACCENT, stroke_width=3, stroke_fill=(0, 0, 0))
        d.line([(zx + r * z, zy + (2 * r * z if zy < cy * s else 0)), (cx * s, cy * s)], fill=ACCENT, width=2)
    side = 2 * r * z
    if small:
        inset(x1, y0, True, 40, 150, 'right edge ×12')
        inset(x0, y1, False, 40, 150 + side + 90, 'bottom edge ×12')
    else:
        inset(x1, y0, True, round(x1 * s) - side - 230, round(y0 * s) + 70, 'right edge ×12')
        inset(x0, y1, False, round(x0 * s) + 60, round(y1 * s) - side - 40, 'bottom edge ×12')
    # The saved PNG with its measured size.
    tw = 300; thumb = result.convert('RGB'); thumb = thumb.resize((tw, max(1, round(thumb.height * tw / thumb.width))), Image.LANCZOS) if thumb.width > tw else thumb
    if thumb.width < 150: thumb = thumb.resize((thumb.width * 3, thumb.height * 3), Image.NEAREST)
    plabel = f'saved PNG {got[0]}×{got[1]}' + (' (shown ×3)' if result.width < 150 else '')
    tx, ty = WIDTH - max(thumb.width, round(d.textlength(plabel, font=f(22)))) - 24, 70
    colour = GOOD if ok else BAD
    pic.paste(thumb, (tx, ty)); d.rectangle([tx - 3, ty - 3, tx + thumb.width + 2, ty + thumb.height + 2], outline=colour, width=4)
    d.text((tx, ty + thumb.height + 8), plabel, font=f(22), fill=colour, stroke_width=3, stroke_fill=(0, 0, 0))
    d.text((tx, ty + thumb.height + 36), ('= ' if ok else '≠ ') + f'drag {want[0]}×{want[1]}', font=f(18), fill=colour, stroke_width=3, stroke_fill=(0, 0, 0))
    d.line([(tx - 4, ty + thumb.height // 2), (x1 * s + 8, y0 * s + 8)], fill=colour, width=2)
    # Caption.
    bar = 64; out = Image.new('RGB', (WIDTH, pic.height + bar), (17, 19, 23)); out.paste(pic, (0, 0))
    od = ImageDraw.Draw(out)
    od.text((14, pic.height + 8), f'{a.desk} · put.gd {a.build}', font=f(19), fill=(231, 233, 238))
    od.text((14, pic.height + 36), f'Region capture, {a.input}{(" (" + a.map + ")") if a.map else ""}: PNG {got[0]}×{got[1]} for a {want[0]}×{want[1]} drag — {"exact" if ok else "one pixel too large"}',
            font=f(16, False), fill=colour)
    out.save(a.out, optimize=True)
    print(a.out, out.size, 'ok' if ok else 'mismatch', got, want)


if __name__ == '__main__':
    main()
