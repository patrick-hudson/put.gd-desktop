#!/usr/bin/env python3
"""B2 native-destination lab helper (round 11, PR B). Runs on the builder only.

The master key is read from ~/Documents/creds.txt into memory; it is never printed,
logged, written or passed in argv/env. It is used only to create and delete this run's
own buckets and short-lived application keys restricted to them.

  b2lab.py setup-bucket NAME_PREFIX TYPE   create a uniquely named bucket (allPublic|allPrivate)
  b2lab.py run STATE_NAME -- CMD...        mint a 2 h key restricted to the bucket, run CMD with
                                           {"applicationKeyId","applicationKey"} on its stdin, delete the key
  b2lab.py list-own                        names of buckets this lab created (prefix putgd-r11-) and key names
  b2lab.py verify STATE_NAME               count file versions, hide markers and unfinished large files
  b2lab.py teardown STATE_NAME             delete every file version in the bucket, then the bucket
Non-secret state lives in RUN/state-NAME.json; results in RUN/results.jsonl.
"""
import base64, json, pathlib, re, secrets, subprocess, sys, time, urllib.error, urllib.request

RUN = pathlib.Path(__file__).resolve().parent
CREDS = pathlib.Path.home() / 'Documents/creds.txt'
API = 'https://api.backblazeb2.com/b2api/v3'


def log(record):
    record = {'t': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), **record}
    line = json.dumps(record, sort_keys=True)
    print(line, flush=True)
    with open(RUN / 'results.jsonl', 'a') as f: f.write(line + '\n')


def master():
    section, out = None, {}
    for line in CREDS.read_text().splitlines():
        if 'backblaze' in line.lower(): section = 'b2'
        elif 'cloudflare' in line.lower(): section = None
        m = re.match(r'^\s*([^:]+?)\s*:\s*(.*?)\s*$', line)
        if section == 'b2' and m:
            k = m.group(1).lower().replace(' ', '')
            if k == 'keyid': out['id'] = m.group(2)
            elif k == 'applicationkey': out['key'] = m.group(2)
    return out


def http(method, url, body=None, auth=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method)
    if data is not None: req.add_header('Content-Type', 'application/json')
    if auth: req.add_header('Authorization', auth)
    try:
        with urllib.request.urlopen(req, timeout=60) as r: return r.status, json.loads(r.read() or b'{}')
    except urllib.error.HTTPError as e:
        try: return e.code, json.loads(e.read() or b'{}')
        except Exception: return e.code, {}


class Account:
    def __init__(self):
        m = master()
        st, a = http('GET', API + '/b2_authorize_account', auth='Basic ' + base64.b64encode(f"{m['id']}:{m['key']}".encode()).decode())
        m.clear()
        if st != 200: raise SystemExit(f'authorize failed: {st} {a.get("code")}')
        s = a['apiInfo']['storageApi']
        self.api, self.token, self.account, self.download = s['apiUrl'], a['authorizationToken'], a['accountId'], s['downloadUrl']
    def call(self, name, body): return http('POST', f'{self.api}/b2api/v3/{name}', body, auth=self.token)


def state(name): return json.loads((RUN / f'state-{name}.json').read_text())


