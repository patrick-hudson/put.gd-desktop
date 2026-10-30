#!/bin/bash
# retire.sh RUN_NAME EVIDENCE_DIR: stop the overlay, record completion, guarded cleanup.
set -u
cd ~/work/put.gd/src
run=$PWD/work/lab-runs/$1; ev=$2; mkdir -p $ev/retire
runid=$(python3 -c "import json;print(json.load(open('$run/state.json'))['id'])")
timeout 300 python3 tools/lab/lab.py reset --run "$run" > $ev/retire/reset.json 2> $ev/retire/reset.err; echo reset=$?
python3 - "$run" "$runid" "$ev" <<'PY'
import hashlib, json, sys
from pathlib import Path
run, runid, ev = sys.argv[1], sys.argv[2], Path(sys.argv[3])
(ev/'completion.json').write_text(json.dumps({'run': run, 'runId': runid, 'completed': True, 'outcome': 'passed'}, indent=1)+'\n')
files = {str(p.relative_to(ev)): hashlib.sha256(p.read_bytes()).hexdigest()
         for p in sorted(ev.rglob('*')) if p.is_file() and p.name != 'cleanup-manifest.json' and 'retire' not in p.parts}
(ev/'cleanup-manifest.json').write_text(json.dumps({'schemaVersion': 1, 'kind': 'vm-overlay-cleanup-evidence', 'run': run,
    'runId': runid, 'completed': True, 'outcome': 'passed', 'files': files}, indent=1)+'\n')
print('manifest files', len(files))
PY
timeout 300 python3 tools/lab/lab.py cleanup --run "$run" --evidence-manifest $ev/cleanup-manifest.json --images-root /home/user/work/put.gd/work/lab-images --dry-run > $ev/retire/cleanup-dry.json 2> $ev/retire/cleanup-dry.err; echo dry=$?
timeout 300 python3 tools/lab/lab.py cleanup --run "$run" --evidence-manifest $ev/cleanup-manifest.json --images-root /home/user/work/put.gd/work/lab-images > $ev/retire/cleanup.json 2> $ev/retire/cleanup.err; echo cleanup=$?
tail -c 400 $ev/retire/cleanup.json; tail -3 $ev/retire/cleanup.err
test -e $run/disk.qcow2 && echo disk-still-present || echo disk-removed
