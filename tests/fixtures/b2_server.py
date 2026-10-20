#!/usr/bin/env python3
"""Offline Backblaze B2 native-API (b2api v3) oracle for #282. Synthetic keys only; loopback only.

Control endpoints (POST): /__reset {buckets, capabilities, restricted, versions}, /__rules {rules}, GET /__state.
A rule: {"call": name|"upload_file"|"upload_part"|"download", "status", "code", "remaining", "drop": true,
"store": true (store, then drop the connection), "competitor": true (another upload of the same name lands first),
"partNumber": n, "race": n (n racing uploads store together and reply together)}.
"""
import base64, hashlib, json, socket, threading, time, urllib.parse, uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

KEY_ID = 'PUTGD_SYNTHETIC_B2_KEY_ID'
KEY = 'putgd-synthetic-b2-application-key-not-real'
ACCOUNT = 'synthetic-account'
lock = threading.RLock()
ALL_CAPS = ['listBuckets', 'listFiles', 'readFiles', 'shareFiles', 'writeFiles', 'deleteFiles']


def fresh():
    return {'events': [], 'rules': [], 'capabilities': list(ALL_CAPS), 'restricted': None, 'arrived': 0, 'stored': 0,
            'buckets': {'bucket-public-id': {'name': 'putgd-fixture-public', 'type': 'allPublic'},
                        'bucket-private-id': {'name': 'putgd-fixture-private', 'type': 'allPrivate'}},
            'versions': [], 'clock': 1700000000000}


state = fresh()
blobs = {}


def now():
    state['clock'] += 1
    return state['clock']


def public(version):
    out = {k: version[k] for k in ('fileId', 'fileName', 'bucketId', 'action', 'contentLength', 'contentSha1', 'contentType', 'fileInfo', 'uploadTimestamp')}
    out['accountId'] = ACCOUNT
    return out


