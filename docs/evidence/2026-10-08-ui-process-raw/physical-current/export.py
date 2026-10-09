#!/usr/bin/env python3
"""Export only explicit completed U11 physical measurement evidence."""
from pathlib import Path
import hashlib,json
root=Path(__file__).resolve().parent
repo=root.parents[2]
out=repo/'docs/evidence/2026-10-08-ui-process-raw/physical-current'
assert json.loads((root/'cold-1/results.json').read_text())['passed']
assert json.loads((root/'matched-execution.json').read_text())['status']=='complete'
assert (root/'comparison.json').is_file()
files=['protocol.json','quiet-gate.json','baseline-binaries.json','view-commands.json','run-matched.py','compare.py','cold_family.py','replay.md','matched-execution.json','comparison.json','latency.py','latency-breakdown.json','decision.md','next-attribution.json','next-attribution.md','root-progress.json','cold-1-driver.log','matched-driver.log','export.py']
for label in ['cold-1','retained-1','split-1','retained-2','split-2']:
 result=json.loads((root/label/'results.json').read_text());assert result['passed']
 files += [f'{label}/{name}' for name in ['results.json','progress.json','samples.jsonl']]
 if label!='cold-1':files.append(label+'.log')
manifest={'schemaVersion':1,'scope':'Explicit U11 physical cold and matched empty-view evidence; excludes private XDG configuration, journal, resident logs and all payloads','files':{}}
for rel in files:
 data=(root/rel).read_bytes();dest=out/rel;dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes(data)
 record={'sha256':hashlib.sha256(data).hexdigest(),'bytes':len(data)}
 assert hashlib.sha256(dest.read_bytes()).hexdigest()==record['sha256'];manifest['files'][rel]=record
p=out/'export-manifest.json';p.write_text(json.dumps(manifest,indent=2)+'\n');assert json.loads(p.read_text())==manifest
print(json.dumps({'files':len(files),'bytes':sum(r['bytes'] for r in manifest['files'].values()),'manifest':str(p)}))
