#!/usr/bin/env python3
"""Point local copies at the loop filesystem and turn them on: setup213.py VARIANT"""
import json, subprocess, sys
v = sys.argv[1]
def cli(method, params=None):
    args = ['bash', 'disk213.sh', 'cli', v, method] + ([json.dumps(params)] if params is not None else [])
    return json.loads(subprocess.run(args, capture_output=True, text=True).stdout)
got = cli('settings.get')['result']
rev = got['metadata']['observedRevision']
root = f'/tmp/r9s-{v}/data/archive'
subprocess.run(['mkdir', '-p', root])
r = cli('settings.patch', {'patch': {'archive': {'keepUploads': True, 'root': root}}, 'expectedRevision': rev})
print(json.dumps({'ok': r['ok'], 'error': r['error']['code'], 'archive': cli('settings.get')['result']['document']['archive']}))
