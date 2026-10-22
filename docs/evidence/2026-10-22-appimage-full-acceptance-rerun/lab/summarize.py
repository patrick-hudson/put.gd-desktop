#!/usr/bin/env python3
"""Print the key numbers of each desktop's results for the evidence README."""
import json, sys
from pathlib import Path

root = Path(sys.argv[1])
for desk in sys.argv[2:]:
    r = root / desk / 'results'
    res = json.loads((r / 'results.json').read_text())
    ident = json.loads(next(r.glob('*-identity.json')).read_text())
    ver = json.loads(next(r.glob('*-versions.json')).read_text())
    mem = res.get('memory', {})
    rec = json.loads((r / 'out/record.json').read_text()) if (r / 'out/record.json').exists() else res.get('record', {})
    ctl = res.get('control', {})
    print(f'== {desk}')
    print('A', ident['hashMatches'], ident['tagMatches'], ident['updateInformationMatches'], ident['rootDesktopFiles'], ident['noDisplay'])
    print('  os', ver['os'], '| session', [(s['Type'], s['Service']) for s in ver['graphicalSessions']], '|',
          {k: ver.get(k) for k in ('plasmashell', 'kwin_wayland', 'gnome-shell', 'cinnamon', 'muffin') if ver.get(k)}, ver['kernel'], ver['gpu'])
    st = res.get('start', {})
    print('B start', st.get('startSeconds'), st.get('sessionSock'), st.get('kdeEntries'), [(p['comm'], p['qtPlatformPlugins']) for p in st.get('processes', [])])
    for k, v in res.items():
        if k.startswith('window:'):
            print('  ', k, [(p['comm'], p['qtPlatformPlugins']) for p in v['processes']])
    print('C', mem.get('finalRssMiB'), mem.get('pssMiB'), mem.get('flaggedLibraries'), mem.get('load'))
    for kind in ('desktop', 'window', 'region'):
        c = res.get('capture:' + kind, {})
        print('D', kind, c.get('operation', {}).get('state'), (c.get('png') or {}).get('size'), c.get('linkCopied'), (c.get('historyEntry') or {}).get('local'))
    e = res.get('edit', {})
    print('D edit', e.get('redactionVerified'), (e.get('dialogResult') or {}).get('last'), e.get('linkCopied'))
    if rec:
        c = rec.get('capabilities', {})
        print('E', c.get('acquisition'), rec.get('liveEncoder'), rec.get('paused'), rec.get('stopped', {}).get('durationUs'), rec.get('edit', {}).get('trimStart'), rec.get('edit', {}).get('trimEnd'), len(rec.get('edit', {}).get('masks', [])))
        print('  exports', {f: (x['status'], (x.get('motion') or {}).get('motion')) for f, x in rec.get('exports', {}).items()})
        print('  dialog', (rec.get('saveCopyDialog', {}).get('inspect') or {}).get('last', {}).get('backend'), rec.get('saveCopyDialog', {}).get('exists'))
        print('  control', {f: (x.get('motion') or {}).get('motion') for f, x in ctl.items()}, 'entries', rec.get('entriesDuring'))
    for k, v in res.items():
        if k.startswith('notify:'):
            n = v['notification']; print('F', k, bool(n.get('logged')), n.get('actionInvoked'), n.get('openedUrls'))
    h = res.get('history', {})
    print('G', h.get('total'), (h.get('copyImage') or {}).get('samePixels'), res.get('delete', {}).get('remoteAfter'), res.get('delete', {}).get('localExistsAfter'))
    s = res.get('settings', {})
    print('H', {t: v['appearance']['scheme'] for t, v in s.get('schemes', {}).items()}, s.get('updateStatus', {}).get('channel'), (s.get('checkNow') or {}).get('ok'), ((s.get('checkNow') or {}).get('result') or {}).get('latest'), s.get('notificationsAfterCheck'))
    li = res.get('loginCheck', {})
    print('I', li.get('fromAppImage'), li.get('shellStarted'), [(p['comm'], p['started']) for p in li.get('processes', [])])
    j = res.get('updateCheck', {})
    print('J', j.get('exit'), j.get('zsyncPermalink'))
    q = res.get('quit', {})
    print('K', q.get('allGone'), q.get('after'), res.get('cleanup', {}).get('after'))
