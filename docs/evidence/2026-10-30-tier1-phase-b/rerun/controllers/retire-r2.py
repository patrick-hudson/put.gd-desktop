from pathlib import Path
import hashlib,json,sys
from tools.lab import lab
row,run_text=sys.argv[1:];run=Path(run_text).resolve();out=Path('/home/patrick/t1r2export')/row
info=json.loads((run/'state.json').read_text());receipt=json.loads((out/'sha256.json').read_text())
assert all((out/p).is_file() and hashlib.sha256((out/p).read_bytes()).hexdigest()==sha for p,sha in receipt.items())
batons=json.loads((out/'batons.json').read_text())['lends'];assert all(v['finalStep']=='cleaned' and 'replace_confirmed' in v['carryBackStates'] and v['guestEntryState']=='cleanup_confirmed' for v in batons)
completion=dict(schemaVersion=1,kind='vm-overlay-cleanup-evidence',run=str(run),runId=info['id'],completed=True,outcome='failed',evidenceExportVerified=True)
(out/'completion.json').write_text(json.dumps(completion,indent=2))
files={str(p.relative_to(out)):hashlib.sha256(p.read_bytes()).hexdigest() for p in out.rglob('*') if p.is_file() and p.name!='completion-manifest.json'}
(out/'completion-manifest.json').write_text(json.dumps(dict(completion,files=files),indent=2))
lab.reset(run)
