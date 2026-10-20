#!/usr/bin/env python3
"""Round 11 (#145) live S3-compatible acceptance driver for Backblaze B2 and Cloudflare R2.

Runs on the builder only. Keys are read from ~/Documents/creds.txt into memory and passed
to put.gd's probes on stdin; they are never printed, logged, written or put in argv/env.
B2's master key cannot use the S3 API, so every invocation mints a bucket-scoped key that
expires within two hours and deletes it again before exiting.

  compat.py PROVIDER setup | raw | test [BASE] | upload FILE MODE [THRESHOLD [LANES]] [BASE] |
            live SMALL CANCEL INTERRUPT BIG | abandon INTERRUPT | verify | purge | teardown
All non-secret results are appended to RUN/results-PROVIDER.jsonl.
"""
import base64, hashlib, json, os, pathlib, re, secrets, subprocess, sys, time, urllib.error, urllib.request

RUN = pathlib.Path('/home/patrick/work/put.gd-r11compat/run')
BUILD = pathlib.Path('/home/patrick/work/put.gd-r11compat/src/build/dev')
PREFIX = 'put.gd/acceptance/round11/'
CREDS = pathlib.Path.home() / 'Documents/creds.txt'


def log(provider, record):
    record = {'t': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), 'provider': provider, **record}
    line = json.dumps(record, sort_keys=True)
    print(line, flush=True)
    with open(RUN / f'results-{provider}.jsonl', 'a') as f:
        f.write(line + '\n')


def secrets_from_file():
    text = CREDS.read_text().splitlines()
    out = {}
    section = None
    for line in text:
        low = line.lower()
        if 'backblaze' in low: section = 'b2'
        elif 'cloudflare' in low: section = 'r2'
        m = re.match(r'^\s*([^:]+?)\s*:\s*(.*?)\s*$', line)
        if section == 'b2' and m:
            k, v = m.group(1).lower(), m.group(2)
            if k == 'keyid': out['b2KeyId'] = v
            elif k.replace(' ', '') == 'applicationkey': out['b2Key'] = v
        if section == 'r2':
            if m and m.group(1).lower() == 'token value': out['cfToken'] = m.group(2)
            elif m and m.group(1).lower() == 'access key id': out['r2KeyId'] = m.group(2)
            elif m and m.group(1).lower() == 'secret access key': out['r2Key'] = m.group(2)
            e = re.search(r'(https://[0-9a-f]{32}\.r2\.cloudflarestorage\.com)', line)
            if e: out['r2Endpoint'] = e.group(1)
    return out


def state_path(provider): return RUN / f'state-{provider}.json'
def load_state(provider):
    p = state_path(provider)
    return json.loads(p.read_text()) if p.exists() else {}
def save_state(provider, state): state_path(provider).write_text(json.dumps(state, indent=1, sort_keys=True))


def http(method, url, body=None, headers=None, auth=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method, headers=headers or {})
    if data is not None: req.add_header('Content-Type', 'application/json')
    if auth: req.add_header('Authorization', auth)
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return r.status, json.loads(r.read() or b'{}')
    except urllib.error.HTTPError as e:
        try: payload = json.loads(e.read() or b'{}')
        except Exception: payload = {}
        return e.code, payload


class B2:
    def __init__(self, s):
        basic = base64.b64encode(f"{s['b2KeyId']}:{s['b2Key']}".encode()).decode()
        st, a = http('GET', 'https://api.backblazeb2.com/b2api/v2/b2_authorize_account', auth='Basic ' + basic)
        if st != 200: raise SystemExit(f'b2 authorize failed: {st} {a.get("code")}')
        self.api, self.token, self.account = a['apiUrl'], a['authorizationToken'], a['accountId']
        self.s3 = a['s3ApiUrl']
        self.download = a['downloadUrl']
        self.keyId = None
    def call(self, name, body):
        return http('POST', f'{self.api}/b2api/v2/{name}', body, auth=self.token)
    def scoped(self, bucketId, bucket):
        st, k = self.call('b2_create_key', {'accountId': self.account, 'keyName': 'putgd-r11-' + secrets.token_hex(3),
            'capabilities': ['listBuckets', 'readBuckets', 'listFiles', 'readFiles', 'shareFiles', 'writeFiles', 'deleteFiles'],
            'validDurationInSeconds': 7200, 'bucketId': bucketId})
        if st != 200: raise SystemExit(f'b2 create key failed: {st} {k.get("code")} {k.get("message")}')
        self.keyId = k['applicationKeyId']
        return k['applicationKeyId'], k['applicationKey']
    def drop(self):
        if self.keyId:
            st, r = self.call('b2_delete_key', {'applicationKeyId': self.keyId})
            print(json.dumps({'scopedKeyDeleted': st == 200}), file=sys.stderr)
            self.keyId = None


