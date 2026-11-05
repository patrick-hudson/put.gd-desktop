#!/usr/bin/env python3
"""Cancel-latency chart for #203 as SVG: chart203.py DATA.json OUT.svg
DATA: {"title": str, "subtitle": str, "rows": [{"label": str, "before": [ms...], "after": [ms...]}], "note": str}
Horizontal bars, median per variant, the individual runs as dots."""
import json
import statistics
import sys

data = json.load(open(sys.argv[1]))
rows = data['rows']
W, left, right, top, rowh = 1100, 330, 90, 110, 78
H = top + rowh * len(rows) + 90
maxv = max(max(r['before'] + r['after']) for r in rows) * 1.08
scale = (W - left - right) / maxv
BEFORE, AFTER, INK, MUTED, GRID = '#b4553a', '#2f6f8f', '#1f2328', '#59636e', '#d8dee4'
o = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" font-family="Libre Franklin, Helvetica, Arial, sans-serif">',
     f'<rect width="{W}" height="{H}" fill="#ffffff"/>',
     f'<text x="24" y="40" font-size="24" font-weight="700" fill="{INK}">{data["title"]}</text>',
     f'<text x="24" y="68" font-size="15" fill="{MUTED}">{data["subtitle"]}</text>']
# grid
step = 1000 if maxv < 12000 else 5000 if maxv < 60000 else 10000
v = 0
while v <= maxv:
    x = left + v * scale
    o.append(f'<line x1="{x:.1f}" y1="{top - 10}" x2="{x:.1f}" y2="{top + rowh * len(rows)}" stroke="{GRID}" stroke-width="1"/>')
    o.append(f'<text x="{x:.1f}" y="{top + rowh * len(rows) + 22}" font-size="13" fill="{MUTED}" text-anchor="middle">{v / 1000:g} s</text>')
    v += step
for i, r in enumerate(rows):
    y = top + i * rowh
    o.append(f'<text x="{left - 16}" y="{y + 34}" font-size="15" fill="{INK}" text-anchor="end">{r["label"]}</text>')
    for j, (key, color) in enumerate((('before', BEFORE), ('after', AFTER))):
        values = r[key]
        med = statistics.median(values)
        by = y + 8 + j * 30
        o.append(f'<rect x="{left}" y="{by}" width="{max(2, med * scale):.1f}" height="24" fill="{color}" rx="2"/>')
        for value in values:
            o.append(f'<circle cx="{left + value * scale:.1f}" cy="{by + 12}" r="3.5" fill="#ffffff" stroke="{INK}" stroke-width="1.2"/>')
        label = f'{med / 1000:.2f} s' if med < 10000 else f'{med / 1000:.1f} s'
        o.append(f'<text x="{left + max(2, med * scale) + 10:.1f}" y="{by + 18}" font-size="14" font-weight="700" fill="{color}">{key} {label}</text>')
ly = top + rowh * len(rows) + 52
o.append(f'<text x="24" y="{ly}" font-size="13" fill="{MUTED}">{data["note"]}</text>')
o.append('</svg>')
open(sys.argv[2], 'w').write('\n'.join(o))
