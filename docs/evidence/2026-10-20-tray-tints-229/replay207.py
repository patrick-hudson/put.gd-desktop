"""Replay the #207 SNI reads (no adapter) through tray_progress.check_tooltip's colour gate."""
import importlib.util, json, sys
from pathlib import Path
root = Path(sys.argv[1])
spec = importlib.util.spec_from_file_location('tp', root / 'tools/lab/tray_progress.py')
tp = importlib.util.module_from_spec(spec); spec.loader.exec_module(tp)
tints = getattr(tp, 'TRAY_TINTS', None)
def gate(entry, tint_set):
    snap = {'pid': entry['probePid'], 'registered': entry['registered'], 'hostRegistered': entry['hostRegistered'],
            'title': entry['title'], 'description': entry['tooltip'] or '',
            'icons': [{'opaqueColors': dict(colors)} for colors in entry['iconColors']]}
    try:
        kw = {'tints': tint_set} if tint_set else {}
        tp.check_tooltip(snap, entry['cli'], 's3', entry['probePid'], **kw)
        return 'ok'
    except tp.ObservationMismatch:
        return 'ok'       # colour gate passed; text/counter correlation needs the full rows
    except tp.TrayProgressError as e:
        return 'colour' if 'brand color' in str(e) else 'other:' + str(e)
evidence = root / 'docs/evidence/tray-batch-207'
for host, name in (('d69 #207 run (light tints)', 'observations-d69-plasma.jsonl'),):
    rows = [json.loads(l) for l in (evidence / name).read_text().splitlines()]
    for label in (['dark', 'light'] if tints else [None]):
        results = [gate(r, tints[label] if tints else None) for r in rows]
        first = next((i for i, r in enumerate(results) if r != 'ok'), None)
        print(f'{host:30} tints={label or "hardcoded dark":15} reads={len(rows):3} colour-gate pass={results.count("ok"):3} '
              f'fail={len(rows) - results.count("ok"):3}' + (f'  first failure: read {first + 1} "{rows[first]["tooltip"]}"' if first is not None else ''))