class Session:
    """Holds the provider's S3 identity for one invocation."""
    def __init__(self, provider):
        self.provider = provider
        self.s = secrets_from_file()
        self.state = load_state(provider)
        self.b2 = None
    def __enter__(self): return self
    def __exit__(self, *exc):
        if self.b2: self.b2.drop()
        self.s = {}
    def s3_identity(self):
        if self.provider == 'b2':
            self.b2 = B2(self.s)
            if not self.state.get('bucketId'): raise SystemExit('run setup first')
            return self.b2.scoped(self.state['bucketId'], self.state['bucket'])
        return self.s['r2KeyId'], self.s['r2Key']
    def settings(self, base=None):
        st = self.state
        out = {'bucket': st['bucket'], 'region': st['region'], 'endpoint': st['endpoint'], 'objectPrefix': PREFIX.rstrip('/'),
               'addressing': 'path', 'aclMode': 'omit', 'storageClass': 'STANDARD'}
        if base: out['publicUrlBase'] = base
        return out
    def client(self, ak, sk):
        import boto3
        from botocore.config import Config
        cfg = Config(signature_version='s3v4', s3={'addressing_style': 'path'}, retries={'max_attempts': 1, 'mode': 'standard'},
                     request_checksum_calculation='when_required', response_checksum_validation='when_required')
        return boto3.client('s3', endpoint_url=self.state['endpoint'], region_name=self.state['region'],
                            aws_access_key_id=ak, aws_secret_access_key=sk, config=cfg)


def probe_env():
    priv = RUN / 'xdg'
    for d in ('config', 'data', 'cache', 'state', 'runtime', 'tmp'): (priv / d).mkdir(parents=True, exist_ok=True)
    os.chmod(priv / 'runtime', 0o700)
    env = {k: v for k, v in os.environ.items() if k not in ('DISPLAY', 'WAYLAND_DISPLAY', 'DBUS_SESSION_BUS_ADDRESS') and not k.startswith('AWS_')}
    env.update(QT_QPA_PLATFORM='offscreen', XDG_CONFIG_HOME=str(priv / 'config'), XDG_DATA_HOME=str(priv / 'data'),
               XDG_CACHE_HOME=str(priv / 'cache'), XDG_STATE_HOME=str(priv / 'state'), XDG_RUNTIME_DIR=str(priv / 'runtime'),
               TMPDIR=str(priv / 'tmp'), DBUS_SESSION_BUS_ADDRESS='disabled:')
    return env


def run_probe(sess, argv, settings_override=None, timeout=4 * 3600):
    ak, sk = sess.s3_identity()
    payload = {'accessKey': ak, 'secretKey': sk}
    if settings_override is not None: payload['settings'] = settings_override
    data = json.dumps(payload).encode()
    del ak, sk, payload
    p = subprocess.run(argv, input=data, capture_output=True, timeout=timeout, env=probe_env())
    data = b''
    lines = []
    for line in p.stdout.decode(errors='replace').splitlines():
        try: lines.append(json.loads(line))
        except ValueError: lines.append({'stdout': line[:300]})
    err = [l for l in p.stderr.decode(errors='replace').splitlines() if l.strip()][-15:]
    return p.returncode, lines, err


def err(e):
    from botocore.exceptions import ClientError
    if isinstance(e, ClientError):
        r = e.response
        return {'httpStatus': r.get('ResponseMetadata', {}).get('HTTPStatusCode'), 'code': r.get('Error', {}).get('Code'),
                'message': (r.get('Error', {}).get('Message') or '')[:160]}
    return {'exception': type(e).__name__, 'message': str(e)[:160]}


def attempt(fn):
    try: return {'ok': True, 'value': fn()}
    except Exception as e: return {'ok': False, **err(e)}


def with_headers(c, op, headers):
    def add(request, **_):
        for k, v in headers.items(): request.headers[k] = v
    c.meta.events.register_first(f'before-sign.s3.{op}', add)
    return lambda: c.meta.events.unregister(f'before-sign.s3.{op}', add)


