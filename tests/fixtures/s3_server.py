#!/usr/bin/env python3
"""Offline S3 wire oracle. Fake credentials only; no AWS network access."""
import base64, hashlib, hmac, json, socket, threading, time, urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

lock = threading.RLock()
barriers = {}
state = {'events': [], 'objects': {}, 'versioning': '', 'rules': [], 'sequence': 0}
ACCESS = 'PUTGD_SYNTHETIC_ACCESS'
SECRET = 'putgd-synthetic-secret-not-a-real-key'

def digest(data): return hashlib.sha256(data).hexdigest()
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
            if self.path == '/__reset':
                state.update({'events': [], 'objects': {}, 'versioning': '', 'rules': [], 'sequence': 0})
                state.update(json.loads(body))
                state['sequence'] = max([state['sequence']] + [int(v['version']) for versions in state['objects'].values() for v in versions if v['version'].isdigit()])
                self.respond(200, b'{}')
            elif self.path == '/__release':
                barriers.setdefault(json.loads(body)['barrier'], threading.Event()).set()
                self.respond(200, b'{}')
            else: self.respond(404)
    def do_GET(self): self.handle_wire()
    def do_HEAD(self): self.handle_wire()
    def do_PUT(self): self.handle_wire()
    def do_DELETE(self): self.handle_wire()
    def handle_wire(self):
        body = self.rfile.read(int(self.headers.get('Content-Length', 0)))
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
                 'headers': {k.lower(): v for k,v in self.headers.items()}, 'body': base64.b64encode(body).decode(),
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
                barrier.wait(10)
                lock.acquire()
            if rule and rule.get('delay'):
                lock.release()
                time.sleep(rule['delay'])
                lock.acquire()
            if rule and rule.get('redirect'):
                self.respond(307, headers={'Location': self.path}); return
            if rule and rule.get('status'):
                self.error(rule['status'], rule.get('code', 'AccessDenied')); return
            versions = state['objects'].setdefault(key, []) if key else []
            if rule and rule.get('competitor'):
                versions.append({'version': 'competitor', 'body': base64.b64encode(b'competitor').decode(), 'metadata': {}, 'etag': '"competitor"', 'marker': False})
            current = versions[-1] if versions else None
            selected = next((v for v in versions if v['version'] == query['versionId'][0]), None) if 'versionId' in query else current
            if self.command == 'GET' and 'versioning' in query:
                xml = '<VersioningConfiguration xmlns="http://s3.amazonaws.com/doc/2006-03-01/">' + ('<Status>' + state['versioning'] + '</Status>' if state['versioning'] else '') + '</VersioningConfiguration>'
                self.respond(200, xml.encode()); return
            if self.command in ('GET', 'HEAD'):
                if self.command == 'GET' and rule and 'responseBody' in rule:
                    self.respond(200, rule['responseBody'].encode()); return
                if not selected or selected.get('marker'):
                    headers = {'x-amz-delete-marker': 'true', 'x-amz-version-id': selected['version']} if selected else {}
                    self.error(404, 'NoSuchKey', headers); return
                headers = {'ETag': selected['etag'], 'Content-Length': len(base64.b64decode(selected['body']))}
                if selected.get('version'): headers['x-amz-version-id'] = selected['version']
                for k,v in selected['metadata'].items(): headers['x-amz-meta-' + k] = v
                self.respond(200, base64.b64decode(selected['body']), headers); return
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

server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
print(server.server_address[1], flush=True)
server.serve_forever()
