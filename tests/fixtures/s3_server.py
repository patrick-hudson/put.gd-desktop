#!/usr/bin/env python3
"""Offline S3 wire oracle. Fake credentials only; no AWS network access."""
import base64, hashlib, hmac, json, os, socket, threading, time, urllib.parse, uuid
import xml.etree.ElementTree as ElementTree
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def _time_scale():
    # Budgets scale like tests/fixtures/Waits.h: PUTGD_TEST_TIME_SCALE, clamped to [1, 20].
    try:
        value = float(os.environ.get('PUTGD_TEST_TIME_SCALE', '1'))
    except ValueError:
        return 1.0
    return min(max(value, 1.0), 20.0) if value == value else 1.0


TIME_SCALE = _time_scale()

lock = threading.RLock()
barriers = {}
state = {'events': [], 'objects': {}, 'versioning': '', 'versionless': False, 'rules': [], 'sequence': 0, 'uploads': {}}
# Large bodies (multipart parts and the objects they complete) stay out of the
# JSON state so /__state remains bounded; events and versions record their digest.
blobs = {}
INLINE_BYTES = 2 * 1024 * 1024
ACCESS = 'PUTGD_SYNTHETIC_ACCESS'
SECRET = 'putgd-synthetic-secret-not-a-real-key'

def digest(data): return hashlib.sha256(data).hexdigest()
def stored(data):
    if len(data) <= INLINE_BYTES: return {'body': base64.b64encode(data).decode()}
    blobs[digest(data)] = data
    return {'body': '', 'blob': digest(data), 'size': len(data)}
def payload(value): return blobs[value['blob']] if value.get('blob') else base64.b64decode(value['body'])
def xml_reply(handler, root, body, headers=None):
    handler.respond(200, ('<?xml version="1.0" encoding="UTF-8"?><' + root + ' xmlns="http://s3.amazonaws.com/doc/2006-03-01/">' + body + '</' + root + '>').encode(), headers)
def signature(handler, body):
    try:
        auth = handler.headers['Authorization']
        parts = dict(part.strip().split('=', 1) for part in auth.split(' ', 1)[1].split(','))
        access, date, region, service, terminal = parts['Credential'].split('/')
        assert access == ACCESS and service == 's3' and terminal == 'aws4_request'
        names = parts['SignedHeaders'].split(';')
        canonical_headers = ''.join(name + ':' + ' '.join(handler.headers[name].split()) + '\n' for name in names)
        parsed = urllib.parse.urlsplit(handler.path)
        query = '&'.join(urllib.parse.quote(k, safe='-_.~') + '=' + urllib.parse.quote(v, safe='-_.~') for k, v in sorted(urllib.parse.parse_qsl(parsed.query, keep_blank_values=True)))
        payload = handler.headers['x-amz-content-sha256']
        assert payload == digest(body)
        canonical = '\n'.join((handler.command, parsed.path, query, canonical_headers, parts['SignedHeaders'], payload))
        scope = '/'.join((date, region, service, terminal))
        to_sign = 'AWS4-HMAC-SHA256\n' + handler.headers['x-amz-date'] + '\n' + scope + '\n' + digest(canonical.encode())
        key = ('AWS4' + SECRET).encode()
        for value in (date, region, service, terminal): key = hmac.new(key, value.encode(), hashlib.sha256).digest()
        assert hmac.compare_digest(hmac.new(key, to_sign.encode(), hashlib.sha256).hexdigest(), parts['Signature'])
        return True
    except (AssertionError, KeyError, TypeError, ValueError): return False