def raw_checks(sess):
    ak, sk = sess.s3_identity()
    c = sess.client(ak, sk)
    del ak, sk
    P = PREFIX + 'raw-' + secrets.token_hex(3) + '/'
    out = {}
    body = os.urandom(1024)
    md5 = hashlib.md5(body).hexdigest()
    r = attempt(lambda: c.put_object(Bucket=sess.state['bucket'], Key=P + 'a.bin', Body=body, IfNoneMatch='*')['ETag'])
    out['putIfNoneMatchNew'] = r
    out['singleEtagIsMd5'] = r.get('value', '').strip('"') == md5
    out['putIfNoneMatchExisting'] = attempt(lambda: c.put_object(Bucket=sess.state['bucket'], Key=P + 'a.bin', Body=body, IfNoneMatch='*')['ETag'])
    out['putIfMatchWrong'] = attempt(lambda: c.put_object(Bucket=sess.state['bucket'], Key=P + 'a.bin', Body=body, IfMatch='"00000000000000000000000000000000"')['ETag'])
    out['putIfMatchRight'] = attempt(lambda: c.put_object(Bucket=sess.state['bucket'], Key=P + 'a.bin', Body=body, IfMatch=r.get('value', '""'))['ETag'])
    out['headMissing'] = attempt(lambda: c.head_object(Bucket=sess.state['bucket'], Key=P + 'missing.bin')['ContentLength'])
    out['bucketVersioning'] = attempt(lambda: {k: v for k, v in c.get_bucket_versioning(Bucket=sess.state['bucket']).items() if k != 'ResponseMetadata'})
    # Multipart: three parts, ListParts paged one at a time, conditional completion with the object size.
    B = sess.state['bucket']
    parts = [os.urandom(5 * 1048576), os.urandom(5 * 1048576), os.urandom(1048576)]
    up = c.create_multipart_upload(Bucket=B, Key=P + 'b.bin', ContentType='application/octet-stream',
                                   Metadata={'putgd-publication': 'r11', 'putgd-sha256': hashlib.sha256(b''.join(parts)).hexdigest()})['UploadId']
    etags = []
    for i, part in enumerate(parts, 1):
        etags.append(c.upload_part(Bucket=B, Key=P + 'b.bin', UploadId=up, PartNumber=i, Body=part,
                                   ContentMD5=base64.b64encode(hashlib.md5(part).digest()).decode())['ETag'])
    out['partEtagsAreMd5'] = [e.strip('"') == hashlib.md5(p).hexdigest() for e, p in zip(etags, parts)]
    pages, marker, listed = [], None, []
    while True:
        kw = {'Bucket': B, 'Key': P + 'b.bin', 'UploadId': up, 'MaxParts': 1}
        if marker is not None: kw['PartNumberMarker'] = marker
        res = attempt(lambda: c.list_parts(**kw))
        if not res['ok']: pages.append(res); break
        v = res['value']
        listed += [p['PartNumber'] for p in v.get('Parts', [])]
        pages.append({'parts': [p['PartNumber'] for p in v.get('Parts', [])], 'isTruncated': v.get('IsTruncated'),
                      'nextPartNumberMarker': v.get('NextPartNumberMarker'), 'maxParts': v.get('MaxParts')})
        if not v.get('IsTruncated') or len(pages) > 6: break
        marker = v.get('NextPartNumberMarker')
    out['listPartsPaging'] = {'pages': pages, 'listed': listed}
    out['listPartsDefault'] = attempt(lambda: [(p['PartNumber'], p['Size'], p['ETag'] == e) for p, e in zip(c.list_parts(Bucket=B, Key=P + 'b.bin', UploadId=up)['Parts'], etags)])
    out['listMultipartUploads'] = attempt(lambda: [u['Key'][len(P):] for u in c.list_multipart_uploads(Bucket=B, Prefix=P).get('Uploads', [])])
    mpu = {'Parts': [{'ETag': e, 'PartNumber': i} for i, e in enumerate(etags, 1)]}
    total = sum(map(len, parts))
    undo = with_headers(c, 'CompleteMultipartUpload', {'If-None-Match': '*', 'x-amz-mp-object-size': str(total)})
    res = attempt(lambda: c.complete_multipart_upload(Bucket=B, Key=P + 'b.bin', UploadId=up, MultipartUpload=mpu)['ETag'])
    undo()
    out['completeIfNoneMatchNew'] = res
    expected = hashlib.md5(b''.join(hashlib.md5(p).digest() for p in parts)).hexdigest() + f'-{len(parts)}'
    out['multipartEtag'] = {'value': res.get('value'), 'expectedMd5OfMd5s': expected, 'matches': (res.get('value') or '').strip('"') == expected}
    out['headMultipart'] = attempt(lambda: {k: v for k, v in c.head_object(Bucket=B, Key=P + 'b.bin').items() if k in ('ContentLength', 'ETag', 'Metadata', 'ContentType', 'StorageClass', 'VersionId', 'PartsCount')})
    # Conditional completion over an existing key.
    up2 = c.create_multipart_upload(Bucket=B, Key=P + 'b.bin')['UploadId']
    e2 = c.upload_part(Bucket=B, Key=P + 'b.bin', UploadId=up2, PartNumber=1, Body=parts[0])['ETag']
    undo = with_headers(c, 'CompleteMultipartUpload', {'If-None-Match': '*'})
    out['completeIfNoneMatchExisting'] = attempt(lambda: c.complete_multipart_upload(Bucket=B, Key=P + 'b.bin', UploadId=up2, MultipartUpload={'Parts': [{'ETag': e2, 'PartNumber': 1}]})['ETag'])
    undo()
    out['existingUnchanged'] = attempt(lambda: c.head_object(Bucket=B, Key=P + 'b.bin')['ETag'] == res.get('value'))
    out['uploadAfterRefusedComplete'] = attempt(lambda: len(c.list_parts(Bucket=B, Key=P + 'b.bin', UploadId=up2).get('Parts', [])))
    out['abortAfterRefused'] = attempt(lambda: c.abort_multipart_upload(Bucket=B, Key=P + 'b.bin', UploadId=up2)['ResponseMetadata']['HTTPStatusCode'])
    out['listPartsAfterAbort'] = attempt(lambda: c.list_parts(Bucket=B, Key=P + 'b.bin', UploadId=up2)['Parts'])
    # Wrong object size on completion.
    up3 = c.create_multipart_upload(Bucket=B, Key=P + 'c.bin')['UploadId']
    e3 = c.upload_part(Bucket=B, Key=P + 'c.bin', UploadId=up3, PartNumber=1, Body=parts[2])['ETag']
    undo = with_headers(c, 'CompleteMultipartUpload', {'If-None-Match': '*', 'x-amz-mp-object-size': '1'})
    out['completeWrongObjectSize'] = attempt(lambda: c.complete_multipart_upload(Bucket=B, Key=P + 'c.bin', UploadId=up3, MultipartUpload={'Parts': [{'ETag': e3, 'PartNumber': 1}]})['ETag'])
    undo()
    attempt(lambda: c.abort_multipart_upload(Bucket=B, Key=P + 'c.bin', UploadId=up3))
    # Minimum part size: two 1 MiB parts.
    up4 = c.create_multipart_upload(Bucket=B, Key=P + 'd.bin')['UploadId']
    small = [c.upload_part(Bucket=B, Key=P + 'd.bin', UploadId=up4, PartNumber=i, Body=parts[2])['ETag'] for i in (1, 2)]
    out['completeSmallParts'] = attempt(lambda: c.complete_multipart_upload(Bucket=B, Key=P + 'd.bin', UploadId=up4, MultipartUpload={'Parts': [{'ETag': e, 'PartNumber': i} for i, e in enumerate(small, 1)]})['ETag'])
    attempt(lambda: c.abort_multipart_upload(Bucket=B, Key=P + 'd.bin', UploadId=up4))
    # Content-MD5 mismatch on a part.
    up5 = c.create_multipart_upload(Bucket=B, Key=P + 'e.bin')['UploadId']
    out['uploadPartBadDigest'] = attempt(lambda: c.upload_part(Bucket=B, Key=P + 'e.bin', UploadId=up5, PartNumber=1, Body=parts[2], ContentMD5=base64.b64encode(hashlib.md5(b'x').digest()).decode())['ETag'])
    attempt(lambda: c.abort_multipart_upload(Bucket=B, Key=P + 'e.bin', UploadId=up5))
    out['abortUnknownUpload'] = attempt(lambda: c.abort_multipart_upload(Bucket=B, Key=P + 'e.bin', UploadId=up5)['ResponseMetadata']['HTTPStatusCode'])
    # ETag-guarded deletion, as put.gd removes objects.
    undo = with_headers(c, 'DeleteObject', {'If-Match': '"00000000000000000000000000000000"'})
    out['deleteIfMatchWrong'] = attempt(lambda: c.delete_object(Bucket=B, Key=P + 'b.bin')['ResponseMetadata']['HTTPStatusCode'])
    undo()
    out['afterDeleteWrong'] = attempt(lambda: c.head_object(Bucket=B, Key=P + 'b.bin')['ETag'])
    undo = with_headers(c, 'DeleteObject', {'If-Match': res.get('value') or '""'})
    out['deleteIfMatchRight'] = attempt(lambda: {k: v for k, v in c.delete_object(Bucket=B, Key=P + 'b.bin').items() if k != 'ResponseMetadata'})
    undo()
    out['listObjectVersions'] = attempt(lambda: {k: len(v) for k, v in c.list_object_versions(Bucket=B, Prefix=P).items() if k in ('Versions', 'DeleteMarkers')})
    log(sess.provider, {'step': 'raw', 'prefix': P, 'results': out})
    purge(sess, c, P)


