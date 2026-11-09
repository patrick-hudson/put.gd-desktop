#!/usr/bin/env python3
"""Scripted loopback receiver for the custom HTTP uploader (#1361). Synthetic data only.

It accepts an upload in any of the body types put.gd sends and answers in the form the
request path names, so one process plays every case:

  /upload/json    {"files":[{"url","thumb","delete"}],"id"}       {json:files[0].url}
  /upload/xml     <upload><file id=".."><url>..</url>..</file></upload>   {xml:/upload/file/url}
  /upload/html    <a href="..">                                    {regex:href="([^"]+)"|1}
  /upload/header  201 with Location and X-Delete headers           {header:Location}
  /upload/text    the link as the whole reply                      (empty URL value)
  /upload/bare    {"id"} only, for a URL built around it           https://host/f/{json:id}
  /upload/nolink  200 with nothing a template finds
  /upload/see     303 to /result/<id> on this host                 {responseurl} or {json:..}
  /upload/away    302 to /result/<id> on another host name
  /upload/moved   307 to /upload/json on this host
  /upload/moved-away  307 to /upload/json on another host name
  /upload/loop    302 to itself

The token is checked where /__reset says: {"auth": "header" | "query" | "argument" | "none"}.
A refused token is echoed in the error body, as some servers do.

Control: POST /__reset {auth, token}, POST /__rules {rules}, GET /__state.
A rule: {"call": "upload" | "delete" | "file", "status", "body", "remaining", "delay" (seconds),
"drop": true (close without a reply), "store": true (store first; with drop or status this is a
reply lost after the file was stored)}.
"""
import hashlib, json, os, socket, sys, threading, time, urllib.parse, uuid
from email.parser import BytesParser
from email.policy import HTTP
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

TOKEN = 'putgd-synthetic-uploader-token-not-real'
lock = threading.RLock()
OTHER_HOST = 'localhost'  # the same listener under another host name


def fresh():
    return {'events': [], 'rules': [], 'auth': 'header', 'token': TOKEN, 'files': {}}


