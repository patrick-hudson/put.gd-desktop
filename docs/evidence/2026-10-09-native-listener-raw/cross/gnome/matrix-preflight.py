#!/usr/bin/env python3
from pathlib import Path
from types import SimpleNamespace
import importlib.util,sys
ROOT=Path('/home/patrick/work/put.gd')
runner=ROOT/'work/resident-native-boundary/cross-desktop-replay.py'
spec=importlib.util.spec_from_file_location('cross',runner);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
a=SimpleNamespace(output=str(m.DEFAULT_OUTPUT),guest='gnome',phase='matrix',cinnamon_gates_sha256='5536705e64b29fae8b586f0c9db9365f1968d77261e10d94c8cac35b43bbabd1',seconds=300,kind='empty',view='settings',label='matrix',values=[],masked_focus_reviewed=False)
r=m.CrossReplay(a)
try:
 stamp=r.attempt.name.split('-')[0]
 paths=[ROOT/'work/lab-runs'/('ui43-gnome-'+stamp+'-'+str(i))/'qmp.sock' for i in range(1,13)]
 paths += [r.run/'qmp.sock',Path('/run/user/1000/putgd/session.sock')]
 # Prospective proxy aliases and their already-known target are both checked.
 result=r.command([sys.executable,str(ROOT/'tools/diagnostics/check_socket_paths.py'),'--socket',*[str(p) for p in paths]])
 m.replay.save(r.attempt/'socket-preflight.json',m.json.loads(result))
 r.matrix()
 m.replay.save(r.attempt/'complete.json',{'utc':m.replay.now(),'phase':'matrix','wrapperSha256':m.replay.sha(__file__)})
except Exception as error:
 m.replay.save(r.attempt/'failed.json',{'utc':m.replay.now(),'phase':'matrix','errorType':type(error).__name__,'message':'Preserve attempt; diagnose explicitly before retry'})
 raise
