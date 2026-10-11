#!/usr/bin/env python3
"""Existing frozen replay matrix, with all resolved fresh control sockets checked first."""
import argparse,importlib.util,sys
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--cinnamon-gates-sha256',required=True);a=p.parse_args()
s=importlib.util.spec_from_file_location('cross',Path('/home/patrick/work/put.gd/work/resident-native-boundary/cross-desktop-replay.py'));m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
a.output=str(m.DEFAULT_OUTPUT);a.guest='kde';a.phase='matrix'
r=m.CrossReplay(a)
paths=[r.run/'qmp.sock',Path('/run/user/1000/putgd/session.sock')]
paths += [m.ROOT/'work/lab-runs'/('ui43-'+r.name+'-'+r.attempt.name.split('-')[0]+'-'+str(i))/'qmp.sock' for i in range(1,13)]
r.command([sys.executable,str(m.ROOT/'tools/diagnostics/check_socket_paths.py'),'--socket',*[str(x) for x in paths]])
try:
 r.matrix();m.replay.save(r.attempt/'complete.json',{'utc':m.replay.now(),'phase':a.phase})
except Exception as e:
 m.replay.save(r.attempt/'failed.json',{'utc':m.replay.now(),'phase':a.phase,'errorType':type(e).__name__,'message':'Preserve attempt; diagnose explicitly before retry'});raise
