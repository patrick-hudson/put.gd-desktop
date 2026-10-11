#!/usr/bin/env python3
from pathlib import Path
import importlib.util,json,subprocess,hashlib,shlex,datetime
root=Path('/home/patrick/work/put.gd')
s=importlib.util.spec_from_file_location('lab',root/'tools/lab/lab.py');lab=importlib.util.module_from_spec(s);s.loader.exec_module(lab)
commands={
 'gnome':['python3',str(root/'work/u11/native/gnome/sample-memory.py'),'--functional-wave-quiescent'],
 'cinnamon':lab.ssh_args(root/'work/lab-runs/current-cinnamon-a3d70dc')+['python3 /home/lab/putgd-lab/u11-memory-guard-v2.py /home/lab/putgd-lab/u11-memory-plan-v2.json'],
 'kde':json.loads((root/'work/u11/native/kde/memory-prepared.json').read_text())['sshCommand']}
children={}
for d,args in commands.items():
 p=root/'work/u11/native'/d/'quiet-memory-preflight'
 metadata={'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'command':args,'driverSha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'phase':'native functional wave and builds finished; root authorized quiet sampling','sourceCommit':'97ca3a521714e2717cedc1139976130627bfb8a8'}
 f=(p/'launcher.log').open('w');children[d]=(subprocess.Popen(args,stdout=f,stderr=subprocess.STDOUT,cwd=root),f)
 metadata['wrapperPid']=children[d][0].pid;(p/'launcher.json').write_text(json.dumps(metadata,indent=2)+'\n')
 print(d,'started wrapper',children[d][0].pid,flush=True)
for d,(p,f) in children.items():
 code=p.wait();f.close();(root/'work/u11/native'/d/'quiet-memory-preflight/launcher-exit.json').write_text(json.dumps({'exitCode':code,'utc':datetime.datetime.now(datetime.timezone.utc).isoformat()})+'\n');print(d,'exit',code,flush=True)
