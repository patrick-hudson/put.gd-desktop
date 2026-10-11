#!/usr/bin/env python3
"""Freeze explicit native evidence without exporting guest profile or transport keys."""
import json,shutil
from common import OUT,RUN,now,save,sha,source
frozen=OUT/'selected-evidence';frozen.mkdir(exist_ok=False)
allowed={'.py','.sh','.md','.json','.jsonl','.log','.xml','.png'}
for path in sorted(OUT.rglob('*')):
 if not path.is_file():continue
 rel=path.relative_to(OUT)
 if any(p in ['selected-evidence','__pycache__','memory-observation'] for p in rel.parts):continue
 if path.suffix not in allowed or path.name=='export-manifest.json':continue
 target=frozen/rel;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(path,target)
# The exact case metadata comes from the immutable case directories, not the run's
# mutable final-head0.ppm. Synthetic binary payloads and transport files are excluded.
for name in ['matrix.json','tray-matrix.json']:
 for case in json.loads((OUT/name).read_text())['attempts']:
  case_root=RUN/case['evidenceDirectory'];assert case_root.parent==RUN/'tests'
  target=frozen/case['evidenceDirectory'];target.mkdir(parents=True,exist_ok=True)
  for basename in ['results.json','results.xml','parameters.json','manifest.json','versions.json','package.json','diagnostics.json','paste.json','ready.json']:
   p=case_root/basename
   if p.is_file():shutil.copyfile(p,target/basename)
files={str(p.relative_to(frozen)):{'sha256':sha(p),'bytes':p.stat().st_size} for p in sorted(frozen.rglob('*')) if p.is_file()}
save(OUT/'export-manifest.json',{'schemaVersion':1,'timestampUtc':now(),'sourceCommit':source()['commit'],'scope':'Native evidence allowlist, no credentials/private keys/source mirror/DEB/synthetic payloads','files':files})
print(json.dumps({'files':len(files),'bytes':sum(x['bytes'] for x in files.values()),'manifest':str(OUT/'export-manifest.json'),'nativeReceiptSha256':sha(OUT/'receipt.json')}))