state = fresh()
blobs = {}


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *_):
        pass

    def origin(self, host=None):
        return 'http://%s:%d' % (host or '127.0.0.1', self.server.server_address[1])

    def reply(self, code, body=b'', content_type='application/json', headers=None):
        if isinstance(body, (dict, list)):
            body = json.dumps(body).encode()
        elif isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header('Content-Type', content_type)
        self.send_header('Content-Length', str(len(body)))
        for name, value in (headers or {}).items():
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(body)

    def drop(self):
        # An abrupt close: no status line reaches the client.
        try:
            self.connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, b'\x01\x00\x00\x00\x00\x00\x00\x00')
            self.connection.close()
        except OSError:
            pass
        self.close_connection = True

    def rule(self, call):
        with lock:
            for rule in state['rules']:
                if rule.get('call') == call and rule.get('remaining', 1) > 0:
                    rule['remaining'] = rule.get('remaining', 1) - 1
                    return rule
        return {}

    def body(self):
        length = int(self.headers.get('Content-Length') or 0)
        data = b''
        while len(data) < length:
            chunk = self.rfile.read(min(65536, length - len(data)))
            if not chunk:
                break
            data += chunk
        return data

    def record(self, call, extra=None):
        parsed = urllib.parse.urlsplit(self.path)
        kept = {k.lower(): v for k, v in self.headers.items() if k.lower() in ('authorization', 'content-type', 'user-agent', 'host') or k.lower().startswith('x-')}
        event = {'call': call, 'method': self.command, 'path': parsed.path, 'query': dict(urllib.parse.parse_qsl(parsed.query, keep_blank_values=True)), 'headers': kept}
        event.update(extra or {})
        with lock:
            state['events'].append(event)
        return event

    def do_GET(self):
        parsed = urllib.parse.urlsplit(self.path)
        parts = parsed.path.strip('/').split('/')
        if parsed.path == '/__state':
            with lock:
                return self.reply(200, state)
        if parts[0] == 'upload':
            return self.upload(b'')
        if parts[0] == 'f' and len(parts) >= 2:
            self.record('file')
            rule = self.rule('file')
            if rule.get('status'):
                return self.reply(rule['status'], rule.get('body', ''))
            with lock:
                entry = state['files'].get(parts[1])
            if not entry or entry.get('deleted'):
                return self.reply(404, {'error': 'no such file'})
            return self.reply(200, blobs[parts[1]], entry['type'] or 'application/octet-stream')
        if parts[0] == 'result' and len(parts) == 2:
            self.record('result')
            with lock:
                entry = state['files'].get(parts[1])
            if not entry:
                return self.reply(404, {'error': 'no such upload'})
            return self.reply(200, self.links(parts[1]))
        if parts[0] == 'delete' and len(parts) == 3:
            self.record('delete')
            rule = self.rule('delete')
            if rule.get('delay'):
                time.sleep(rule['delay'])
            with lock:
                entry = state['files'].get(parts[1])
                valid = bool(entry) and entry['deleteToken'] == parts[2] and not entry.get('deleted')
                if valid and (not rule or rule.get('store')):
                    entry['deleted'] = True
            if rule.get('drop'):
                return self.drop()
            if rule.get('status'):
                return self.reply(rule['status'], rule.get('body', ''))
            return self.reply(200, {'deleted': True}) if valid else self.reply(404, {'error': 'no such file'})
        self.reply(404, {'error': 'unknown path'})

    def do_POST(self):
        if self.path == '/__reset':
            options = json.loads(self.body() or b'{}')
            with lock:
                state.clear()
                state.update(fresh())
                state.update({k: options[k] for k in ('auth', 'token') if k in options})
                blobs.clear()
            return self.reply(200, {})
        if self.path == '/__rules':
            with lock:
                state['rules'] = json.loads(self.body())['rules']
            return self.reply(200, {})
        if self.path.startswith('/upload/'):
            return self.upload(self.body())
        self.reply(404, {'error': 'unknown path'})

    do_PUT = do_POST
    do_PATCH = do_POST

    def links(self, file_id):
        entry = state['files'][file_id]
        base = self.origin()
        return {'id': file_id, 'files': [{'url': '%s/f/%s/%s' % (base, file_id, urllib.parse.quote(entry['name'])),
                                          'thumb': '%s/f/%s/thumb' % (base, file_id),
                                          'delete': '%s/delete/%s/%s' % (base, file_id, entry['deleteToken'])}]}

    def upload(self, data):
        parsed = urllib.parse.urlsplit(self.path)
        style = parsed.path.strip('/').split('/')[1] if '/' in parsed.path.strip('/') else 'json'
        query = dict(urllib.parse.parse_qsl(parsed.query, keep_blank_values=True))
        content_type = self.headers.get('Content-Type', '')
        fields, name, content, file_type, kind = {}, '', data, content_type, 'binary'
        if content_type.startswith('multipart/form-data'):
            kind = 'multipart'
            message = BytesParser(policy=HTTP).parsebytes(b'Content-Type: ' + content_type.encode() + b'\r\n\r\n' + data)
            content = None
            for part in message.iter_parts():
                field = part.get_param('name', header='content-disposition')
                if part.get_filename() is not None:
                    name, content, file_type = part.get_filename(), part.get_payload(decode=True), part.get_content_type()
                    fields['__file_field'] = field
                else:
                    fields[field] = part.get_payload(decode=True).decode()
            if content is None:
                content = fields.get('content', '').encode()
        elif content_type.startswith('application/x-www-form-urlencoded'):
            kind = 'form'
            fields = dict(urllib.parse.parse_qsl(data.decode(), keep_blank_values=True))
            content, name = fields.get('content', '').encode(), fields.get('name', '')
        elif content_type.startswith('application/json'):
            kind = 'json'
            try:
                fields = json.loads(data)
            except ValueError:
                return self.reply(400, {'error': 'body is not JSON'})
            content, name = str(fields.get('content', '')).encode(), str(fields.get('name', ''))
        elif content_type.startswith('application/xml'):
            kind = 'xml'
        elif self.command == 'GET':
            kind = 'none'
            content, name = query.get('content', '').encode(), query.get('name', '')
        else:
            name = query.get('name', '')
        with lock:
            auth, token = state['auth'], state['token']
        offered = {'header': self.headers.get('Authorization', ''), 'query': query.get('key', ''), 'argument': str(fields.get('key', '')), 'none': ''}[auth]
        event = self.record('upload', {'style': style, 'kind': kind, 'fields': {k: v for k, v in fields.items() if isinstance(v, str)}, 'fileName': name,
                                       'fileBytes': len(content), 'fileType': file_type, 'sha256': hashlib.sha256(content).hexdigest()})
        if auth != 'none' and offered not in (token, 'Bearer ' + token):
            event['refused'] = True
            return self.reply(401, {'error': 'token %s is not valid' % offered})
        rule = self.rule('upload')
        if rule.get('delay'):
            time.sleep(rule['delay'])
        if rule and not rule.get('store') and (rule.get('drop') or rule.get('status')):
            if rule.get('drop'):
                return self.drop()
            return self.reply(rule['status'], rule.get('body', {'error': 'synthetic failure'}), rule.get('type', 'application/json'))
        file_id = uuid.uuid4().hex[:12]
        with lock:
            blobs[file_id] = content
            state['files'][file_id] = {'name': name or 'file', 'type': file_type, 'bytes': len(content), 'sha256': hashlib.sha256(content).hexdigest(),
                                       'deleteToken': uuid.uuid4().hex, 'deleted': False}
            links = self.links(file_id)
        event['stored'] = file_id
        if rule.get('drop'):
            return self.drop()
        if rule.get('status'):
            return self.reply(rule['status'], rule.get('body', {'error': 'synthetic failure'}), rule.get('type', 'application/json'))
        first = links['files'][0]
        if style == 'json':
            return self.reply(200, links)
        if style == 'xml':
            return self.reply(200, '<?xml version="1.0"?><upload><file id="%s"><url>%s</url><thumb>%s</thumb><delete href="%s"/></file></upload>' %
                              (file_id, first['url'], first['thumb'], first['delete']), 'application/xml')
        if style == 'html':
            return self.reply(200, '<html><body>Stored. <a class="link" href="%s">open</a> <a class="remove" href="%s">delete</a></body></html>' %
                              (first['url'], first['delete']), 'text/html')
        if style == 'header':
            return self.reply(201, '', 'text/plain', {'Location': first['url'], 'X-Delete': first['delete']})
        if style == 'text':
            return self.reply(200, first['url'] + '\n', 'text/plain')
        if style == 'bare':
            return self.reply(200, {'id': file_id, 'name': urllib.parse.quote(name or 'file')})
        if style == 'nolink':
            return self.reply(200, {'ok': True})
        if style in ('see', 'away'):
            return self.reply(303 if style == 'see' else 302, '', 'text/plain', {'Location': '%s/result/%s' % (self.origin(OTHER_HOST if style == 'away' else None), file_id)})
        if style in ('moved', 'moved-away'):
            # The redirect asks for the same request again: what was stored here is discarded.
            with lock:
                state['files'].pop(file_id, None)
                blobs.pop(file_id, None)
            event.pop('stored', None)
            target = '%s/upload/json%s' % (self.origin(OTHER_HOST if style == 'moved-away' else None), ('?' + parsed.query) if parsed.query else '')
            return self.reply(307, '', 'text/plain', {'Location': target})
        if style == 'loop':
            return self.reply(302, '', 'text/plain', {'Location': self.origin() + '/upload/loop'})
        return self.reply(404, {'error': 'unknown reply style'})


def main():
    server = ThreadingHTTPServer(('127.0.0.1', int(os.environ.get('PUTGD_HTTP_FIXTURE_PORT', '0'))), Handler)
    server.daemon_threads = True
    sys.stdout.write('%d\n' % server.server_address[1])
    sys.stdout.flush()
    server.serve_forever()


if __name__ == '__main__':
    main()
