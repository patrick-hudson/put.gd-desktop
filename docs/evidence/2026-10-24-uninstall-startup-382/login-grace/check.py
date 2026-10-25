import sys,json,subprocess,time
from pathlib import Path
sys.path.insert(0,'work/i382f');import control
sys.path.insert(0,'tools/lab');from rfb import RFB
out=Path('work/i382g')
def call(*args):
 r=control.run('f44',['python3','/home/patrick/work/p382g/i382.py','native',*args]);(out/('-'.join(args)+'.json')).write_text(json.dumps(r)+'\n');assert not r['exit'],r;print(args,'passed',flush=True);return json.loads(r['stdout'])
call('retention','before','inactive');call('logout','inactive');call('assert-inactive');call('relogin','inactive');d=call('check','absent','inactive');call('retention','after','inactive')
lines=[l for l in d['journal']['stdout'].splitlines() if ('app-gnome-putgd' in l or 'put.gd login start:' in l)];errors=[l for l in lines if any(x in l.lower() for x in ['failed','refusing','cannot','not executable'])]
s=call('status','final');assert not s['personalApps'];v=RFB(5970);v.screenshot(str(out/'cleaned-login.png'));v.close()
(out/'result.json').write_text(json.dumps({'scope':'Real Fedora GNOME login with current script template, missing native executable and no resident; not package acceptance','templateSha256':json.loads(json.loads((out/'render-result.json').read_text())['stdout'])['templateSha256'],'sessionId':d['sessionId'],'entryAbsent':d['entry'] is None,'scriptAbsent':d['scriptSha256'] is None,'savedPreference':d['preference'],'startupJournalLines':lines,'startupJournalErrors':errors,'pass':d['pass'] and not errors},indent=2)+'\n')
call('cleanup');r=subprocess.run(['python3','tools/lab/desktop_lock.py','release','f44','--session','codex-i382-login-grace','--verified-clean'],capture_output=True,text=True);(out/'release-lock.txt').write_text(r.stdout+r.stderr);assert not r.returncode,r.stdout+r.stderr
assert not errors,errors
print('Real GNOME cleanup journal regression passed')
