#!/usr/bin/env python3
"""Writes the creds.txt R2 S3 key pair as JSON to stdout, for piping into i329.py only."""
import json, pathlib, re, sys
if sys.stdout.isatty(): raise SystemExit('refusing to write keys to a terminal')
out, section = {}, None
for line in (pathlib.Path.home() / 'Documents/creds.txt').read_text().splitlines():
    if 'cloudflare' in line.lower(): section = 'r2'
    elif 'backblaze' in line.lower(): section = None
    m = re.match(r'^\s*([^:]+?)\s*:\s*(.*?)\s*$', line)
    if section == 'r2' and m:
        if m.group(1).lower() == 'access key id': out['accessKey'] = m.group(2)
        elif m.group(1).lower() == 'secret access key': out['secretKey'] = m.group(2)
if len(out) != 2: raise SystemExit('R2 keys not found')
sys.stdout.write(json.dumps(out))