def purge(sess, c, prefix):
    B = sess.state['bucket']
    removed = {'objects': 0, 'versions': 0, 'uploads': 0}
    v = attempt(lambda: c.list_object_versions(Bucket=B, Prefix=prefix))
    if v['ok']:
        for item in v['value'].get('Versions', []) + v['value'].get('DeleteMarkers', []):
            c.delete_object(Bucket=B, Key=item['Key'], VersionId=item['VersionId'])
            removed['versions'] += 1
    for o in c.list_objects_v2(Bucket=B, Prefix=prefix).get('Contents', []):
        c.delete_object(Bucket=B, Key=o['Key'])
        removed['objects'] += 1
    for u in c.list_multipart_uploads(Bucket=B, Prefix=prefix).get('Uploads', []):
        c.abort_multipart_upload(Bucket=B, Key=u['Key'], UploadId=u['UploadId'])
        removed['uploads'] += 1
    log(sess.provider, {'step': 'purge', 'prefix': prefix, 'removed': removed})


def verify(sess):
    ak, sk = sess.s3_identity()
    c = sess.client(ak, sk)
    del ak, sk
    B = sess.state['bucket']
    out = {'objects': attempt(lambda: [o['Key'] for o in c.list_objects_v2(Bucket=B, Prefix=PREFIX).get('Contents', [])]),
           'versions': attempt(lambda: {k: [x['Key'] for x in v] for k, v in c.list_object_versions(Bucket=B, Prefix=PREFIX).items() if k in ('Versions', 'DeleteMarkers')}),
           'incompleteUploads': attempt(lambda: [u['Key'] for u in c.list_multipart_uploads(Bucket=B, Prefix=PREFIX).get('Uploads', [])]),
           'bucketObjects': attempt(lambda: c.list_objects_v2(Bucket=B).get('KeyCount', 0))}
    log(sess.provider, {'step': 'verify', 'prefix': PREFIX, 'results': out})


