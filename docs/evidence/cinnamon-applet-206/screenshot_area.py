#!/usr/bin/env python3
"""Capture one screen rectangle with Cinnamon's ScreenshotArea and print colour stats of it (on the host)."""
import collections, subprocess, sys, time
from PIL import Image
out, x, y, w, h = sys.argv[1], *map(int, sys.argv[2:6])
subprocess.run(['gdbus', 'call', '--session', '--dest', 'org.Cinnamon', '--object-path', '/org/Cinnamon', '--method',
                'org.Cinnamon.ScreenshotArea', 'false', str(x), str(y), str(w), str(h), 'false', out],
               check=True, capture_output=True, timeout=10, env={'DBUS_SESSION_BUS_ADDRESS': 'unix:path=/run/user/1000/bus', 'PATH': '/usr/bin:/bin'})
for _ in range(20):
    try:
        crop = Image.open(out).convert('RGB'); break
    except OSError:
        time.sleep(.1)
counts = collections.Counter(crop.getdata()); total = crop.size[0] * crop.size[1]
print({'size': crop.size, 'top': [('#%02x%02x%02x' % c, round(n / total, 3)) for c, n in counts.most_common(6)]})
