#!/usr/bin/env python3
"""Pair fixed phases by PID/TID, then correlate numeric syscall intervals."""
import json
import re
from decimal import Decimal
from pathlib import Path

ROOT = Path(__file__).resolve().parent
rows = []
for line in (ROOT / 'phases.log').read_text().splitlines():
    mono, pid, parent, tid, phase, edge, wall = line.split()
    rows.append(dict(mono=int(mono), pid=int(pid), tid=int(tid), phase=phase,
                     edge=edge, wall=int(wall)))
rows.sort(key=lambda r: r['mono'])
pairs = [('history-commit', 'history-commit-before', 'history-commit-after'),
         ('foreign-keys', 'foreign-keys-before', 'foreign-keys-after'),
         ('user-version-exec', 'user-version-exec-before', 'user-version-exec-after'),
         ('user-version-next', 'user-version-next-before', 'user-version-next-after')]
pending, spans = {}, []
for row in rows:
    name, edge = row['phase'], row['edge']
    for phase, begin, end in pairs:
        if name == begin:
            name, edge = phase, 'begin'
        elif name == end:
            name, edge = phase, 'end'
    key = row['pid'], row['tid'], name
    if edge == 'begin':
        pending.setdefault(key, []).append(row)
    elif edge == 'end':
        assert pending.get(key), ('unpaired end', row)
        start = pending[key].pop()
        assert row['mono'] >= start['mono']
        spans.append(dict(pid=row['pid'], tid=row['tid'], phase=name,
                          begin=start['mono'], end=row['mono'],
                          wallBegin=start['wall'], wallEnd=row['wall'],
                          durationMs=(row['mono']-start['mono'])/1e6))

calls, unfinished = [], {}
line_re = re.compile(r'^(\d+)\s+(\d+\.\d+)\s+(.+)$')
duration_re = re.compile(r'<(\d+\.\d+)>$')
for line in (ROOT / 'syscalls.log').read_text().splitlines():
    match = line_re.match(line)
    if not match:
        continue
    tid, wall, body = match.groups()
    tid, wall = int(tid), int(Decimal(wall)*1_000_000_000)
    if '<unfinished ...>' in body:
        name = body.split('(', 1)[0]
        unfinished[tid] = name, wall
        continue
    resumed = re.match(r'<\.\.\. (\w+) resumed>', body)
    if resumed:
        name, wall = unfinished.pop(tid)
        assert name == resumed.group(1)
    else:
        name = body.split('(', 1)[0]
    duration = duration_re.search(body)
    if duration and name in {'fsync', 'fdatasync', 'fcntl', 'clock_nanosleep'}:
        elapsed = int(Decimal(duration.group(1))*1_000_000_000)
        calls.append(dict(tid=tid, name=name, begin=wall, end=wall+elapsed,
                          durationMs=elapsed/1e6))

def correlated(span):
    selected = [c for c in calls if c['tid'] == span['tid'] and
                span['wallBegin'] <= c['begin'] < span['wallEnd']]
    return {name: dict(count=len(found), totalMs=sum(c['durationMs'] for c in found),
                      maxMs=max((c['durationMs'] for c in found), default=0))
            for name in sorted({c['name'] for c in selected})
            for found in [[c for c in selected if c['name'] == name]]}

invokes = [r for r in rows if r['phase'] == 'invoke']
groups = []
for index, invoke in enumerate(invokes):
    boundary = invokes[index+1]['mono'] if index+1 < len(invokes) else 10**30
    workers = [s for s in spans if s['phase'] == 'accepted-worker' and
               s['pid'] == invoke['pid'] and invoke['mono'] <= s['begin'] < boundary]
    assert len(workers) == 1, (index, workers)
    accepted = workers[0]
    own = [s for s in spans if s['pid'] == accepted['pid'] and s['tid'] == accepted['tid']
           and accepted['begin'] <= s['begin'] and s['end'] <= accepted['end']]
    prior = [s for s in spans if s['phase'] in {'finish-worker', 'history-commit'}
             and s['pid'] == accepted['pid'] and s['tid'] != accepted['tid']
             and s['begin'] < accepted['begin'] < s['end']]
    def detail(span):
        return dict(**span, beginRelativeMs=(span['begin']-invoke['mono'])/1e6,
                    endRelativeMs=(span['end']-invoke['mono'])/1e6,
                    syscalls=correlated(span))
    groups.append(dict(index=index, warmup=index<5, worker=detail(accepted),
                       spans=[detail(s) for s in own], priorOverlaps=[detail(s) for s in prior]))
result = dict(schemaVersion=2, unscored=True,
              pairing='same PID/TID LIFO phase pairing; accepted worker owns its nested spans',
              syscallScope='same TID and start within exact realtime span; names/timings only',
              groups=groups, unpairedBegins=sum(map(len,pending.values())),
              incompleteSyscalls=len(unfinished))
(ROOT / 'summary.json').write_text(json.dumps(result, indent=2)+'\n')
print(json.dumps({'invocations':len(groups), 'unpairedBegins':result['unpairedBegins'],
                  'incompleteSyscalls':len(unfinished), 'slowWorkers':[
                      {'index':g['index'], 'warmup':g['warmup'],
                       'workerMs':g['worker']['durationMs'],
                       'phases':[{k:s[k] for k in ('phase','durationMs','syscalls')} for s in g['spans']],
                       'priorOverlaps':[{k:s[k] for k in ('phase','durationMs','endRelativeMs','syscalls')} for s in g['priorOverlaps']]}
                      for g in groups if g['worker']['durationMs'] > 100]}, indent=2))
