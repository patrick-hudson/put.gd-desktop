#!/usr/bin/env python3
"""In-guest sampler: put.gd process family memory every 20 ms until a stop file appears.
Family = resident and its helpers by installed executable path. Output: JSON lines."""
import json, os, sys, time
FAMILY = {'/usr/bin/putgd', '/usr/bin/putgd-ui', '/usr/bin/putgd-capture', '/usr/bin/putgd-s3-worker'}
out, stop = sys.argv[1], sys.argv[2]
def kib(text, key):
    for line in text.splitlines():
        if line.startswith(key + ':'):
            return int(line.split()[1])
    return 0
with open(out, 'w') as stream:
    while not os.path.exists(stop):
        now = time.monotonic(); rss = swap = pss = 0; members = []
        for pid in os.listdir('/proc'):
            if not pid.isdigit():
                continue
            try:
                exe = os.readlink(f'/proc/{pid}/exe')
                if exe not in FAMILY:
                    continue
                status = open(f'/proc/{pid}/status').read()
                rollup = open(f'/proc/{pid}/smaps_rollup').read()
            except OSError:
                continue
            r, s, p = kib(status, 'VmRSS'), kib(status, 'VmSwap'), kib(rollup, 'Pss')
            rss += r; swap += s; pss += p
            members.append([int(pid), os.path.basename(exe), r, s, p, kib(status, 'VmHWM')])
        stream.write(json.dumps({'t': round(now, 3), 'rssSwapKiB': rss + swap, 'pssKiB': pss, 'members': members}) + '\n')
        stream.flush()
        time.sleep(.02)
