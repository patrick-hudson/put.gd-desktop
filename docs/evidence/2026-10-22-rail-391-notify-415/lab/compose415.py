#!/usr/bin/env python3
"""#415 frame: the banner crop with the real click marked, what the click did, and a caption naming the build."""
import json, sys
from PIL import Image, ImageDraw
sys.path.insert(0, sys.path[0])
from annotate391 import font, RED

trial, banner, out = sys.argv[1], sys.argv[2], sys.argv[3]
after = sys.argv[4] if len(sys.argv) > 4 and sys.argv[4] != '-' else None
caption = sys.argv[5].split('|')
t = json.load(open(trial))
inner = t['banner'].get('inner') or t['banner']['components'][0]['box']
cx, cy = t['clicked'][0] - inner[0], t['clicked'][1] - inner[1]
scale = 2
b = Image.open(banner).convert('RGB'); b = b.resize((b.width * scale, b.height * scale), Image.LANCZOS)
d = ImageDraw.Draw(b); x, y = cx * scale, cy * scale
d.ellipse([x - 14, y - 14, x + 14, y + 14], outline=RED, width=4); d.line([x - 22, y, x + 22, y], fill=RED, width=2); d.line([x, y - 22, x, y + 22], fill=RED, width=2)
panels = [('1. The banner; the red mark is where the single real click landed (first click)', b)]
log = [f'2. Notification server: {", ".join(t["dbusMembers"][2:])}', f'   ActionInvoked: {t["actionInvoked"]} ({t["pressAfterShownSeconds"]} s after the banner showed)']
log += [f'   Stand-in URL recorder: {u.split(" lib=")[0]}' for u in t['openedUrls'][-1:]]
if after:
    a = Image.open(after).convert('RGB'); a = a.resize((a.width * scale, a.height * scale), Image.LANCZOS)
    panels.append(('3. The link opened in the stand-in recorder (its own pop-up), not a browser', a))
hf, lf = font(15, True), font(14)
width = max(900, *(p.width + 20 for _, p in panels))
height = 14 + 22 * len(caption) + sum(26 + p.height + 12 for _, p in panels) + 22 * len(log) + 20
img = Image.new('RGB', (width, height), (250, 250, 250)); dd = ImageDraw.Draw(img); yy = 8
for i, line in enumerate(caption): dd.text((10, yy), line, fill=(20, 20, 20), font=hf if i == 0 else lf); yy += 22
dd.line([0, yy + 2, width, yy + 2], fill=(160, 160, 160)); yy += 10
title, panel = panels[0]; dd.text((10, yy), title, fill=RED, font=hf); yy += 24; img.paste(panel, (10, yy)); yy += panel.height + 12
for line in log: dd.text((10, yy), line, fill=(20, 20, 20), font=lf); yy += 22
yy += 6
for title, panel in panels[1:]: dd.text((10, yy), title, fill=RED, font=hf); yy += 24; img.paste(panel, (10, yy)); yy += panel.height + 12
img.crop((0, 0, width, yy + 6)).save(out, optimize=True)
