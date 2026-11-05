#!/usr/bin/env python3
"""lab_verify_empty.py SRC CONFIG KEYLIST: after `lab_publish.py ... clean`, the bucket's lab/ prefix lists 0 objects
(the signer's own S3 client), and every key the pass published answers 404 over pkg.put.gd. Read-only."""
import json, sys, urllib.error, urllib.request
from pathlib import Path
SRC, CONFIG, KEYS = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
sys.path.insert(0, str(SRC / 'tools/releases'))
import pkgrepo  # noqa: E402
config = pkgrepo.Config.load(CONFIG)
s3, cloudfront = pkgrepo.Signer(config).clients()
listed = s3.list_objects_v2(Bucket=config.bucket, Prefix='lab/').get('KeyCount', 0)
codes = {}
for key in [k.strip() for k in KEYS.read_text().splitlines() if k.strip().startswith('lab/')]:
    try:
        with urllib.request.urlopen(urllib.request.Request(f'{config.base_url}/{key}', method='HEAD'), timeout=30) as r: codes[key] = r.status
    except urllib.error.HTTPError as e: codes[key] = e.code
print(json.dumps({'labPrefixObjects': listed, 'keysChecked': len(codes), 'not404': {k: v for k, v in codes.items() if v != 404}}))
