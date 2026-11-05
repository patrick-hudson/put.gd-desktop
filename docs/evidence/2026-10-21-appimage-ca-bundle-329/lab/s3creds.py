#!/usr/bin/env python3
"""Writes one AWS key set from ~/Documents/creds.txt as JSON to stdout, for piping into
i329.py's stdin only. Set 1: the first labelled Access Key ID / Secret Access Key pair
(AWS section). Set 2: the unlabeled two-token line."""
import json, pathlib, re, sys
if sys.stdout.isatty(): raise SystemExit('refusing to write keys to a terminal')
which = sys.argv[1] if len(sys.argv) > 1 else '1'
lines = (pathlib.Path.home() / 'Documents/creds.txt').read_text().splitlines()
found = None
if which == '1':
    access = secret = None
    for line in lines:
        if 'backblaze' in line.lower() or 'cloudflare' in line.lower(): break
        m = re.match(r'^\s*(Access Key ID|Secret Access Key)\s*:\s*(\S+)\s*$', line)
        if m and m.group(1) == 'Access Key ID' and access is None: access = m.group(2)
        elif m and m.group(1) == 'Secret Access Key' and secret is None: secret = m.group(2)
    if access and secret: found = {'accessKey': access, 'secretKey': secret}
else:
    for line in lines:
        parts = line.split()
        if len(parts) == 2 and ':' not in line and re.fullmatch(r'[A-Z0-9]{20}', parts[0]) and len(parts[1]) == 40:
            found = {'accessKey': parts[0], 'secretKey': parts[1]}; break
if not found: raise SystemExit(f'key set {which} not found')
sys.stdout.write(json.dumps(found))