def setup(sess):
    st = sess.state
    if st.get('bucket'): return log(sess.provider, {'step': 'setup', 'reused': st})
    name = f'putgd-r11-{sess.provider}-' + secrets.token_hex(4)
    if sess.provider == 'b2':
        b2 = B2(sess.s)
        status, b = b2.call('b2_create_bucket', {'accountId': b2.account, 'bucketName': name, 'bucketType': 'allPublic'})
        public = status == 200
        if not public:
            log(sess.provider, {'step': 'setup', 'allPublicRefused': {'status': status, 'code': b.get('code'), 'message': b.get('message', '')[:200]}})
            status, b = b2.call('b2_create_bucket', {'accountId': b2.account, 'bucketName': name, 'bucketType': 'allPrivate'})
        if status != 200: raise SystemExit(f'b2 create bucket failed: {status} {b.get("code")} {b.get("message")}')
        region = re.match(r'https://s3\.([a-z0-9-]+)\.backblazeb2\.com', b2.s3).group(1)
        st.update(bucket=name, bucketId=b['bucketId'], bucketType=b['bucketType'], endpoint=b2.s3, region=region,
                  publicBase=f'{b2.download}/file/{name}' if public else None, versioning='always (B2 keeps versions)')
    else:
        st.update(bucket=name, endpoint=sess.s['r2Endpoint'], region='auto')
        save_state(sess.provider, st)
        ak, sk = sess.s3_identity()
        c = sess.client(ak, sk)
        del ak, sk
        made = attempt(lambda: c.create_bucket(Bucket=name)['ResponseMetadata']['HTTPStatusCode'])
        if not made['ok']: st.clear(); save_state(sess.provider, st); raise SystemExit(f'r2 create bucket failed: {made}')
        account = re.search(r'https://([0-9a-f]{32})\.', st['endpoint']).group(1)
        status, r = http('PUT', f'https://api.cloudflare.com/client/v4/accounts/{account}/r2/buckets/{name}/domains/managed',
                         {'enabled': True}, auth='Bearer ' + sess.s['cfToken'])
        dom = (r.get('result') or {}).get('domain')
        st['publicBase'] = f'https://{dom}' if status == 200 and dom else None
        if not st['publicBase']:
            log(sess.provider, {'step': 'setup', 'r2devRefused': {'status': status, 'errors': [{'code': e.get('code'), 'message': e.get('message', '')[:160]} for e in r.get('errors', [])]}})
    save_state(sess.provider, st)
    log(sess.provider, {'step': 'setup', 'state': st})


