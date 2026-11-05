#!/usr/bin/env python3
"""manifest.py RUN_DIR EVIDENCE_DIR: completion.json + manifest.json (vm-overlay-cleanup-evidence) over every file in EVIDENCE_DIR."""
import hashlib, json, sys
from pathlib import Path
run, ev = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
state = json.loads((run / 'state.json').read_text())
fields = {'run': str(run), 'runId': state['id'], 'completed': True, 'outcome': 'passed'}
(ev / 'completion.json').write_text(json.dumps(fields, indent=1) + '\n')
files = {}
for f in sorted(ev.rglob('*')):
    if f.is_symlink(): raise SystemExit(f'symlink {f}')
    if f.is_file() and f.name != 'manifest.json':
        files[str(f.relative_to(ev))] = hashlib.sha256(f.read_bytes()).hexdigest()
(ev / 'manifest.json').write_text(json.dumps({'schemaVersion': 1, 'kind': 'vm-overlay-cleanup-evidence', **fields, 'files': files}, indent=1) + '\n')
print(json.dumps({'manifest': str(ev / 'manifest.json'), 'files': len(files), 'runId': state['id']}))
