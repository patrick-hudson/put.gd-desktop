"""Annotated before/after pictures for #283 from verified window crops (Pillow)."""
import json, sys
from PIL import Image, ImageDraw, ImageFont

FONT = '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf'
BOLD = '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'
COLOURS = {'green': (22, 163, 74), 'red': (220, 38, 38), 'orange': (234, 88, 12), 'blue': (37, 99, 235), 'violet': (124, 58, 237)}


def panel(spec, scheme):
    im = Image.open(spec['image']).convert('RGB')
    pad_top, pad = 46, 18
    w, h = im.size
    canvas = Image.new('RGB', (w + 2 * pad, h + pad_top + pad + 120), (233, 235, 239) if scheme == 'light' else (27, 30, 36))
    canvas.paste(im, (pad, pad_top))
    d = ImageDraw.Draw(canvas)
    title_colour = (27, 30, 36) if scheme == 'light' else (231, 233, 238)
    d.text((pad, 12), spec['title'], font=ImageFont.truetype(BOLD, 20), fill=title_colour)
    # The window's outline, so its size reads against the background.
    d.rectangle([pad - 1, pad_top - 1, pad + w, pad_top + h], outline=(120, 126, 138), width=1)
    label_font = ImageFont.truetype(BOLD, 14)
    for box in spec['boxes']:
        colour = COLOURS[box['colour']]
        x, y, bw, bh = box['rect']
        d.rectangle([pad + x, pad_top + y, pad + x + bw - 1, pad_top + y + bh - 1], outline=colour, width=4)
        lx, ly = box.get('label_at', [x + 8, y + 8])
        text = box['label']
        tw = d.textlength(text, font=label_font)
        d.rounded_rectangle([pad + lx - 6, pad_top + ly - 4, pad + lx + tw + 6, pad_top + ly + 20], radius=6, fill=colour)
        d.text((pad + lx, pad_top + ly), text, font=label_font, fill=(255, 255, 255))
        if 'arrow_to' in box:
            ax, ay = box['arrow_to']
            sx, sy = pad + lx + tw / 2, pad_top + ly + 20
            d.line([sx, sy, pad + ax, pad_top + ay], fill=colour, width=3)
            d.ellipse([pad + ax - 5, pad_top + ay - 5, pad + ax + 5, pad_top + ay + 5], fill=colour)
    y = pad_top + h + 12
    for line in spec.get('notes', []):
        d.text((pad, y), line, font=ImageFont.truetype(FONT, 15), fill=title_colour)
        y += 22
    return canvas.crop((0, 0, canvas.width, y + 8))


def main():
    spec = json.load(open(sys.argv[1]))
    panels = [panel(p, spec['scheme']) for p in spec['panels']]
    width = sum(p.width for p in panels)
    height = max(p.height for p in panels)
    out = Image.new('RGB', (width, height), (233, 235, 239) if spec['scheme'] == 'light' else (27, 30, 36))
    x = 0
    for p in panels:
        out.paste(p, (x, 0)); x += p.width
    out.save(spec['out'], optimize=True)


if __name__ == '__main__':
    main()
