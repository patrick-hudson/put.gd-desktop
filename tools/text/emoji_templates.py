#!/usr/bin/env python3
"""Builds the emoji templates putgd-ocr matches on screen (#1748).

Every fully-qualified emoji of Unicode's emoji-test.txt is drawn by Pango and
cairo in each font that shows it as one glyph, at 96 px. The glyph's ink box
(alpha above 8) is stretched to SIDE x SIDE and COARSE x COARSE pixels by area
averaging, as premultiplied 8-bit RGBA. Emoji that are plain shapes (keycaps,
coloured circles and squares, dashes, arrows, signs in a square) are marked as
shapes: on a screen they are buttons, status dots and coloured letters far more
often than emoji, so putgd-ocr matches them, to keep a red dot from passing for
a stop sign, but never names one. Nothing here runs at build time: the output is committed as
src/ocr/emoji/templates.bin with its inputs in SOURCE.md.

    FONTCONFIG_FILE=fonts.conf tools/text/emoji_templates.py \\
      --emoji-test emoji-test.txt --style noto="Noto Color Emoji" \\
      --style twemoji="Twemoji Mozilla" --side 20 --output templates.bin

fonts.conf must make each named family resolvable. Needs PyGObject (Pango,
PangoCairo), pycairo and numpy.

The file, little-endian:
  0  "PGEM", u32 version 1, u32 count, u32 side, u32 coarse side,
     u32 string offset, u32 string bytes, u32 coarse offset, u32 pixel offset,
     three u32 0
  48 count entries of 12 bytes: u32 string offset, u8 string bytes, u8 style
     (the --style order), u16 aspect (ink box width / height x 1024), u8 colour
     (the share of the glyph's opaque pixels that are colour, x 255), u8 flags
     (1: a plain shape, matched but never named), u16 0
  then the UTF-8 strings, then count x coarse x coarse x 4 bytes, then
  count x side x side x 4 bytes, each block starting on 64 bytes.
"""
import argparse, hashlib, struct, sys

import cairo
import gi
import numpy as np

gi.require_version('Pango', '1.0')
gi.require_version('PangoCairo', '1.0')
from gi.repository import Pango, PangoCairo  # noqa: E402

RENDER = 96
CHROMA = 32  # max - min of R, G, B: a colour pixel, as putgd-ocr counts it

# Plain shapes, by code point: an emoji of one of these alone (with or without
# its variation selector) is a shape, and so is every keycap. A sequence that
# only uses one, such as a woman running (U+2640 after a joiner), is not.
SHAPES = [
    (0x00A9, 0x00A9), (0x00AE, 0x00AE), (0x203C, 0x203C), (0x2049, 0x2049),  # (c) (r) !! !?
    (0x2122, 0x2122), (0x2139, 0x2139), (0x2194, 0x21AA), (0x23E9, 0x23EF),  # tm, i, arrows, media
    (0x23F8, 0x23FA), (0x24C2, 0x24C2), (0x25AA, 0x25FE), (0x2611, 0x2611),  # media, (M), squares, ballot
    (0x2640, 0x2642), (0x2648, 0x2653), (0x2660, 0x2668), (0x267E, 0x267E),  # signs, zodiac, suits, infinity
    (0x2695, 0x2695), (0x26A7, 0x26A7), (0x26AA, 0x26AB), (0x26CE, 0x26CE),  # medical, gender, circles, ophiuchus
    (0x2714, 0x2714), (0x2716, 0x2716), (0x2733, 0x2734), (0x2747, 0x2747),  # check, x, asterisks, sparkle
    (0x2753, 0x2757), (0x2795, 0x2797), (0x27A1, 0x27A1), (0x27B0, 0x27BF),  # ? !, + - /, arrow, loops
    (0x2934, 0x2935), (0x2B05, 0x2B07), (0x2B1B, 0x2B1C), (0x2B55, 0x2B55),  # arrows, squares, circle
    (0x3030, 0x3030), (0x303D, 0x303D), (0x3297, 0x3299),                    # wavy dash, part mark, ideographs
    (0x1F170, 0x1F251),  # letters and ideographs in squares and circles
    (0x1F4B2, 0x1F4B2), (0x1F500, 0x1F504), (0x1F518, 0x1F518),              # dollar, arrows, radio button
    (0x1F51F, 0x1F524), (0x1F532, 0x1F53D), (0x1F6AE, 0x1F6AE), (0x1F6B9, 0x1F6BC),  # 10, abc, shapes, signs
    (0x1F7E0, 0x1F7EB), (0x1F7F0, 0x1F7F0), (0x267F, 0x267F), (0x1F6BE, 0x1F6BE),    # circles, squares, =, signs
    (0x20E3, 0x20E3),  # keycaps
]


def plain_shape(text):
    points = [c for c in text if c != '\ufe0f']
    if '\u20e3' in points:
        return True
    return len(points) == 1 and any(lo <= ord(points[0]) <= hi for lo, hi in SHAPES)