class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def log_message(self, *_): pass
    def respond(self, code, body=b'', headers=None):
        self.send_response(code)
        for key, value in (headers or {}).items(): self.send_header(key, str(value))
        if 'Content-Length' not in (headers or {}): self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        if self.command != 'HEAD':
            try: self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError): pass
    def error(self, code, name='AccessDenied', headers=None):
        # Real S3 identifies every failed request with these two headers.
        headers = {'x-amz-request-id': 'SYNTHREQ0123ABCD', 'x-amz-id-2': 'synthetic+Extended/Id2=', **(headers or {})}
        self.respond(code, ('<Error><Code>' + name + '</Code><Message>synthetic fixture</Message></Error>').encode(), headers)
    def do_POST(self):
        body = self.rfile.read(int(self.headers.get('Content-Length', 0)))
        with lock:
            if not self.path.startswith('/__'):
                return self.handle_wire(body)
            if self.path == '/__reset':
                blobs.clear()
                state.update({'events': [], 'objects': {}, 'versioning': '', 'versionless': False, 'rules': [], 'sequence': 0, 'uploads': {}})
                state.update(json.loads(body))
                state['sequence'] = max([state['sequence']] + [int(v['version']) for versions in state['objects'].values() for v in versions if v['version'].isdigit()])
                self.respond(200, b'{}')
            elif self.path == '/__clear_events':
                state['events'] = []
                self.respond(200, b'{}')
            elif self.path == '/__rules':
                state['rules'] = json.loads(body)['rules']
                self.respond(200, b'{}')
            elif self.path == '/__tamper':
                # Make a stored multipart part differ from what the client sent.
                request = json.loads(body)
                state['uploads'][request['uploadId']]['parts'][request['part']]['etag'] = request['etag']
                self.respond(200, b'{}')
            elif self.path == '/__release':
                barriers.setdefault(json.loads(body)['barrier'], threading.Event()).set()
                self.respond(200, b'{}')
            else: self.respond(404)
    def do_GET(self): self.handle_wire()
    def do_HEAD(self): self.handle_wire()
    def do_PUT(self): self.handle_wire()
    def do_DELETE(self): self.handle_wire()
    def handle_wire(self, body=None):
        if body is None: body = self.rfile.read(int(self.headers.get('Content-Length', 0)))
        if self.path == '/__state':
            with lock: self.respond(200, json.dumps(state).encode())
            return
        parsed = urllib.parse.urlsplit(self.path)
        split = urllib.parse.unquote(parsed.path).lstrip('/').split('/', 1)
        bucket = split[0]
        key = split[1] if len(split) > 1 else ''
        query = urllib.parse.parse_qs(parsed.query, keep_blank_values=True)
        signed = bool(self.headers.get('Authorization'))
        event = {'method': self.command, 'path': self.path, 'bucket': bucket, 'key': key,
                 'headers': {k.lower(): v for k,v in self.headers.items()}, 'body': base64.b64encode(body).decode() if len(body) <= INLINE_BYTES else '',
                 'bodyBytes': len(body), 'bodySha256': digest(body),
                 'signatureValid': signature(self, body) if signed else False}
        with lock:
            state['events'].append(event)
            rule = None
            for candidate in state['rules']:
                if candidate.get('method') == self.command and (not candidate.get('key') or candidate['key'] == key) and (not candidate.get('query') or candidate['query'] in parsed.query) and candidate.get('remaining', 1) > 0:
                    candidate['remaining'] = candidate.get('remaining', 1) - 1
                    rule = candidate
                    break
            if signed and not event['signatureValid']:
                self.error(403, 'SignatureDoesNotMatch'); return
            if rule and rule.get('barrier'):
                barrier = barriers.setdefault(rule['barrier'], threading.Event())
                lock.release()
                barrier.wait(10 * TIME_SCALE)
                lock.acquire()
            if rule and rule.get('delay'):
                lock.release()
                time.sleep(rule['delay'])
                lock.acquire()
            if rule and rule.get('redirect'):
                self.respond(307, headers={'Location': self.path}); return
            if rule and rule.get('status'):
                self.error(rule['status'], rule.get('code', 'AccessDenied')); return
            # Like Cloudflare R2 (#145): writes return a version ID, but versions cannot be addressed.
            if state['versionless'] and 'versionId' in query:
                self.error(501, 'NotImplemented'); return
            versions = state['objects'].setdefault(key, []) if key else []
            if rule and rule.get('competitor'):
                versions.append({'version': 'competitor', 'body': base64.b64encode(b'competitor').decode(), 'metadata': {}, 'etag': '"competitor"', 'marker': False})
            current = versions[-1] if versions else None
            selected = next((v for v in versions if v['version'] == query['versionId'][0]), None) if 'versionId' in query else current
            if 'uploads' in query or 'uploadId' in query:
                return self.multipart(key, query, body, versions, rule)
            if self.command == 'GET' and 'versioning' in query:
                xml = '<VersioningConfiguration xmlns="http://s3.amazonaws.com/doc/2006-03-01/">' + ('<Status>' + state['versioning'] + '</Status>' if state['versioning'] else '') + '</VersioningConfiguration>'
                self.respond(200, xml.encode()); return
            if self.command in ('GET', 'HEAD'):
                if self.command == 'GET' and rule and 'responseBody' in rule:
                    self.respond(200, rule['responseBody'].encode()); return
                if not selected or selected.get('marker'):
                    headers = {'x-amz-delete-marker': 'true', 'x-amz-version-id': selected['version']} if selected else {}
                    self.error(404, 'NoSuchKey', headers); return
                headers = {'ETag': selected['etag'], 'Content-Length': len(payload(selected))}
                if selected.get('version'): headers['x-amz-version-id'] = selected['version']
                for k,v in selected['metadata'].items(): headers['x-amz-meta-' + k] = v
                self.respond(200, payload(selected), headers); return
            if self.command == 'PUT':
                if self.headers.get('if-none-match') == '*' and current and not current.get('marker'):
                    self.error(412, 'PreconditionFailed'); return
                if self.headers.get('if-match') and (not current or current.get('marker') or current['etag'] != self.headers['if-match']):
                    self.error(412, 'PreconditionFailed'); return
                state['sequence'] += 1
                version = str(state['sequence']) if state['versioning'] == 'Enabled' else ('null' if state['versioning'] == 'Suspended' else '')
                value = {'version': version, 'body': base64.b64encode(body).decode(),
                         'metadata': {k.lower()[11:]: v for k,v in self.headers.items() if k.lower().startswith('x-amz-meta-')},
                         'etag': '"' + hashlib.md5(body).hexdigest() + '"', 'marker': False}
                if state['versioning'] != 'Enabled': versions[:] = [v for v in versions if v['version'] != version]
                if rule and rule.get('stripMetadata'): value['metadata'] = {}
                versions.append(value)
                headers = {'ETag': value['etag']}
                if version: headers['x-amz-version-id'] = version
                elif state['versionless']: headers['x-amz-version-id'] = uuid.uuid4().hex
            else:
                if self.headers.get('if-match') and (not selected or selected.get('marker') or selected['etag'] != self.headers['if-match']):
                    self.error(412, 'PreconditionFailed'); return
                headers = {}
                if 'versionId' in query:
                    versions[:] = [v for v in versions if v['version'] != query['versionId'][0]]
                    headers['x-amz-version-id'] = query['versionId'][0]
                elif state['versioning']:
                    state['sequence'] += 1
                    marker = str(state['sequence']) if state['versioning'] == 'Enabled' else 'null'
                    if marker == 'null': versions[:] = [v for v in versions if v['version'] != 'null']
                    versions.append({'version': marker, 'body': '', 'metadata': {}, 'etag': '', 'marker': True})
                    headers = {'x-amz-delete-marker': 'true', 'x-amz-version-id': marker}
                else: versions.clear()
            if rule and rule.get('drop'):
                self.close_connection = True
                self.connection.shutdown(socket.SHUT_RDWR)
                self.connection.close()
                return
            self.respond(200 if self.command == 'PUT' else 204, headers=headers)

    def multipart(self, key, query, body, versions, rule):
        # Called with the state lock held. Mirrors S3's multipart wire contract.
        uploads = state['uploads']
        if self.command == 'POST' and 'uploads' in query:
            upload_id = 'synthetic-upload-' + uuid.uuid4().hex
            uploads[upload_id] = {'key': key, 'parts': {},
                                  'metadata': {k.lower()[11:]: v for k,v in self.headers.items() if k.lower().startswith('x-amz-meta-')},
                                  'contentType': self.headers.get('content-type', ''), 'acl': self.headers.get('x-amz-acl', ''),
                                  'storageClass': self.headers.get('x-amz-storage-class', '')}
            return xml_reply(self, 'InitiateMultipartUploadResult', '<Bucket>b</Bucket><Key>' + key + '</Key><UploadId>' + upload_id + '</UploadId>')
        upload_id = query['uploadId'][0]
        upload = uploads.get(upload_id)
        if not upload or upload['key'] != key:
            return self.error(404, 'NoSuchUpload')
        if self.command == 'PUT':
            md5 = hashlib.md5(body)
            if self.headers.get('content-md5') and self.headers['content-md5'] != base64.b64encode(md5.digest()).decode():
                return self.error(400, 'BadDigest')
            number = query['partNumber'][0]
            upload['parts'][number] = {'etag': '"' + md5.hexdigest() + '"', 'size': len(body), **stored(body)}
            if rule and rule.get('drop'):
                self.close_connection = True
                self.connection.shutdown(socket.SHUT_RDWR)
                self.connection.close()
                return
            # Real S3 identifies every response, successful or not, with these two headers.
            return self.respond(200, headers={'ETag': upload['parts'][number]['etag'], 'x-amz-request-id': 'SYNTHPART' + number.zfill(4),
                                              'x-amz-id-2': 'synthetic+Part/Id2' + number + '='})
        if self.command == 'GET':
            marker = int(query.get('part-number-marker', ['0'])[0])
            limit = int(query.get('max-parts', ['1000'])[0])
            numbers = sorted(int(n) for n in upload['parts'] if int(n) > marker)
            page, more = numbers[:limit], len(numbers) > limit
            xml = ''.join('<Part><PartNumber>%d</PartNumber><ETag>%s</ETag><Size>%d</Size></Part>' % (n, upload['parts'][str(n)]['etag'].replace('"', '&quot;'), upload['parts'][str(n)]['size']) for n in page)
            tail = '<IsTruncated>' + ('true' if more else 'false') + '</IsTruncated>' + ('<NextPartNumberMarker>%d</NextPartNumberMarker>' % page[-1] if more else '')
            return xml_reply(self, 'ListPartsResult', '<UploadId>' + upload_id + '</UploadId>' + tail + xml)
        if self.command == 'DELETE':
            del uploads[upload_id]
            return self.respond(204)
        # CompleteMultipartUpload: conditional on the key's current object.
        current = versions[-1] if versions else None
        if self.headers.get('if-none-match') == '*' and current and not current.get('marker'):
            return self.error(412, 'PreconditionFailed')
        if self.headers.get('if-match') and (not current or current.get('marker') or current['etag'] != self.headers['if-match']):
            return self.error(412, 'PreconditionFailed')
        listed = [(int(p.findtext('{*}PartNumber')), p.findtext('{*}ETag')) for p in ElementTree.fromstring(body).iter('{http://s3.amazonaws.com/doc/2006-03-01/}Part')]
        if not listed or [n for n, _ in listed] != sorted(n for n, _ in listed) or any(str(n) not in upload['parts'] or upload['parts'][str(n)]['etag'] != e for n, e in listed):
            return self.error(400, 'InvalidPart')
        data = b''.join(payload(upload['parts'][str(n)]) for n, _ in listed)
        if self.headers.get('x-amz-mp-object-size') and int(self.headers['x-amz-mp-object-size']) != len(data):
            return self.error(400, 'InvalidRequest')
        composite = hashlib.md5(b''.join(bytes.fromhex(upload['parts'][str(n)]['etag'].strip('"')) for n, _ in listed)).hexdigest()
        state['sequence'] += 1
        version = str(state['sequence']) if state['versioning'] == 'Enabled' else ('null' if state['versioning'] == 'Suspended' else '')
        value = {'version': version, **stored(data), 'metadata': upload['metadata'], 'etag': '"%s-%d"' % (composite, len(listed)), 'marker': False,
                 'contentType': upload['contentType'], 'acl': upload['acl'], 'storageClass': upload['storageClass']}
        if state['versioning'] != 'Enabled': versions[:] = [v for v in versions if v['version'] != version]
        versions.append(value)
        del uploads[upload_id]
        headers = {'x-amz-version-id': version} if version else ({'x-amz-version-id': uuid.uuid4().hex} if state['versionless'] else {})
        headers.update({'x-amz-request-id': 'SYNTHDONE0001', 'x-amz-id-2': 'synthetic+Complete/Id2='})
        return xml_reply(self, 'CompleteMultipartUploadResult', '<Key>' + key + '</Key><ETag>' + value['etag'].replace('"', '&quot;') + '</ETag>', headers)

server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
print(server.server_address[1], flush=True)
server.serve_forever()
