#!/usr/bin/env python3
"""Host side of the GUI fuzzer: a rolling screen record of a lab guest from QMP screendump (never the guest's own capture).

  ring.py RUN_DIR RING_DIR [--every 2] [--keep 40]   keep the last N frames of every head until stopped
  ring.py save RING_DIR OUT_DIR EPOCH [--before 60] [--after 6]   turn the frames around EPOCH into a PNG strip and an mp4

Run it on the QEMU host. Frames are PPM files named <epoch-ms>-head<N>.ppm; the oldest are deleted by exact name.
"""
import argparse
import glob
import os
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)) + '/..')
from lab import QMP  # noqa: E402


def keep_ring(run, ring, every, keep):
    ring = os.path.abspath(ring)
    os.makedirs(ring, exist_ok=True)
    heads = 2
    while True:
        stamp = int(time.time() * 1000)
        try:
            with QMP(os.path.join(run, 'qmp.sock')) as qmp:
                for head in range(heads):
                    qmp.execute('screendump', {'filename': '%s/%d-head%d.ppm' % (ring, stamp, head), 'device': 'video0', 'head': head})
        except Exception as error:  # the guest may be restarting; keep trying
            print('ring:', error, flush=True)
        for head in range(heads):
            frames = sorted(glob.glob('%s/*-head%d.ppm' % (ring, head)))
            for old in frames[:-keep]:
                os.unlink(old)
        time.sleep(every)


def save(ring, out, epoch, before, after):
    os.makedirs(out, exist_ok=True)
    low, high = (epoch - before) * 1000, (epoch + after) * 1000
    for head in (0, 1):
        frames = [f for f in sorted(glob.glob('%s/*-head%d.ppm' % (ring, head))) if low <= int(os.path.basename(f).split('-')[0]) <= high]
        if not frames:
            continue
        work = os.path.join(out, 'frames-head%d' % head)
        os.makedirs(work, exist_ok=True)
        for index, frame in enumerate(frames):
            subprocess.run(['magick', frame, '-resize', '960x', os.path.join(work, '%04d.png' % index)], check=False)
        subprocess.run(['ffmpeg', '-y', '-loglevel', 'error', '-framerate', '2', '-i', os.path.join(work, '%04d.png'), '-c:v', 'libx264', '-pix_fmt',
                        'yuv420p', os.path.join(out, 'last-minute-head%d.mp4' % head)], check=False)
        last = os.path.join(work, '%04d.png' % max(0, len(frames) - 1))
        if os.path.exists(last):
            shutil.copy(last, os.path.join(out, 'final-head%d.png' % head))
    print('saved', out)


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == 'save':
        parser = argparse.ArgumentParser()
        parser.add_argument('command')
        parser.add_argument('ring')
        parser.add_argument('out')
        parser.add_argument('epoch', type=float)
        parser.add_argument('--before', type=int, default=60)
        parser.add_argument('--after', type=int, default=6)
        a = parser.parse_args()
        save(a.ring, a.out, a.epoch, a.before, a.after)
    else:
        parser = argparse.ArgumentParser()
        parser.add_argument('run')
        parser.add_argument('ring')
        parser.add_argument('--every', type=float, default=2)
        parser.add_argument('--keep', type=int, default=40)
        a = parser.parse_args()
        keep_ring(a.run, a.ring, a.every, a.keep)
