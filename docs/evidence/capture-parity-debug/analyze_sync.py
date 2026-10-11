#!/usr/bin/env python3
"""Recompute locating-only sync/lock durations from numeric archived traces."""
from pathlib import Path
from decimal import Decimal
import json
import re

root = Path(__file__).resolve().parent / 'phases' / 'journal-sync'
clock = json.loads((root / 'parameters.json').read_text())['clockPair']
offset = clock['monotonicNs'] - clock['realtimeNs']
points = []
for line in (root / 'phases.log').read_text().splitlines():
    t, pid, parent, tid, phase, edge = line.split()
    points.append((int(t), int(tid), phase, edge))
points.sort()
calls = []
unfinished = {}
for line in (root / 'syscalls-numeric.log').read_text().splitlines():
    match = re.fullmatch(r'(\d+) (\d+\.\d+) (.*)', line)
    if not match:
        continue
    tid, stamp, body = int(match[1]), match[2], match[3]
    started = int(Decimal(stamp) * 1000000000) + offset
    if '<unfinished ...>' in body:
        unfinished[tid] = (started, body.split('(', 1)[0])
        continue
    resume = re.match(r'<\.\.\. (\w+) resumed>', body)
    if resume:
        if tid not in unfinished:
            continue
        started, syscall = unfinished.pop(tid)
        assert syscall == resume[1]
    else:
        syscall = body.split('(', 1)[0]
    duration = re.search(r'<(\d+\.\d+)>$', body)
    if duration:
        calls.append((started, tid, syscall, float(duration[1]) * 1000,
                      bool(re.search(r'\b(?:EAGAIN|EACCES|EWOULDBLOCK)\b', body))))
begins = [point for point in points if point[2:] == ('accepted-worker', 'begin')]
rows = []
for begin, tid, phase, edge in begins[5:]:
    end = next(t for t, thread, phase, edge in points
               if thread == tid and t >= begin and (phase, edge) == ('accepted-worker', 'end'))
    start = next(t for t, thread, phase, edge in points
                 if thread == tid and begin <= t <= end and phase == 'history-commit-before')
    stop = next(t for t, thread, phase, edge in points
                if thread == tid and start <= t <= end and phase == 'history-commit-after')
    selected = [call for call in calls if call[1] == tid and start <= call[0] <= stop]
    sync = [call for call in selected if call[2] in ('fsync', 'fdatasync')]
    lock = [call for call in selected if call[2] in ('fcntl', 'flock')]
    rows.append({'threadId': tid, 'commitMs': (stop-start)/1e6,
                 'syncMs': sum(call[3] for call in sync), 'syncCalls': len(sync),
                 'lockSyscallMs': sum(call[3] for call in lock),
                 'failedLockAttempts': sum(call[4] for call in lock)})
print(json.dumps({'schemaVersion': 1, 'unscored': True,
                  'scope': 'Archived numeric same-thread intervals; calibrated clocks; no FD paths',
                  'samples': rows}, indent=2))
