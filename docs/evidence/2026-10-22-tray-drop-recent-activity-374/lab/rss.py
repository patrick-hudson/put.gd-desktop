#!/usr/bin/env python3
"""rss.py ROOTPID SECONDS: sample VmRSS of ROOTPID and its descendants once a second."""
import json, sys, time
from pathlib import Path
root, secs = int(sys.argv[1]), int(sys.argv[2])
def family():
    kids = {}
    for p in Path('/proc').iterdir():
        if p.name.isdigit():
            try: kids.setdefault(int(p.joinpath('stat').read_text().rsplit(')', 1)[1].split()[1]), []).append(int(p.name))
            except OSError: pass
    out, todo = [], [root]
    while todo:
        pid = todo.pop(); out.append(pid); todo += kids.get(pid, [])
    return out
samples = []
for _ in range(secs):
    total, procs = 0, {}
    for pid in family():
        try:
            st = Path(f'/proc/{pid}/status').read_text()
            name = next(l.split()[1] for l in st.splitlines() if l.startswith('Name:'))
            rss = int(next(l.split()[1] for l in st.splitlines() if l.startswith('VmRSS:')))
            procs[f'{name}:{pid}'] = rss; total += rss
        except (OSError, StopIteration): pass
    samples.append({'total_kib': total, 'procs': procs}); time.sleep(1)
tot = [s['total_kib'] for s in samples]
print(json.dumps({'root': root, 'seconds': secs, 'min_mib': round(min(tot)/1024, 2), 'median_mib': round(sorted(tot)[len(tot)//2]/1024, 2), 'max_mib': round(max(tot)/1024, 2), 'last': samples[-1]['procs']}, indent=1))
