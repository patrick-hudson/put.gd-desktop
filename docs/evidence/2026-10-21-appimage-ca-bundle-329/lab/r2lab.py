#!/usr/bin/env python3
"""#329 temporary Cloudflare R2 bucket (Patrick approved 2026-10-21), adapted from round 11's compat.py.
Runs on d69 (boto3). Keys are read from ~/Documents/creds.txt into memory only; never printed or written.

  r2lab.py setup | verify | purge | teardown       state (non-secret) in state-r2.json next to this file
"""
import json, pathlib, re, secrets, sys, time, urllib.error, urllib.request
HERE = pathlib.Path(__file__).resolve().parent
STATE = HERE / 'state-r2.json'
CREDS = pathlib.Path.home() / 'Documents/creds.txt'


def log(record):
    record = {'t': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), **record}
    line = json.dumps(record, sort_keys=True); print(line, flush=True)
    with open(HERE / 'results-r2.jsonl', 'a') as f: f.write(line + '\n')


def creds():
    out, section = {}, None
    for line in CREDS.read_text().splitlines():
        low = line.lower()
        if 'backblaze' in low: section = 'b2'
        elif 'cloudflare' in low: section = 'r2'
        m = re.match(r'^\s*([^:]+?)\s*:\s*(.*?)\s*$', line)
        if section == 'r2':
            if m and m.group(1).lower() == 'token value': out['token'] = m.group(2)
            elif m and m.group(1).lower() == 'access key id': out['ak'] = m.group(2)
            elif m and m.group(1).lower() == 'secret access key': out['sk'] = m.group(2)
            e = re.search(r'(https://[0-9a-f]{32}\.r2\.cloudflarestorage\.com)', line)
            if e: out['endpoint'] = e.group(1)
    return out


def client(c):
    import boto3
    from botocore.config import Config
    cfg = Config(signature_version='s3v4', s3={'addressing_style': 'path'}, retries={'max_attempts': 2, 'mode': 'standard'},
                 request_checksum_calculation='when_required', response_checksum_validation='when_required')
    return boto3.client('s3', endpoint_url=c['endpoint'], region_name='auto', aws_access_key_id=c['ak'], aws_secret_access_key=c['sk'], config=cfg)


def cf(method, url, token, body):
    req = urllib.request.Request(url, data=json.dumps(body).encode(), method=method, headers={'Content-Type': 'application/json', 'Authorization': 'Bearer ' + token})
    try:
        with urllib.request.urlopen(req, timeout=60) as r: return r.status, json.loads(r.read() or b'{}')
    except urllib.error.HTTPError as e:
        try: return e.code, json.loads(e.read() or b'{}')
        except Exception: return e.code, {}


def main():
    action = sys.argv[1]
    c = creds()
    s3 = client(c)
    account = re.search(r'https://([0-9a-f]{32})\.', c['endpoint']).group(1)
    st = json.loads(STATE.read_text()) if STATE.exists() else {}
    if action == 'setup':
        if st: raise SystemExit('state exists')
        name = 'putgd-i329-r2-' + secrets.token_hex(4)
        s3.create_bucket(Bucket=name)
        status, r = cf('PUT', f'https://api.cloudflare.com/client/v4/accounts/{account}/r2/buckets/{name}/domains/managed', c['token'], {'enabled': True})
        dom = (r.get('result') or {}).get('domain')
        st = {'bucket': name, 'endpoint': c['endpoint'], 'region': 'auto', 'publicBase': f'https://{dom}' if status == 200 and dom else None}
        STATE.write_text(json.dumps(st, indent=1))
        log({'step': 'setup', 'bucket': name, 'publicDomain': bool(st['publicBase']), 'cfStatus': status})
    elif action in ('verify', 'purge'):
        objs = s3.list_objects_v2(Bucket=st['bucket']).get('Contents', [])
        ups = s3.list_multipart_uploads(Bucket=st['bucket']).get('Uploads', [])
        if action == 'purge':
            for o in objs: s3.delete_object(Bucket=st['bucket'], Key=o['Key'])
            for u in ups: s3.abort_multipart_upload(Bucket=st['bucket'], Key=u['Key'], UploadId=u['UploadId'])
        log({'step': action, 'bucket': st['bucket'], 'objects': [o['Key'] for o in objs][:50], 'objectCount': len(objs), 'incompleteUploads': len(ups)})
    elif action == 'teardown':
        status = None
        if st.get('publicBase'):
            status, _ = cf('PUT', f'https://api.cloudflare.com/client/v4/accounts/{account}/r2/buckets/{st["bucket"]}/domains/managed', c['token'], {'enabled': False})
        code = s3.delete_bucket(Bucket=st['bucket'])['ResponseMetadata']['HTTPStatusCode']
        names = [b['Name'] for b in s3.list_buckets().get('Buckets', []) if b['Name'].startswith('putgd-i329')]
        log({'step': 'teardown', 'bucket': st['bucket'], 'r2devDisabled': status, 'deleteBucket': code, 'remainingI329Buckets': names})
        STATE.rename(HERE / 'state-r2.retired.json')
    c.clear()


if __name__ == '__main__':
    main()
