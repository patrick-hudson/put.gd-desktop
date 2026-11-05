"""ubuntu_cves.py OUT SOURCE... : CVEs whose noble status is still open (needed/deferred/pending), from Ubuntu's CVE API."""
import json, sys, time, urllib.parse, urllib.request
from concurrent.futures import ThreadPoolExecutor
OPEN = ('needed', 'deferred', 'pending')
def fetch(src, status):
    found, offset = [], 0
    while True:
        url = 'https://ubuntu.com/security/cves.json?' + urllib.parse.urlencode(
            [('package', src), ('version', 'noble'), ('status', status), ('limit', '20'), ('offset', str(offset))])
        for attempt in range(6):
            try:
                d = json.load(urllib.request.urlopen(url, timeout=120)); break
            except Exception as e:
                err = e; time.sleep(10 * (attempt + 1))
        else:
            return src, status, {'error': str(err)}
        for c in d['cves']:
            for p in c['packages']:
                if p['name'] != src: continue
                for s in p['statuses']:
                    if s['release_codename'] == 'noble' and s['status'] in OPEN:
                        found.append({'id': c['id'], 'priority': c.get('priority'), 'published': (c.get('published') or '')[:10],
                                      'status': s['status'], 'note': s.get('description') or ''})
        offset += 20
        if offset >= d['total_results']: return src, status, found
out = {}
with ThreadPoolExecutor(4) as pool:
    for src, status, res in pool.map(lambda a: fetch(*a), [(s, st) for s in sys.argv[2:] for st in OPEN]):
        out.setdefault(src, {})[status] = res
        with open(sys.argv[1], 'w') as f: json.dump(out, f, indent=1, sort_keys=True)
