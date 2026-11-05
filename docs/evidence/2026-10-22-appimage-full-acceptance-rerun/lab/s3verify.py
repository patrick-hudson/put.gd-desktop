#!/usr/bin/env python3
"""s3verify.py PREFIX [--purge]: counts (and with --purge removes) everything under put.gd/PREFIX with key set 2.
Keys are read from ~/Documents/creds.txt into memory only. Runs on d69 (boto3)."""
import json, pathlib, re, sys, time
import boto3
for line in (pathlib.Path.home() / 'Documents/creds.txt').read_text().splitlines():
    p = line.split()
    if len(p) == 2 and ':' not in line and re.fullmatch(r'[A-Z0-9]{20}', p[0]) and len(p[1]) == 40: ak, sk = p; break
c = boto3.client('s3', region_name='us-east-2', aws_access_key_id=ak, aws_secret_access_key=sk)
del ak, sk
B, P = 'put.gd', sys.argv[1]
if not P.startswith('acceptance/afa2/'): raise SystemExit('only acceptance/afa2/ prefixes')
v = c.list_object_versions(Bucket=B, Prefix=P)
versions = v.get('Versions', []) + v.get('DeleteMarkers', [])
ups = c.list_multipart_uploads(Bucket=B, Prefix=P).get('Uploads', [])
if '--purge' in sys.argv:
    for x in versions: c.delete_object(Bucket=B, Key=x['Key'], VersionId=x['VersionId'])
    for u in ups: c.abort_multipart_upload(Bucket=B, Key=u['Key'], UploadId=u['UploadId'])
print(json.dumps({'t': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), 'prefix': f'{B}/{P}', 'purged': '--purge' in sys.argv,
                  'objects': c.list_objects_v2(Bucket=B, Prefix=P).get('KeyCount', 0), 'versions': len(v.get('Versions', [])),
                  'deleteMarkers': len(v.get('DeleteMarkers', [])), 'incompleteUploads': len(ups),
                  'keys': sorted({x['Key'][len(P):] for x in versions})[:40]}))
