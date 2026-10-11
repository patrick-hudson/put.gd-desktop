from pathlib import Path
import sys,json,subprocess,datetime
out=Path(__file__).resolve().parent;root=out.parents[3];sys.path.insert(0,str(root/'tools/lab'));import lab
run=root/'work/lab-runs/current-gnome-a3d70dc';expected=json.loads((out/'baseline-resident.json').read_text())['identity'];assert lab.verify_running_app(run)==expected
ops=lab.cli(run,'operation.recent')['operations'];assert all(x['state'] in ['completed','failed','cancelled','partial','interrupted'] for x in ops)
(out/'stop-old-terminal-operations.json').write_text(json.dumps({'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'operations':[{'id':x['id'],'state':x['state']} for x in ops]},indent=2)+'\n')
s=(out/'install.py').read_text();start=s.index("stop=r'''")+len("stop=r'''");end=s.index("'''.replace('EXPECTED'",start);code=s[start:end].replace('EXPECTED',repr(expected)).replace('TARGET',repr('/home/lab/putgd-u11-97ca3a521714/previous-lab-data'))
p=subprocess.run(lab.ssh_args(run)+['python3','-'],input=code,text=True,capture_output=True,timeout=30)
(out/'preserve-state.json').write_text(json.dumps({'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'exitCode':p.returncode,'stdout':p.stdout,'stderr':p.stderr,'userRequestedImmediateOldResidentStop':True},indent=2)+'\n');p.check_returncode();print(p.stdout)
