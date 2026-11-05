#!/usr/bin/env python3
"""probe.py NAME: one upload of out/capture-region.png through Edit & upload (notifications off); prints state, link, error."""
import json, sys, time
sys.path.insert(0, 'tool'); import afa
run = afa.Run(sys.argv[1])
src = run.dir / 'out/capture-region.png'
r = run.ok('editor.open', {'source': 'file', 'path': str(src)}); eid = r['operationId']
run.wait(lambda: (lambda x: x if x.get('ok') and x['result'].get('phase') == 'editing' else None)(run.cli('editor.inspect', {'id': eid})), 30)
sub = run.ok('editor.submit', {'id': eid})
op = afa.operation_done(run, sub.get('operationId') or eid)
s = afa.summarize_op(op or {})
errs = [l.get('fields', {}).get('error') for l in run.logs('Upload failed')][-1:]
print(json.dumps({'state': s.get('state'), 'links': [i['link'] for i in s.get('items', [])], 'error': s.get('error'), 'uploadError': errs}, default=str))
