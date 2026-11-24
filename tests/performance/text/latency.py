#!/usr/bin/env python3
"""Summarize candidate observer monotonic timestamps, never infer engine timings.

Input JSONL rows: {operationId, selection_end_ns, spawn_ns, engine_start_ns,
engine_ready_ns, recognition_end_ns, copy_ack_ns}. All use the SAME monotonic
clock. Spawn/engine start/recognition/copy remain separate in each receipt.
Missing points or out-of-order clocks block scoring; no fabricated zero values.
"""
import argparse
import json
import math
from pathlib import Path

POINTS=('selection_end_ns','spawn_ns','engine_start_ns','engine_ready_ns','recognition_end_ns','copy_ack_ns')


def measure(row):
    ticks=[row[k] for k in POINTS]
    if any(type(t)!=int or t<0 for t in ticks) or ticks != sorted(ticks):
        raise ValueError('timestamps must be ordered monotonic integer nanoseconds')
    durations={name:(ticks[b]-ticks[a])/1e6 for name,a,b in
               [('spawn_ms',0,2),('engine_start_ms',2,3),('recognition_ms',3,4),('copy_ms',4,5),('total_ms',0,5)]}
    durations.update(operationId=row['operationId'],dispatch_to_spawn_ms=(ticks[1]-ticks[0])/1e6,
                     spawn_to_engine_ms=(ticks[2]-ticks[1])/1e6)
    return durations


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('events',type=Path)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args()
    rows=[measure(json.loads(line)) for line in a.events.read_text().splitlines() if line.strip()]
    if len({r['operationId'] for r in rows})!=len(rows): raise ValueError('duplicate operation ID')
    p95={key:sorted(r[key] for r in rows)[math.ceil(.95*len(rows))-1] for key in rows[0] if key.endswith('_ms')} if rows else {}
    with a.output.open('x') as out:
        json.dump(dict(rows=rows,p95=p95,sample_count=len(rows),
                       status='measured' if rows else 'blocked',
                       assessment='Compare these observations with the issue plan; no acceptance verdict.'),out,indent=2)
    return 0 if rows else 2


if __name__=='__main__': raise SystemExit(main())
