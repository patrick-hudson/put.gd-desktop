#!/usr/bin/env python3
"""Render a terminal transcript as a PNG with boxed, labelled lines.

usage: render.py TRANSCRIPT OUT.png TITLE ANNOTATIONS.json
ANNOTATIONS.json: [{"match": "regex", "label": "text", "color": "#rrggbb"}, ...]
Each annotation boxes the first line (after the previous annotation's line) that
matches and puts its label in the right-hand margin with an arrow.
"""
import json
import re
import sys
from PIL import Image, ImageDraw, ImageFont

transcript, out, title, notes = sys.argv[1:5]
lines = [line.rstrip('\n').expandtabs() for line in open(transcript, encoding='utf-8', errors='replace')]
annotations = json.load(open(notes))
mono = ImageFont.truetype('/usr/share/fonts/google-noto-vf/NotoSansMono[wght].ttf', 15)
sans = ImageFont.truetype('/usr/share/fonts/google-noto-vf/NotoSansMono[wght].ttf', 15)
bold = ImageFont.truetype('/usr/share/fonts/google-noto-vf/NotoSansMono[wght].ttf', 20)
try:
    bold.set_variation_by_name('Bold')
    sans.set_variation_by_name('SemiBold')
except Exception:
    pass
cell_w = mono.getbbox('M')[2]
line_h = 21
max_cols = 132
wrapped, origin = [], []
for index, line in enumerate(lines):
    chunks = [line[i:i + max_cols] for i in range(0, max(len(line), 1), max_cols)] or ['']
    for chunk in chunks:
        wrapped.append(chunk)
        origin.append(index)
marks = {}
start = 0
for note in annotations:
    pattern = re.compile(note['match'])
    if 'after' in note:
        start = next(i for i in range(start, len(lines)) if re.search(note['after'], lines[i]))
    for index in range(start, len(lines)):
        if pattern.search(lines[index]):
            marks[index] = note
            start = index + 1
            break
    else:
        raise SystemExit('annotation did not match: ' + note['match'])
pad, top, margin = 18, 56, 500
width = pad * 2 + cell_w * max_cols + margin
height = top + line_h * len(wrapped) + pad
image = Image.new('RGB', (width, height), '#1e1f22')
draw = ImageDraw.Draw(image)
draw.rectangle((0, 0, width, 44), fill='#2b2d31')
draw.text((pad, 10), title, font=bold, fill='#f2f2f2')
for row, text in enumerate(wrapped):
    y = top + row * line_h
    color = '#8ab4f8' if text.startswith('$ ') else '#d4d4d4'
    draw.text((pad, y), text, font=mono, fill=color)
label_y = top
for index, note in marks.items():
    rows = [row for row, value in enumerate(origin) if value == index]
    y0 = top + rows[0] * line_h - 2
    y1 = top + (rows[-1] + 1) * line_h
    x1 = pad + cell_w * max(len(wrapped[row]) for row in rows) + 6
    draw.rectangle((pad - 6, y0, x1, y1), outline=note['color'], width=3)
    label_x = pad + cell_w * max_cols + 40
    text_lines = note['label'].split('\n')
    ly = max(label_y, y0 - 4)
    draw.line((x1, (y0 + y1) // 2, label_x - 8, ly + 10), fill=note['color'], width=3)
    draw.polygon([(x1, (y0 + y1) // 2), (x1 + 12, (y0 + y1) // 2 - 6), (x1 + 12, (y0 + y1) // 2 + 6)], fill=note['color'])
    for k, text in enumerate(text_lines):
        draw.text((label_x, ly + k * 19), text, font=sans, fill=note['color'])
    label_y = ly + len(text_lines) * 19 + 8
image.save(out)
print(out, image.size)