def ordered():
    # B2 lists by name, then newest first.
    return sorted(state['versions'], key=lambda v: (v['fileName'], -v['uploadTimestamp']))


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *_):
        pass

    def reply(self, code, value=None, raw=None, headers=None):
        body = raw if raw is not None else json.dumps(value if value is not None else {}).encode()
        self.send_response(code)
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.send_header('Content-Length', str(len(body)))
        if raw is None:
            self.send_header('Content-Type', 'application/json')
        self.end_headers()
        self.wfile.write(body)

    def error(self, code, name, message='synthetic fixture'):
        self.reply(code, {'status': code, 'code': name, 'message': message})

    def drop(self):
        self.close_connection = True
        try:
            self.connection.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.connection.close()

    def rule(self, call, part=None):
        for r in state['rules']:
            if r.get('call') == call and (part is None or r.get('partNumber') in (None, part)) and r.get('remaining', 1) > 0:
                r['remaining'] = r.get('remaining', 1) - 1
                return r
        return None

    def wait_for(self, condition, timeout=10):
        # Called with the state lock held; other requests run while it waits.
        deadline = time.monotonic() + timeout
        while not condition() and time.monotonic() < deadline:
            lock.release()
            time.sleep(0.02)
            lock.acquire()

    def body(self):
        return self.rfile.read(int(self.headers.get('Content-Length', 0)))

    def record(self, call, extra=None):
        event = {'call': call, 'path': self.path.split('?')[0], 'headers': {k.lower(): v for k, v in self.headers.items() if k.lower() != 'authorization'},
                 'authorized': bool(self.headers.get('Authorization'))}
        event.update(extra or {})
        state['events'].append(event)

    def do_GET(self):
        with lock:
            parsed = urllib.parse.urlsplit(self.path)
            if parsed.path == '/__state':
                return self.reply(200, {k: v for k, v in state.items()})
            if parsed.path == '/b2api/v3/b2_authorize_account':
                self.record('b2_authorize_account')
                auth = self.headers.get('Authorization', '')
                expected = 'Basic ' + base64.b64encode(f'{KEY_ID}:{KEY}'.encode()).decode()
                if auth != expected:
                    return self.error(401, 'bad_auth_token', 'invalid key')
                base = f'http://127.0.0.1:{self.server.server_address[1]}'
                # The live b2api v3 shape: capabilities and any bucket restriction sit in storageApi.
                storage = {'apiUrl': base, 'downloadUrl': base, 'capabilities': state['capabilities'], 'namePrefix': None,
                           'bucketId': None, 'bucketName': None, 'infoType': 'storageApi',
                           'recommendedPartSize': 100000000, 'absoluteMinimumPartSize': 5000000}
                if state['restricted']:
                    storage['bucketId'] = state['restricted']
                    storage['bucketName'] = state['buckets'][state['restricted']]['name']
                return self.reply(200, {'accountId': ACCOUNT, 'authorizationToken': 'account-token', 'apiInfo': {'storageApi': storage}})
            if parsed.path.startswith('/file/'):
                _, _, bucket, name = parsed.path.split('/', 3)
                name = urllib.parse.unquote(name)
                query = urllib.parse.parse_qs(parsed.query)
                self.record('download', {'name': name, 'token': bool(query.get('Authorization'))})
                r = self.rule('download')
                if r and r.get('status'):
                    return self.error(r['status'], r.get('code', 'unauthorized'))
                ids = [i for i, b in state['buckets'].items() if b['name'] == bucket]
                if not ids:
                    return self.error(404, 'not_found')
                if state['buckets'][ids[0]]['type'] != 'allPublic':
                    token = (query.get('Authorization') or [''])[0]
                    if not token.startswith('dl:') or not name.startswith(token[3:]):
                        return self.error(401, 'unauthorized')
                for v in ordered():
                    if v['bucketId'] == ids[0] and v['fileName'] == name and v['action'] != 'start':
                        if v['action'] != 'upload':
                            break
                        return self.reply(200, raw=blobs[v['fileId']], headers={'X-Bz-File-Id': v['fileId']})
                return self.error(404, 'not_found')
            self.error(404, 'not_found')

    def do_POST(self):
        data = self.body()
        with lock:
            path = urllib.parse.urlsplit(self.path).path
            if path == '/__reset':
                blobs.clear()
                state.clear()
                state.update(fresh())
                options = json.loads(data or b'{}')
                for k in ('capabilities', 'restricted', 'buckets'):
                    if k in options:
                        state[k] = options[k]
                for v in options.get('versions', []):
                    v = dict(v)
                    v.setdefault('fileId', 'seed-' + uuid.uuid4().hex)
                    v.setdefault('action', 'upload')
                    v.setdefault('fileInfo', {})
                    v.setdefault('contentType', 'text/plain')
                    content = v.pop('content', 'seed').encode()
                    v.setdefault('contentLength', len(content))
                    v.setdefault('contentSha1', hashlib.sha1(content).hexdigest())
                    v['uploadTimestamp'] = now()
                    blobs[v['fileId']] = content
                    state['versions'].append(v)
                return self.reply(200, {})
            if path == '/__rules':
                state['rules'] = json.loads(data)['rules']
                return self.reply(200, {})
            if path.startswith('/upload/'):
                return self.upload_file(path.split('/')[2], data)
            if path.startswith('/upload_part/'):
                return self.upload_part(path.split('/')[2], data)
            if not path.startswith('/b2api/v3/'):
                return self.error(404, 'not_found')
            call = path[len('/b2api/v3/'):]
            body = json.loads(data or b'{}')
            self.record(call, {'body': body})
            if self.headers.get('Authorization') != 'account-token':
                return self.error(401, 'bad_auth_token')
            r = self.rule(call)
            if r and r.get('drop'):
                return self.drop()
            if r and r.get('status'):
                return self.error(r['status'], r.get('code', 'service_unavailable'))
            handler = getattr(self, call, None)
            if not handler:
                return self.error(400, 'bad_request', 'unsupported call')
            needs = {'b2_list_buckets': 'listBuckets', 'b2_list_file_versions': 'listFiles', 'b2_list_parts': 'listFiles',
                     'b2_get_file_info': 'readFiles', 'b2_get_download_authorization': 'shareFiles', 'b2_get_upload_url': 'writeFiles',
                     'b2_start_large_file': 'writeFiles', 'b2_get_upload_part_url': 'writeFiles', 'b2_finish_large_file': 'writeFiles',
                     'b2_cancel_large_file': 'writeFiles', 'b2_delete_file_version': 'deleteFiles'}
            if needs.get(call) and needs[call] not in state['capabilities']:
                return self.error(401, 'unauthorized')
            bucket = body.get('bucketId')
            if bucket and state['restricted'] and bucket != state['restricted']:
                return self.error(401, 'unauthorized')
            return handler(body, r)

    # --- API calls -------------------------------------------------------------
    def b2_list_buckets(self, body, rule):
        out = [{'accountId': ACCOUNT, 'bucketId': i, 'bucketName': b['name'], 'bucketType': b['type']}
               for i, b in state['buckets'].items() if body.get('bucketName') in (None, b['name'])]
        self.reply(200, {'buckets': out})

    def b2_get_upload_url(self, body, rule):
        if body.get('bucketId') not in state['buckets']:
            return self.error(400, 'bad_bucket_id')
        self.reply(200, {'bucketId': body['bucketId'], 'uploadUrl': f'http://127.0.0.1:{self.server.server_address[1]}/upload/{body["bucketId"]}',
                         'authorizationToken': 'upload-token'})

    def competitor(self, bucket, name):
        cid = 'competitor-' + uuid.uuid4().hex
        blobs[cid] = b'competitor'
        state['versions'].append({'fileId': cid, 'fileName': name, 'bucketId': bucket, 'action': 'upload', 'contentLength': 10,
                                  'contentSha1': hashlib.sha1(b'competitor').hexdigest(), 'contentType': 'text/plain', 'fileInfo': {}, 'uploadTimestamp': now()})

    def upload_file(self, bucket, data):
        name = urllib.parse.unquote(self.headers.get('X-Bz-File-Name', ''))
        info = {k.lower()[len('x-bz-info-'):]: urllib.parse.unquote(v) for k, v in self.headers.items() if k.lower().startswith('x-bz-info-')}
        self.record('upload_file', {'name': name, 'bytes': len(data), 'sha1': self.headers.get('X-Bz-Content-Sha1'), 'info': info})
        if self.headers.get('Authorization') != 'upload-token':
            return self.error(401, 'bad_auth_token')
        r = self.rule('upload_file')
        if r and r.get('status'):
            return self.error(r['status'], r.get('code', 'service_unavailable'))
        if r and r.get('drop'):
            return self.drop()
        if hashlib.sha1(data).hexdigest() != self.headers.get('X-Bz-Content-Sha1'):
            return self.error(400, 'bad_request', 'sha1 did not match')
        if r and r.get('race'):
            # A deterministic race: store only once every racer's upload has arrived (so
            # each passed its pre-upload check), reply only once all of them are stored.
            state['arrived'] += 1
            self.wait_for(lambda: state['arrived'] >= r['race'])
        if r and r.get('competitor'):
            self.competitor(bucket, name)
        fid = 'file-' + uuid.uuid4().hex
        blobs[fid] = data
        v = {'fileId': fid, 'fileName': name, 'bucketId': bucket, 'action': 'upload', 'contentLength': len(data),
             'contentSha1': hashlib.sha1(data).hexdigest(), 'contentType': self.headers.get('Content-Type'), 'fileInfo': info, 'uploadTimestamp': now()}
        state['versions'].append(v)
        if r and r.get('store'):
            return self.drop()
        if r and r.get('race'):
            state['stored'] += 1
            self.wait_for(lambda: state['stored'] >= r['race'])
        self.reply(200, public(v))

    def b2_list_file_versions(self, body, rule):
        start, prefix, limit = body.get('startFileName', ''), body.get('prefix', ''), int(body.get('maxFileCount', 100))
        rows = [v for v in ordered() if v['bucketId'] == body.get('bucketId') and v['fileName'] >= start and v['fileName'].startswith(prefix)]
        page, rest = rows[:limit], rows[limit:]
        self.reply(200, {'files': [public(v) for v in page], 'nextFileName': rest[0]['fileName'] if rest else None, 'nextFileId': rest[0]['fileId'] if rest else None})

    def find(self, fid):
        return next((v for v in state['versions'] if v['fileId'] == fid), None)

    def b2_get_file_info(self, body, rule):
        v = self.find(body.get('fileId'))
        if not v:
            return self.error(404, 'not_found')
        self.reply(200, public(v))

    def b2_delete_file_version(self, body, rule):
        v = self.find(body.get('fileId'))
        if not v or v['fileName'] != body.get('fileName'):
            return self.error(400, 'file_not_present')
        state['versions'].remove(v)
        self.reply(200, {'fileId': v['fileId'], 'fileName': v['fileName']})

    def b2_get_download_authorization(self, body, rule):
        self.reply(200, {'bucketId': body['bucketId'], 'fileNamePrefix': body['fileNamePrefix'], 'authorizationToken': 'dl:' + body['fileNamePrefix']})

    def b2_start_large_file(self, body, rule):
        fid = 'large-' + uuid.uuid4().hex
        v = {'fileId': fid, 'fileName': body['fileName'], 'bucketId': body['bucketId'], 'action': 'start', 'contentLength': 0, 'contentSha1': 'none',
             'contentType': body.get('contentType'), 'fileInfo': body.get('fileInfo', {}), 'uploadTimestamp': now(), 'parts': {}}
        state['versions'].append(v)
        self.reply(200, public(v))

    def b2_get_upload_part_url(self, body, rule):
        if not self.find(body.get('fileId')):
            return self.error(400, 'bad_request')
        self.reply(200, {'fileId': body['fileId'], 'uploadUrl': f'http://127.0.0.1:{self.server.server_address[1]}/upload_part/{body["fileId"]}',
                         'authorizationToken': 'part-token'})

    def upload_part(self, fid, data):
        n = int(self.headers.get('X-Bz-Part-Number', 0))
        self.record('upload_part', {'partNumber': n, 'bytes': len(data), 'sha1': self.headers.get('X-Bz-Content-Sha1')})
        if self.headers.get('Authorization') != 'part-token':
            return self.error(401, 'bad_auth_token')
        r = self.rule('upload_part', n)
        if r and r.get('status'):
            return self.error(r['status'], r.get('code', 'service_unavailable'))
        if r and r.get('drop'):
            return self.drop()
        v = self.find(fid)
        if not v or v['action'] != 'start':
            return self.error(400, 'bad_request')
        if hashlib.sha1(data).hexdigest() != self.headers.get('X-Bz-Content-Sha1'):
            return self.error(400, 'bad_request', 'sha1 did not match')
        v['parts'][str(n)] = {'sha1': hashlib.sha1(data).hexdigest(), 'size': len(data)}
        blobs[f'{fid}:{n}'] = data
        self.reply(200, {'fileId': fid, 'partNumber': n, 'contentLength': len(data), 'contentSha1': hashlib.sha1(data).hexdigest()})

    def b2_list_parts(self, body, rule):
        v = self.find(body.get('fileId'))
        if not v or v['action'] != 'start':
            return self.error(400, 'bad_request')
        start, limit = int(body.get('startPartNumber', 1)), int(body.get('maxPartCount', 1000))
        numbers = sorted(int(n) for n in v['parts'] if int(n) >= start)
        page, rest = numbers[:limit], numbers[limit:]
        self.reply(200, {'parts': [{'fileId': v['fileId'], 'partNumber': n, 'contentLength': v['parts'][str(n)]['size'],
                                    'contentSha1': v['parts'][str(n)]['sha1']} for n in page], 'nextPartNumber': rest[0] if rest else None})

    def b2_finish_large_file(self, body, rule):
        v = self.find(body.get('fileId'))
        if not v or v['action'] != 'start':
            return self.error(400, 'bad_request', 'not an unfinished large file')
        sha1s = body.get('partSha1Array', [])
        if [v['parts'].get(str(i + 1), {}).get('sha1') for i in range(len(sha1s))] != sha1s or len(v['parts']) != len(sha1s):
            return self.error(400, 'bad_request', 'part sha1 array mismatch')
        if any(v['parts'][str(i + 1)]['size'] < 5000000 for i in range(len(sha1s) - 1)):
            return self.error(400, 'bad_request', 'part too small')
        if rule and rule.get('competitor'):
            self.competitor(v['bucketId'], v['fileName'])
        data = b''.join(blobs.pop(f'{v["fileId"]}:{i + 1}') for i in range(len(sha1s)))
        blobs[v['fileId']] = data
        v.update({'action': 'upload', 'contentLength': len(data), 'contentSha1': 'none', 'uploadTimestamp': now()})
        del v['parts']
        self.reply(200, public(v))

    def b2_cancel_large_file(self, body, rule):
        v = self.find(body.get('fileId'))
        if not v or v['action'] != 'start':
            return self.error(400, 'bad_request')
        state['versions'].remove(v)
        self.reply(200, {'fileId': v['fileId'], 'fileName': v['fileName']})


server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
print(server.server_address[1], flush=True)
server.serve_forever()