def teardown(sess):
    st = sess.state
    if sess.provider == 'b2':
        b2 = B2(sess.s)
        status, r = b2.call('b2_delete_bucket', {'accountId': b2.account, 'bucketId': st['bucketId']})
        log('b2', {'step': 'teardown', 'deleteBucket': status, 'code': r.get('code')})
    else:
        account = re.search(r'https://([0-9a-f]{32})\.', st['endpoint']).group(1)
        if st.get('publicBase'):
            status, r = http('PUT', f'https://api.cloudflare.com/client/v4/accounts/{account}/r2/buckets/{st["bucket"]}/domains/managed',
                             {'enabled': False}, auth='Bearer ' + sess.s['cfToken'])
            log('r2', {'step': 'teardown', 'r2devDisabled': status})
        ak, sk = sess.s3_identity()
        c = sess.client(ak, sk)
        del ak, sk
        log('r2', {'step': 'teardown', 'deleteBucket': attempt(lambda: c.delete_bucket(Bucket=st['bucket'])['ResponseMetadata']['HTTPStatusCode'])})
    state_path(sess.provider).rename(RUN / f'state-{sess.provider}.retired.json')


def main():
    provider, action, rest = sys.argv[1], sys.argv[2], sys.argv[3:]
    assert provider in ('b2', 'r2')
    with Session(provider) as sess:
        if action == 'setup': setup(sess)
        elif action == 'raw': raw_checks(sess)
        elif action == 'verify': verify(sess)
        elif action == 'teardown': teardown(sess)
        elif action == 'purge':
            ak, sk = sess.s3_identity(); c = sess.client(ak, sk); del ak, sk
            purge(sess, c, PREFIX)
        elif action == 'test':
            base = rest[0] if rest else None
            code, lines, errs = run_probe(sess, [str(BUILD / 's3_compat_upload_probe'), '/dev/null', 'probe'], sess.settings(base))
            log(provider, {'step': 'testConnection', 'publicUrlBase': base, 'exit': code, 'lines': lines, 'stderr': errs})
        elif action == 'upload':
            base = None
            if rest and rest[-1].startswith('https://'): base = rest.pop()
            code, lines, errs = run_probe(sess, [str(BUILD / 's3_compat_upload_probe'), *rest], sess.settings(base))
            log(provider, {'step': 'upload', 'args': [pathlib.Path(rest[0]).name, *rest[1:]], 'publicUrlBase': base, 'exit': code, 'lines': lines, 'stderr': errs})
        elif action in ('live', 'abandon', 'delete-existing'):
            st = sess.state
            if action == 'delete-existing':
                data = RUN / rest[0]
                rest = ['--delete-existing', rest[1] if len(rest) > 1 else '-', '-', '-']
            else:
                data = RUN / ('live-data-' + secrets.token_hex(3))
                data.mkdir()
            args = [str(BUILD / 's3_compat_live_probe'), st['bucket'], st['region'], st['endpoint'], PREFIX.rstrip('/'), str(data)]
            args += rest if action in ('live', 'delete-existing') else ['--abandon-only', '-', rest[0], '-']
            code, lines, errs = run_probe(sess, args, {'addressing': 'path'})
            log(provider, {'step': action, 'exit': code, 'lines': lines, 'stderr': errs, 'dataDir': data.name})
        else:
            raise SystemExit('unknown action')


if __name__ == '__main__':
    main()
