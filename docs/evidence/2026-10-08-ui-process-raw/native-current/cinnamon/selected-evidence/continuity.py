#!/usr/bin/env python3
"""One bounded real local-file operation across a normal visible UI close.

Uses the independent native fixture's actual clipboard file ownership. No S3,
injected delay, fake provider, memory sample, or product instrumentation is used.
A fast operation is recorded as inconclusive, not silently retried.
"""
import json
from pathlib import Path
import shlex
import sys
import time

import native as n

SIZE = 512 * 1024 * 1024
OUT = n.OUT / 'continuity-run1'


def main():
    OUT.mkdir(mode=0o700, exist_ok=False)
    settings = n.lab.cli(n.RUN, 'settings.get')['document']
    limits = settings['limits']
    assert settings['actions']['clipboard']['destination'] == 'local'
    assert settings['archive']['root'] == '/home/lab/putgd-lab/archive'
    assert min(limits['maxFileBytes'], limits['maxBatchBytes'], limits['pendingArtifactBytes']) > SIZE + 100
    resident = n.lab.verify_running_app(n.RUN)
    assert n.helper_pid(n.snapshot()) == 0
    n.lab.guest_session(n.RUN, '''set -eu
pkill -x putgd-desktop-f || true
pkill -f '^python3 /home/[^/]+/putgd-lab/gtk_clipboard_fixture.py --output ' || true
rm -f "$HOME/putgd-lab/fixture/ready.json"
nohup putgd-desktop-fixture --output "$HOME/putgd-lab/fixture" > "$HOME/putgd-lab/fixture.log" 2>&1 < /dev/null &
''')
    n.wait_for(lambda: [w for w in n.windows() if w['title'] == 'put.gd independent synthetic paint / paste fixture'], 'native clipboard fixture')
    def fixture_ready():
        return n.lab.guest_session(n.RUN, 'if test -s "$HOME/putgd-lab/fixture/ready.json"; then echo ready; fi\n').strip() == 'ready'
    n.wait_for(fixture_ready, 'independent fixture ready.json after its one-second initialization')
    preparation = '''python3 - <<'PY'
import hashlib,json,pathlib,shutil
root=pathlib.Path.home()/'putgd-lab/fixture'
assert (root/'ready.json').is_file()
size=512*1024*1024
free=shutil.disk_usage(root).free
assert free>3*size+512*1024*1024
path=root/'synthetic-1.txt'
original=path.read_bytes();assert original==b'putgd synthetic file 1\\n'
block=(b'putgd U11 synthetic continuity\\n'*40000)[:1024*1024]
assert len(block)==1024*1024
digest=hashlib.sha256()
with path.open('wb') as stream:
 for index in range(size//len(block)):
  stream.write(block);digest.update(block)
print(json.dumps({'path':str(path),'size':size,'sha256':digest.hexdigest(),'freeBeforeBytes':free,
                  'streamBlockBytes':len(block),'blockPattern':'putgd U11 synthetic continuity newline'}))
PY
'''
    (OUT / 'prepare.sh').write_text(preparation)
    payload = json.loads(n.lab.guest_session(n.RUN, preparation))
    n.lab.guest_session(n.RUN, "wmctrl -a 'put.gd independent synthetic paint / paste fixture'\n")
    n.lab.key(n.RUN, ['ctrl', 'alt', 'shift', 'f'])
    time.sleep(.2)
    n.lab.cli(n.RUN, 'ui.show', {'view': 'activity'})
    ui = n.snapshot();pid = n.helper_pid(ui);assert pid > 0
    window = n.wait_for(lambda: next((w for w in n.windows() if w['pid'] == pid), None), 'Activity helper')
    n.lab.guest_session(n.RUN, 'wmctrl -ia ' + shlex.quote(window['id']) + '\n')
    before = {o['id'] for o in n.lab.cli(n.RUN, 'operation.recent')['operations']}
    parameters = {'schemaVersion': 1, 'timestampUtc': n.now(), 'payload': payload,
                  'limits': limits, 'archive': settings['archive'], 'resident': resident,
                  'helperPid': pid, 'operationIdsBefore': sorted(before),
                  'scope': 'Real synthetic local-file copy only, not slow S3 or memory acceptance.'}
    n.lab.write_json(OUT / 'parameters.json', parameters)
    observer = '''import json,os,pathlib,subprocess,time
root=pathlib.Path.home()/'putgd-lab/u11-continuity'
p=json.loads((root/'parameters.json').read_text());before=set(p['operationIdsBefore']);pid=p['helperPid']
started=time.monotonic();observations=[];last=None;failure=None
def cli(method,params=None):
 r=subprocess.run(['putgd-cli',method,json.dumps(params or {})],capture_output=True,text=True,timeout=5)
 d=json.loads(r.stdout)
 if r.returncode or not d.get('ok'):raise RuntimeError('Observer CLI rejected '+method)
 return d['result']
try:
 while time.monotonic()-started<60:
  goneBefore=not pathlib.Path('/proc',str(pid)).exists()
  ops=[o for o in cli('operation.recent')['operations'] if o['id'] not in before]
  if len(ops)>1:raise RuntimeError('Unexpected extra native operation')
  if ops:
   last=cli('operation.inspect',{'id':ops[0]['id']})
   row={'monotonicSeconds':time.monotonic(),'helperGoneBeforeInspect':goneBefore,
        'operationId':last['id'],'state':last['state'],'copyStatus':last['copyStatus']}
   observations.append(row)
   with (root/'observations.jsonl').open('a') as stream:stream.write(json.dumps(row)+'\\n')
   if goneBefore and last['state'] in ('completed','failed','partial','cancelled') and last['copyStatus']!='pending':break
  time.sleep(.03)
except Exception as e:failure=str(e)
finally:
 activeAfter=any(o['helperGoneBeforeInspect'] and o['state'] not in ('completed','failed','partial','cancelled','interrupted') for o in observations)
 (root/'result.json').write_text(json.dumps({'observations':observations,'operation':last,'failure':failure,
  'activeOperationObservedAfterHelperExit':activeAfter,'elapsedSeconds':time.monotonic()-started},indent=2)+'\\n')
'''
    (OUT / 'observer.py').write_text(observer)
    n.lab.guest_session(n.RUN, 'mkdir -p "$HOME/putgd-lab/u11-continuity"\n')
    n.lab.put(n.RUN, str(OUT / 'observer.py'), '/home/lab/putgd-lab/u11-continuity/observer.py')
    n.lab.put(n.RUN, str(OUT / 'parameters.json'), '/home/lab/putgd-lab/u11-continuity/parameters.json')
    n.lab.guest_session(n.RUN, 'nohup python3 "$HOME/putgd-lab/u11-continuity/observer.py" > "$HOME/putgd-lab/u11-continuity/observer.log" 2>&1 < /dev/null &\n')
    n.witness('continuity-before-trigger')
    n.event('continuity-input', keys=['ctrl', 'shift', '4'], thenKeys=['alt', 'f4'], helperPid=pid)
    n.lab.key(n.RUN, ['ctrl', 'shift', '4'])
    n.lab.key(n.RUN, ['alt', 'f4'])
    after = n.wait_for(lambda: d if n.helper_pid(d := n.snapshot()) == 0 else None, 'helper normal exit')
    assert after['diagnostics']['ui']['lastExit']['status'] == 'normal'
    assert after['diagnostics']['ui']['lastExit']['code'] == 0
    n.witness('continuity-after-close')
    deadline = time.monotonic() + 65
    while True:
        try:
            n.lab.get(n.RUN, 'putgd-lab/u11-continuity/result.json', OUT / 'observer-result.json')
            break
        except n.lab.LabError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(.2)
    result = json.loads((OUT / 'observer-result.json').read_text())
    op = result['operation'];assert op and result['failure'] is None
    assert op['state'] == 'completed' and all(i['local']['status'] == 'saved' for i in op['items'])
    paths = [i['local']['path'] for i in op['items']]
    assert len(paths) == 2 and all(p.startswith('/home/lab/putgd-lab/archive/') for p in paths)
    hashes = n.lab.ssh(n.RUN, 'sha256sum ' + ' '.join(shlex.quote(p) for p in paths) + '\n')
    sizes = n.lab.ssh(n.RUN, "stat -c '%s' " + ' '.join(shlex.quote(p) for p in paths) + '\n')
    assert hashes.splitlines()[0].split()[0] == payload['sha256']
    assert int(sizes.splitlines()[0]) == SIZE
    assert n.lab.verify_running_app(n.RUN) == resident
    result.update(timestampUtc=n.now(), normalExit=after['diagnostics']['ui']['lastExit'],
                  residentUnchanged=resident, artifactSha256Output=hashes, artifactSizes=sizes,
                  verdict='pass' if result['activeOperationObservedAfterHelperExit'] else 'inconclusive-fast-operation')
    n.lab.write_json(OUT / 'receipt.json', result)
    print(json.dumps({'verdict': result['verdict'], 'operationId': op['id'], 'bytes': SIZE}))


if __name__ == '__main__':
    main()
