"""Grep every written byte under the given roots for planted values (raw, URL-encoded, base64, hex)."""
import base64, json, sys
from pathlib import Path
from urllib.parse import quote
# The planted values and their distinctive parts (tokens, signature, credential,
# keys, scheme-less links) come from the same private JSON file as live.py.
import os as _os
VALUES = json.load(open(_os.environ['SD726_LEAK_VALUES']))
extra = json.loads(sys.argv[1]) if len(sys.argv) > 2 and sys.argv[1].startswith('{') else {}
roots = [Path(p) for p in sys.argv[1:] if not p.startswith('{')]
VALUES.update(extra)
forms = {}
for name, value in VALUES.items():
    data = value.encode()
    for label, form in (('raw', data), ('quote', quote(value, safe='').encode()), ('hex', data.hex().encode())):
        forms[(name, label)] = form
    for n in range(3):
        encoded = base64.b64encode(b'\0'*n+data).decode()
        forms[(name, 'b64-%d' % n)] = encoded[(n*8+5)//6:((n+len(data))*8)//6].encode()
        encoded = base64.urlsafe_b64encode(b'\0'*n+data).decode()
        forms[(name, 'b64url-%d' % n)] = encoded[(n*8+5)//6:((n+len(data))*8)//6].encode()
files = [f for root in roots for f in root.rglob('*') if f.is_file()]
hits = [(str(f), name, label) for f in files for (name, label), form in forms.items() if form in f.read_bytes()]
print(json.dumps({'files': len(files), 'bytes': sum(f.stat().st_size for f in files), 'forms': len(forms), 'hits': hits}, indent=1))
