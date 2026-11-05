#!/usr/bin/env python3
"""#213 transcript: drive213.py VARIANT OUT.txt — prepare, start, watch, stop, via putgd-cli."""
import json, subprocess, sys, time
v, out = sys.argv[1], sys.argv[2]
lines = []
t0 = time.monotonic()
def log(text):
    stamp = f'{time.monotonic() - t0:6.2f}s'
    lines.append(f'[{stamp}] {text}'); print(lines[-1], flush=True)
def cli(method, params):
    r = json.loads(subprocess.run(['bash', 'disk213.sh', 'cli', v, method, json.dumps(params)], capture_output=True, text=True).stdout)
    return r
def short(r, keys):
    if not r['ok']:
        return 'ERROR ' + json.dumps({'code': r['error']['code'], 'message': r['error']['message'], 'details': r['error']['details']})
    res = r['result']; return json.dumps({k: res.get(k) for k in keys})
disk = cli('settings.get', {})['result']['effectiveLimits']['disk']
log('settings.get effectiveLimits.disk ' + json.dumps({k: disk[k] for k in ('freeBytes', 'reserveBytes', 'availableBytes')}))
log('$ putgd-cli recording.prepare {"target":{"kind":"desktop"},"format":"mp4"}')
r = cli('recording.prepare', {'target': {'kind': 'desktop'}, 'format': 'mp4'})
log('  ' + short(r, ('recordingId', 'state')))
if r['ok']:
    rid = r['result']['recordingId']
    for _ in range(100):
        i = cli('recording.inspect', {'id': rid})
        if i['ok'] and i['result']['state'] == 'ready': break
        time.sleep(0.2)
    log('  inspect: state ' + i['result']['state'] + ', config.budgetBytes ' + str(i['result']['config'].get('budgetBytes')))
    log('$ putgd-cli recording.start {"id":...}')
    r = cli('recording.start', {'id': rid}); log('  ' + short(r, ('state',)))
    started = time.monotonic(); last = None
    while time.monotonic() - started < 20:
        i = cli('recording.inspect', {'id': rid})
        if not i['ok']: log('  inspect ' + i['error']['code']); break
        state = i['result']['state']; prog = i['result'].get('progress') or {}
        if state != last: log(f'  inspect: state {state}, bytes {prog.get("bytes")}, durationUs {i["result"].get("durationUs")}' + (f', stopReason {i["result"]["stopReason"]}' if 'stopReason' in i['result'] else '')); last = state
        if state == 'reviewing': break
        time.sleep(0.25)
    log(f'  recording ended itself {time.monotonic() - started:.1f} s after start' if last == 'reviewing' else '  still recording after 20 s')
    log('$ putgd-cli recording.stop {"id":...}   # the caller still thinks it is recording')
    r = cli('recording.stop', {'id': rid}); log('  ' + short(r, ('state', 'stopReason', 'durationUs')))
    cli('recording.cancel', {'id': rid})
open(out, 'w').write('\n'.join(lines) + '\n')
