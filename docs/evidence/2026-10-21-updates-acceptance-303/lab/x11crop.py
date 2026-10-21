#!/usr/bin/env python3
"""Crop one of our own X11 windows: viewable, nothing stacked above it overlapping, put.gd theme colours >= 40%."""
import json, os, re, subprocess, sys
sys.path.insert(0, '/home/patrick/work/put.gd-303'); sys.path.insert(0, '/home/patrick/work/put.gd-303/src-head/tools/lab')
import accept303 as a
win, out = sys.argv[1], sys.argv[2]
env = dict(os.environ, DISPLAY=':0', XAUTHORITY='/home/patrick/.Xauthority')
def geo(w):
    t = subprocess.run(['xwininfo', '-id', w], env=env, capture_output=True, text=True).stdout
    g = {k: int(re.search(k + r':\s+(-?\d+)', t).group(1)) for k in ('Absolute upper-left X', 'Absolute upper-left Y', 'Width', 'Height')}
    return [g['Absolute upper-left X'], g['Absolute upper-left Y'], g['Absolute upper-left X'] + g['Width'], g['Absolute upper-left Y'] + g['Height']], 'IsViewable' in t
box, viewable = geo(win)
stack = subprocess.run(['xprop', '-root', '_NET_CLIENT_LIST_STACKING'], env=env, capture_output=True, text=True).stdout.split('#')[1].replace(' ', '').split(',')
ids = [int(s, 16) for s in stack]; mine = int(win, 0)
# The client list holds frame-less client ids; compare with the frame-less id of our window.
above = []
if mine in ids:
    for other in ids[ids.index(mine) + 1:]:
        b, v = geo(hex(other))
        if v and not (b[2] <= box[0] or b[0] >= box[2] or b[3] <= box[1] or b[1] >= box[3]): above.append(hex(other))
LIGHT = [(245, 246, 248), (255, 255, 255), (237, 239, 243), (220, 231, 250), (213, 217, 224)]
f = a.Frames().grab()
total = hit = 0
for y in range(box[1], box[3], 2):
    for x in range(box[0], box[2], 2):
        total += 1; hit += any(not a.differs(a.pixel(f, x, y), c, 3) for c in LIGHT + a.THEME)
share = hit / total
report = {'window': win, 'box': box, 'viewable': viewable, 'inStacking': mine in ids, 'overlappingAbove': above, 'themeShare': round(share, 3)}
if viewable and mine in ids and not above and share >= .4:
    a.write_png(f, box, out); report['kept'] = out
print(json.dumps(report))
