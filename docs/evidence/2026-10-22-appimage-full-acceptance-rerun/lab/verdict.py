#!/usr/bin/env python3
"""verdict.py RUNDIR : one line per item from the run's results.json and step files."""
import json, sys
from pathlib import Path
d = Path(sys.argv[1]); R = json.loads((d / 'results.json').read_text())
def j(n):
    p = d / f'{n}.json'
    try: return json.loads(p.read_text())
    except Exception as e: return {'_missing': str(e)[:80]}
g = lambda o, *ks: [o := (o or {}).get(k) if isinstance(o, dict) else None for k in ks][-1]
out = {}
ft = R.get('findTray', {}); out['B.tray'] = {'found': list((ft.get('found') or {}).keys()), 'menu': [e['label'] for e in (R.get('trayMenu') or {}).get('menu', [])][:12]}
w = next((v for k, v in R.items() if k.startswith('window:')), {}); out['B.window'] = {'procs': [(p['comm'], p.get('platform')) for p in w.get('processes', [])], 'switcher': bool(w.get('switcher'))}
for k in ('region', 'window', 'desktop'):
    c = R.get('capture:' + k, {}); out['D.' + k] = {'state': g(c, 'operation', 'state'), 'size': g(c, 'png', 'size'), 'cover': g(c, 'coverCheck', 'ok'),
                                                  'history': bool(c.get('historyEntry')), 'linkCopied': c.get('linkCopied'), 'serves': g(c, 'linkServes', 'matchesLocal')}
for k, v in R.items():
    if k.startswith('regionSize:'): out['D.' + k] = {x: v.get(x) for x in ('expected', 'png', 'sizePass', 'origin', 'originOffset', 'consistent', 'screenCovered', 'originExact')}
e = R.get('edit', {}); out['D.edit'] = {'redaction': e.get('redactionVerified'), 'dialog': g(e, 'dialogState', 'backend') or g(e, 'dialogState', 'open'),
                                       'saved': g(e, 'savedCopy', 'exists'), 'savedPath': g(e, 'dialogResult', 'last', 'path'), 'upload': g(e, 'upload', 'state'), 'linkCopied': e.get('linkCopied')}
r = R.get('record', {}); ex = r.get('exports') or {}
out['E.record'] = {'acq': g(r, 'capabilities', 'acquisition'), 'enc': r.get('liveEncoder'), 'prepare': g(r, 'prepare', 'state'),
                   'motion': {k: (g(v, 'motion', 'motion'), v.get('status')) for k, v in ex.items()},
                   'saveCopy': (g(r, 'saveCopyDialog', 'final', 'last', 'status') or g(r, 'saveCopyDialog', 'result') or str(g(r, 'saveCopyDialog', 'open', 'result', 'fileDialog', 'backend'))),
                   'upload': g(r, 'upload', 'state') or str(r.get('upload'))[:80], 'kwinEntry': r.get('entriesDuring'), 'edit': {k: str(g(r, 'edit', k))[:60] for k in ('trimStart', 'trimEnd')} if r.get('edit') else str(r.get('review'))[:120]}
out['E.control'] = {k: (v or {}).get('motion') for k, v in (R.get('control') or {}).items()}
n = next((v for k, v in R.items() if k.startswith('notify:')), {}); nn = n.get('notification', {})
out['F'] = {'shown': bool(nn.get('logged')), 'actionInvoked': nn.get('actionInvoked'), 'opened': nn.get('openedUrls'), 'handlerOff': g(R, 'handlerOff', 'same'), 'filesMatch': g(R, 'handlerOff', 'filesMatch')}
h = R.get('history', {}); out['G.history'] = {'total': h.get('total'), 'previews': {k: v.get('state') for k, v in (h.get('previews') or {}).items()}, 'copyImage': g(h, 'copyImage', 'samePixels')}
dl = R.get('delete', {}); out['G.delete'] = {k: dl.get(k) for k in ('remoteAfter', 'remoteStatus', 'localExistsAfter', 'historyRowKept')}
s = R.get('settings', {}); out['H.settings'] = {'schemes': {k: g(v, 'appearance', 'scheme') for k, v in (s.get('schemes') or {}).items()}, 'checkNow': s.get('checkNow'),
                                               'status': {k: g(s, 'updateStatus', k) for k in ('channel', 'error')}, 'notifsAfterCheck': len(s.get('notificationsAfterCheck') or [])}
for k, v in R.items():
    if k.startswith('statusClean'): out['H.' + k] = {'pass': v.get('pass'), 'control': v.get('statusControl')}
    if k.startswith('channel:'): out['H.' + k] = {'ok': g(v, 'check', 'ok'), 'err': g(v, 'check', 'error'), 'channel': g(v, 'status', 'channel'), 'reason': g(v, 'status', 'install', 'reason'),
                                                   'result': str(g(v, 'status', 'result'))[:160], 'notifs': len(v.get('notifications') or [])}
    if k.startswith('updatesFirstOpen'): out['H.348'] = {'calls': v.get('calls'), 'show': v.get('show')}
print(json.dumps(out, indent=1, default=str))
