#!/usr/bin/env python3
"""Verify every native head is fresh on one combined loopback VNC stream.

  wall_probe.py --port 5902 --layout native-layout.json --output NEW_DIR

Run on the desktop host, under its desktop lease. The layout is an explicit
JSON list of {name,x,y,width,height} in actual logical compositor coordinates.
Cover all heads with the moving multi_output_fixture first. Two raw frames
must match the actual bounding rectangle and change independently inside EVERY
head. A stale-sized w0 stream is a failure: renew only the pilot-owned server,
then rerun into a new directory. Never restart Patrick's original wall service.
Screenshots are saved only after each head has the known fixture colours.
"""
import argparse
import hashlib
import json
from pathlib import Path
import time

import rfb

COLORS = [(220, 30, 40), (20, 180, 70), (35, 70, 220), (235, 190, 20),
          (30, 200, 210), (180, 40, 200), (230, 110, 30), (70, 90, 110)]
MIN_CHANGED_FRACTION = .01


def rectangle_bytes(frame, width, rect):
    x, y, w, h = rect
    return b''.join(frame[((y + row) * width + x) * 4:((y + row) * width + x + w) * 4] for row in range(h))


def check_frames(first, second, layout):
    left = min(m['x'] for m in layout); top = min(m['y'] for m in layout)
    width = max(m['x'] + m['width'] for m in layout) - left
    height = max(m['y'] + m['height'] for m in layout) - top
    if first[:2] != (width, height) or second[:2] != (width, height):
        return {'ok': False, 'reason': 'stream-size-stale-or-incomplete', 'expected': [width, height],
                'first': list(first[:2]), 'second': list(second[:2]), 'heads': []}
    results = []
    if any(len(f[2]) != width * height * 4 for f in (first, second)):
        return {'ok': False, 'reason': 'frame-buffer-incomplete', 'heads': []}
    for m in layout:
        rect = [m['x'] - left, m['y'] - top, m['width'], m['height']]
        a = rectangle_bytes(first[2], width, rect); b = rectangle_bytes(second[2], width, rect)
        # BGRX input; ignore X since its value need not be stable.
        rgb = bytes(v for i, v in enumerate(b) if i % 4 != 3)
        known = {tuple(reversed(c)) for c in COLORS} | {(255, 255, 255)}
        covered = sum(tuple(rgb[i:i + 3]) in known for i in range(0, len(rgb), 3)) / (len(rgb) // 3)
        changed = sum(a[i:i + 3] != b[i:i + 3] for i in range(0, len(a), 4))
        pixels = m['width'] * m['height']
        required = max(2, (pixels + 99) // 100)
        results.append({'name': m['name'], 'rect': rect, 'fixtureFraction': covered,
                        'changedPixels': changed, 'changedFraction': changed / pixels,
                        'requiredChangedPixels': required, 'minimumChangedFraction': MIN_CHANGED_FRACTION,
                        'ok': covered >= .99 and changed >= required})
    return {'ok': all(h['ok'] for h in results), 'heads': results, 'streamSize': [width, height],
            'frameSha256': [hashlib.sha256(f[2]).hexdigest() for f in (first, second)]}


def save_checked_frame(client, previous, layout, path):
    """Validate the actual saved frame, not just the frames preceding it."""
    try:
        frame = client.screenshot(path)
        gate = check_frames(previous, frame, layout)
        if not gate['ok']:
            path.unlink(missing_ok=True)
        return gate
    except BaseException:
        path.unlink(missing_ok=True)
        raise


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--port', type=int, required=True); p.add_argument('--layout', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args(); a.output.mkdir(mode=0o700, exist_ok=False)
    layout = json.loads(a.layout.read_text())
    if len(layout) < 2 or any(type(m[k]) is not int for m in layout for k in ('x', 'y', 'width', 'height')):
        raise ValueError('At least two actual native output rectangles required')
    client = rfb.RFB(a.port)
    try:
        client.frame()  # discard startup/negotiation frame
        time.sleep(.25); first = client.frame()
        time.sleep(.5); second = client.frame()
        result = check_frames(first, second, layout)
        if result['ok']:
            # Take a final screenshot only while the same synthetic cover is live.
            shot = a.output / 'all-heads.png'
            gate = save_checked_frame(client, second, layout, shot)
            if not gate['ok']:
                result.update(ok=False, finalFrameGate=gate)
    finally:
        client.close()
    (a.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result)); return 0 if result['ok'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