def main():
    action, rest = sys.argv[1], sys.argv[2:]
    if action == 'setup-bucket':
        prefix, kind = rest
        acct = Account()
        name = f'{prefix}-{secrets.token_hex(4)}'
        st, b = acct.call('b2_create_bucket', {'accountId': acct.account, 'bucketName': name, 'bucketType': kind})
        log({'step': 'createBucket', 'bucketType': kind, 'status': st, 'code': b.get('code'), 'message': (b.get('message') or '')[:200], 'bucket': name if st == 200 else None})
        if st == 200:
            (RUN / f'state-{kind}.json').write_text(json.dumps({'bucket': name, 'bucketId': b['bucketId'], 'bucketType': b['bucketType'],
                                                               'downloadUrl': acct.download, 'apiUrl': acct.api}, indent=1))
    elif action == 'run':
        name, cmd = rest[0], rest[rest.index('--') + 1:]
        s = state(name)
        acct = Account()
        st, k = acct.call('b2_create_key', {'accountId': acct.account, 'keyName': 'putgd-r11-' + secrets.token_hex(3),
            'capabilities': ['listBuckets', 'listFiles', 'readFiles', 'shareFiles', 'writeFiles', 'deleteFiles'],
            'validDurationInSeconds': 7200, 'bucketId': s['bucketId']})
        if st != 200: raise SystemExit(f'create key failed: {st} {k.get("code")} {k.get("message")}')
        try:
            data = json.dumps({'keyId': k['applicationKeyId'], 'applicationKey': k['applicationKey']}).encode()
            k['applicationKey'] = ''
            p = subprocess.run(cmd, input=data)
            data = b''
            sys.exit(p.returncode)
        finally:
            st2, _ = acct.call('b2_delete_key', {'applicationKeyId': k['applicationKeyId']})
            print(json.dumps({'scopedKeyDeleted': st2 == 200}), file=sys.stderr)
    elif action == 'list-own':
        acct = Account()
        _, b = acct.call('b2_list_buckets', {'accountId': acct.account})
        _, k = acct.call('b2_list_keys', {'accountId': acct.account, 'maxKeyCount': 1000})
        print(json.dumps({'ownBuckets': [x['bucketName'] for x in b.get('buckets', []) if x['bucketName'].startswith('putgd-r11-')],
                          'ownKeys': [x['keyName'] for x in k.get('keys', []) if x['keyName'].startswith('putgd-r11-')]}))
    elif action == 'verify':
        s = state(rest[0])
        acct = Account()
        versions, start = [], {}
        while True:
            _, r = acct.call('b2_list_file_versions', {'bucketId': s['bucketId'], 'maxFileCount': 1000, **start})
            versions += [{'fileName': f['fileName'], 'action': f['action']} for f in r.get('files', [])]
            if not r.get('nextFileName'): break
            start = {'startFileName': r['nextFileName'], 'startFileId': r.get('nextFileId')}
        _, u = acct.call('b2_list_unfinished_large_files', {'bucketId': s['bucketId']})
        log({'step': 'verify', 'bucket': s['bucket'], 'fileVersions': len(versions), 'hideMarkers': sum(v['action'] == 'hide' for v in versions),
             'unfinishedLargeFiles': len(u.get('files', [])), 'names': sorted({v['fileName'] for v in versions})[:20]})
    elif action == 'cancel-unfinished':
        s = state(rest[0])
        acct = Account()
        _, u = acct.call('b2_list_unfinished_large_files', {'bucketId': s['bucketId']})
        done = [acct.call('b2_cancel_large_file', {'fileId': f['fileId']})[0] for f in u.get('files', [])]
        log({'step': 'cancelUnfinished', 'bucket': s['bucket'], 'cancelled': done.count(200), 'of': len(done)})
    elif action == 'teardown':
        s = state(rest[0])
        acct = Account()
        removed, start = 0, {}
        while True:
            _, r = acct.call('b2_list_file_versions', {'bucketId': s['bucketId'], 'maxFileCount': 1000, **start})
            for f in r.get('files', []):
                st, _ = acct.call('b2_delete_file_version', {'fileName': f['fileName'], 'fileId': f['fileId']}); removed += st == 200
            if not r.get('nextFileName'): break
            start = {'startFileName': r['nextFileName'], 'startFileId': r.get('nextFileId')}
        _, u = acct.call('b2_list_unfinished_large_files', {'bucketId': s['bucketId']})
        for f in u.get('files', []): acct.call('b2_cancel_large_file', {'fileId': f['fileId']}); removed += 1
        st, r = acct.call('b2_delete_bucket', {'accountId': acct.account, 'bucketId': s['bucketId']})
        log({'step': 'teardown', 'bucket': s['bucket'], 'removedVersionsAndLargeFiles': removed, 'deleteBucket': st, 'code': r.get('code')})
        (RUN / f'state-{rest[0]}.json').rename(RUN / f'state-{rest[0]}.retired.json')
    else:
        raise SystemExit('unknown action')


if __name__ == '__main__':
    main()
