#!/usr/bin/env python3
"""Bounded cost-location probe. Requires successful matched physical gate."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time

HERE=Path(__file__).resolve().parent
COMMIT='97ca3a521714e2717cedc1139976130627bfb8a8'
def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()
def write(path,value):
    path.write_text(json.dumps(value,indent=2)+'\n')

parser=argparse.ArgumentParser()
parser.add_argument('--gate',required=True,help='Completed root matched-execution.json; requires all four exit codes zero')
args=parser.parse_args()
gate_path=Path(args.gate).resolve(strict=True)
gate=json.loads(gate_path.read_text())
assert gate['status']=='complete'
assert [r['label'] for r in gate['runs']]==['retained-1','split-1','retained-2','split-2']
assert all(r['exitCode']==0 for r in gate['runs'])
source=json.loads((HERE/'../../native/source.json').resolve().read_text())
assert source['commit']==COMMIT, source
assert 'cinnamon' in os.environ.get('XDG_CURRENT_DESKTOP','').lower()
assert os.environ.get('XDG_SESSION_TYPE')=='x11'
assert os.environ.get('DISPLAY')
assert os.environ.get('QT_QPA_PLATFORM','') in ('','xcb')
output=HERE/'results'
output.mkdir(mode=0o700) # No overwrite, no retry-to-green.
receipt={'schemaVersion':1,'purpose':'cost-location-only-not-product-savings','sourceCommit':COMMIT,'gateSha256':digest(gate_path),'startedUtc':datetime.now(timezone.utc).isoformat(),'variantOrder':['full','native','full','native'],'stageDurationMs':5000,'timeoutSeconds':15,'newDependencies':False,'nativeFontsAndDialogsUnchanged':True,'status':'building','runs':[],'ownedInputHashes':{p.name:digest(p) for p in [HERE/'CMakeLists.txt',HERE/'CMakePresets.json',HERE/'probe.cpp',HERE/'run.py']}}
receipt_path=output/'receipt.json'
write(receipt_path,receipt)
for label,argv in [('configure',['cmake','--preset','dev']),('build',['cmake','--build','--preset','dev'])]:
    with (output/(label+'.stdout')).open('x') as stdout,(output/(label+'.stderr')).open('x') as stderr:
        result=subprocess.run(argv,cwd=HERE,stdout=stdout,stderr=stderr)
    receipt[label]={'argv':argv,'exitCode':result.returncode,'stdoutSha256':digest(output/(label+'.stdout')),'stderrSha256':digest(output/(label+'.stderr'))}
    write(receipt_path,receipt)
    if result.returncode:
        receipt['status']='failed-preserved-no-automatic-retry';write(receipt_path,receipt);sys.exit(result.returncode)
binary=HERE/'build/style-cost-probe'
receipt['sourceManifestSha256']=digest((HERE/'../../native/source.json').resolve())
receipt['sourceArchiveSha256']=source['archiveSha256']
receipt['visualStyleHashes']={name:digest((HERE/'../../native/host/source/src/ui'/name).resolve()) for name in ['VisualStyle.cpp','VisualStyle.h']}
receipt['binarySha256']=digest(binary)
receipt['status']='sampling';write(receipt_path,receipt)
for index,variant in enumerate(receipt['variantOrder'],1):
    label=f'{index}-{variant}'
    stdout_path=output/(label+'.jsonl');stderr_path=output/(label+'.stderr')
    started=time.monotonic()
    try:
        with stdout_path.open('x') as stdout,stderr_path.open('x') as stderr:
            result=subprocess.run([str(binary),variant],cwd=HERE,stdout=stdout,stderr=stderr,timeout=15)
        exit_code=result.returncode
    except subprocess.TimeoutExpired:
        exit_code=124
    records=[json.loads(line) for line in stdout_path.read_text().splitlines()]
    row={'label':label,'argv':[str(binary),variant],'elapsedSeconds':time.monotonic()-started,'exitCode':exit_code,'stdoutSha256':digest(stdout_path),'stderrSha256':digest(stderr_path),'stages':records}
    receipt['runs'].append(row);write(receipt_path,receipt)
    if exit_code or [r['stage'] for r in records]!=['qapplication-constructed','style-applied-or-native','brand-and-tray-created','two-second-idle-cleanup','quiet-settled']:
        receipt['status']='failed-preserved-no-automatic-retry';write(receipt_path,receipt);sys.exit(exit_code or 5)
    deps=records[-1]['loadedSharedObjects']
    row['dependencyHashes']={p:digest(Path(p)) for p in deps}
    write(receipt_path,receipt)
receipt['status']='complete';receipt['completedUtc']=datetime.now(timezone.utc).isoformat()
write(receipt_path,receipt)
print(json.dumps({'status':receipt['status'],'receipt':str(receipt_path),'settled':[{'label':r['label'],'memory':r['stages'][-1]['memory']} for r in receipt['runs']]}))
