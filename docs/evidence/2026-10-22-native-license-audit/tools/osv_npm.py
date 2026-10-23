"""osv_npm.py PACKAGE_LOCK OUT: query OSV for every package version in the
Proton helper's npm lock (a second source besides `npm audit`). Run with python3 -I."""
import json, sys, urllib.request

lock = json.load(open(sys.argv[1]))
packages = []
for path, entry in lock['packages'].items():
    if not path or 'version' not in entry:
        continue
    name = entry.get('name') or path.split('node_modules/')[-1]
    packages.append({'name': name, 'version': entry['version'], 'dev': entry.get('dev', False),
                     'optional': entry.get('optional', False), 'resolved': entry.get('resolved', '')})
query = {'queries': [{'package': {'ecosystem': 'npm', 'name': p['name']}, 'version': p['version']} for p in packages]}
request = urllib.request.Request('https://api.osv.dev/v1/querybatch', json.dumps(query).encode(), {'Content-Type': 'application/json'})
results = json.load(urllib.request.urlopen(request, timeout=60))['results']
for p, r in zip(packages, results):
    p['osv'] = [v['id'] for v in r.get('vulns', [])]
json.dump(packages, open(sys.argv[2], 'w'), indent=1)
for p in packages:
    print(f"{p['name']}@{p['version']}{' dev' if p['dev'] else ''}{' optional' if p['optional'] else ''}: {p['osv'] or 'none'}")