def sequences(path):
    out = []
    for line in open(path, encoding='utf-8'):
        if '; fully-qualified' not in line:
            continue
        out.append(''.join(chr(int(c, 16)) for c in line.split(';')[0].split()))
    return out


def render(text, family):
    """The glyph as unpremultiplied RGBA, or None when the font draws it as more than one glyph."""
    side = int(RENDER * 2.2)
    surface = cairo.ImageSurface(cairo.FORMAT_ARGB32, side, side)
    context = cairo.Context(surface)
    layout = PangoCairo.create_layout(context)
    font = Pango.FontDescription.from_string(family)
    font.set_absolute_size(RENDER * Pango.SCALE)
    layout.set_font_description(font)
    layout.set_text(text, -1)
    attributes = Pango.AttrList()
    attributes.insert(Pango.attr_fallback_new(False))
    layout.set_attributes(attributes)
    if layout.get_unknown_glyphs_count():
        return None
    glyphs, runs = 0, layout.get_iter()
    while True:
        run = runs.get_run_readonly()
        if run is not None:
            glyphs += run.glyphs.num_glyphs
        if not runs.next_run():
            break
    if glyphs != 1:
        return None
    context.move_to(RENDER * 0.3, RENDER * 0.2)
    PangoCairo.show_layout(context, layout)
    surface.flush()
    bgra = np.ndarray((side, surface.get_stride() // 4, 4), np.uint8, surface.get_data())[:, :side]
    return bgra[:, :, [2, 1, 0, 3]].astype(np.float64)  # cairo's ARGB32 is premultiplied BGRA


def area_matrix(n, m):
    """Rows average the input pixels each of n output pixels covers, from m input pixels."""
    matrix = np.zeros((n, m))
    for i in range(n):
        start, end = i * m / n, (i + 1) * m / n
        for j in range(int(start), min(m, int(np.ceil(end)))):
            matrix[i, j] = max(0.0, min(end, j + 1) - max(start, j))
        matrix[i] /= matrix[i].sum()
    return matrix


def stretch(box, side):
    height, width = box.shape[:2]
    rows, columns = area_matrix(side, height), area_matrix(side, width)
    out = np.einsum('ah,hwc,bw->abc', rows, box, columns, optimize=True)
    return np.clip(np.rint(out), 0, 255).astype(np.uint8).tobytes()


def template(premultiplied, side, coarse):
    alpha = premultiplied[:, :, 3]
    ys, xs = np.nonzero(alpha > 8)
    if len(xs) == 0:
        return None
    box = premultiplied[ys.min():ys.max() + 1, xs.min():xs.max() + 1]
    height, width = box.shape[:2]
    opaque = box[:, :, 3] > 128
    rgb = box[:, :, :3] * 255 / np.maximum(box[:, :, 3:4], 1)
    colour = (rgb.max(axis=2) - rgb.min(axis=2) >= CHROMA) & opaque
    share = round(255 * colour.sum() / max(1, opaque.sum()))
    return stretch(box, side), stretch(box, coarse), width / height, share


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--emoji-test', required=True)
    parser.add_argument('--style', action='append', required=True, help='name="Font family"')
    parser.add_argument('--side', type=int, default=20)
    parser.add_argument('--coarse', type=int, default=10)
    parser.add_argument('--output', required=True)
    arguments = parser.parse_args()
    styles = [s.split('=', 1) for s in arguments.style]
    entries, strings, coarse, pixels = [], bytearray(), bytearray(), bytearray()
    shapes = [text for text in sequences(arguments.emoji_test) if plain_shape(text)]
    print(f'{len(shapes)} plain shapes', file=sys.stderr)
    for style, (name, family) in enumerate(styles):
        kept = 0
        for text in sequences(arguments.emoji_test):
            drawn = render(text, family)
            made = None if drawn is None else template(drawn, arguments.side, arguments.coarse)
            if made is None:
                continue
            image, small, aspect, share = made
            encoded = text.encode('utf-8')
            entries.append((len(strings), len(encoded), style, min(65535, round(aspect * 1024)), share,
                            1 if plain_shape(text) else 0))
            strings += encoded
            coarse += small
            pixels += image
            kept += 1
        print(f'{name}: {kept} templates', file=sys.stderr)
    align = lambda n: (n + 63) // 64 * 64  # noqa: E731
    string_offset = 48 + 12 * len(entries)
    coarse_offset = align(string_offset + len(strings))
    pixel_offset = align(coarse_offset + len(coarse))
    out = bytearray(b'PGEM')
    out += struct.pack('<11I', 1, len(entries), arguments.side, arguments.coarse, string_offset, len(strings),
                       coarse_offset, pixel_offset, 0, 0, 0)
    for entry in entries:
        out += struct.pack('<IBBHBBH', *entry, 0)
    out += strings
    out += bytes(coarse_offset - len(out))
    out += coarse
    out += bytes(pixel_offset - len(out))
    out += pixels
    open(arguments.output, 'wb').write(out)
    print(f'{arguments.output}: {len(out)} bytes, sha256 {hashlib.sha256(out).hexdigest()}', file=sys.stderr)


if __name__ == '__main__':
    main()
