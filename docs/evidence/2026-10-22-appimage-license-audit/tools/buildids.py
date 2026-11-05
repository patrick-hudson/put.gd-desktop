"""buildids.py ROOT -> JSON {relpath: build-id} for every ELF under ROOT (no symlinks)."""
import json, os, subprocess, sys
root = sys.argv[1]; out = {}
for d, _, files in os.walk(root):
    for f in files:
        p = os.path.join(d, f)
        if os.path.islink(p) or not os.path.isfile(p):
            continue
        with open(p, 'rb') as s:
            if s.read(4) != b'\x7fELF':
                continue
        r = subprocess.run(['readelf', '-n', p], capture_output=True, text=True).stdout
        bid = next((l.split(':', 1)[1].strip() for l in r.splitlines() if 'Build ID' in l), None)
        out[os.path.relpath(p, root)] = bid
json.dump(out, sys.stdout, indent=0, sort_keys=True)
