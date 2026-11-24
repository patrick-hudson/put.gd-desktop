#!/usr/bin/env python3
"""A small HTTP/1.1 fault proxy for the S3 differential (tools/lab/s3_differential.py).

It sits between put.gd and a real S3-compatible server on loopback and relays every request. A rule can make it
  lose_reply  forward the request, read the server's whole reply, then close the client connection without sending it
              (the request took effect, the client sees a transport error: put.gd's unknown-outcome path)
  drop        close the client connection before forwarding (nothing took effect)
  status      answer with an S3 error document instead of forwarding (default 500 InternalError)
  delay       sleep before forwarding
  tamper      flip one byte of the request body before forwarding (Content-MD5 and the payload hash must catch it)
A rule matches a method, an optional substring of the path+query, and fires `count` times (default 1), after skipping `skip` matches.

Bodies are relayed by Content-Length; a request with `Expect: 100-continue` is answered locally with 100 first.
Chunked request bodies are relayed by decoding the chunk framing (the AWS SDK sends Content-Length for these uploads).
The proxy keeps a log of every request it saw: method, path, status of the reply it got, bytes, and whether a rule fired.
"""
import json
import socket
import threading
import time


class Rule:
    def __init__(self, method, match='', action='lose_reply', count=1, skip=0, status=500, code='InternalError', delay=0.0):
        self.method, self.match, self.action = method, match, action
        self.remaining, self.skip, self.status, self.code, self.delay = count, skip, status, code, delay
        self.fired = 0

    def applies(self, method, target):
        if method != self.method or (self.match and self.match not in target):
            return False
        if self.skip > 0:
            self.skip -= 1
            return False
        return self.remaining > 0

    def to_json(self):
        return {'method': self.method, 'match': self.match, 'action': self.action, 'fired': self.fired}


def read_line(stream):
    line = b''
    while not line.endswith(b'\r\n'):
        chunk = stream.recv(1)
        if not chunk:
            return None
        line += chunk
    return line


def read_head(sock):
    """The request or response head as (first_line, [(name, value)], raw_bytes), or None on EOF."""
    first = read_line(sock)
    if first is None:
        return None
    raw, headers = first, []
    while True:
        line = read_line(sock)
        if line is None:
            return None
        raw += line
        if line == b'\r\n':
            return first.decode('latin-1').strip(), headers, raw
        name, _, value = line.decode('latin-1').partition(':')
        headers.append((name.strip(), value.strip()))


def header(headers, name, default=None):
    for key, value in headers:
        if key.lower() == name.lower():
            return value
    return default


def read_exact(sock, count):
    data = bytearray()
    while len(data) < count:
        chunk = sock.recv(min(1 << 20, count - len(data)))
        if not chunk:
            raise ConnectionError('short body')
        data += chunk
    return bytes(data)


def read_chunked(sock):
    """A chunked body with its framing intact, to be relayed byte for byte."""
    framed = bytearray()
    while True:
        line = read_line(sock)
        framed += line
        size = int(line.split(b';')[0].strip() or b'0', 16)
        if size == 0:
            while True:
                trailer = read_line(sock)
                framed += trailer
                if trailer in (b'\r\n', None):
                    return bytes(framed)
        framed += read_exact(sock, size + 2)


class Proxy:
    def __init__(self, upstream_host, upstream_port, listen_port=0):
        self.upstream = (upstream_host, upstream_port)
        self.rules = []
        self.log = []
        self.lock = threading.Lock()
        self.server = socket.socket()
        self.server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.server.bind(('127.0.0.1', listen_port))
        self.server.listen(64)
        self.port = self.server.getsockname()[1]
        self.stopping = False
        self.threads = []
        threading.Thread(target=self.accept, daemon=True).start()

    def set_rules(self, rules):
        with self.lock:
            self.rules = list(rules)

    def clear(self):
        with self.lock:
            self.rules, self.log = [], []

    def snapshot(self):
        with self.lock:
            return list(self.log), [r.to_json() for r in self.rules]

    def stop(self):
        self.stopping = True
        try:
            self.server.close()
        except OSError:
            pass

    def accept(self):
        while not self.stopping:
            try:
                client, _ = self.server.accept()
            except OSError:
                return
            thread = threading.Thread(target=self.serve, args=(client,), daemon=True)
            thread.start()

    def pick(self, method, target):
        with self.lock:
            for rule in self.rules:
                if rule.applies(method, target):
                    rule.remaining -= 1
                    rule.fired += 1
                    return rule
        return None

    def serve(self, client):
        upstream = None
        try:
            client.settimeout(120)
            while True:
                head = read_head(client)
                if head is None:
                    return
                first, headers, raw = head
                method, target, _ = first.split(' ', 2)
                length = header(headers, 'Content-Length')
                if (header(headers, 'Expect') or '').lower() == '100-continue':
                    client.sendall(b'HTTP/1.1 100 Continue\r\n\r\n')
                if (header(headers, 'Transfer-Encoding') or '').lower() == 'chunked':
                    body = read_chunked(client)
                else:
                    body = read_exact(client, int(length)) if length else b''
                rule = self.pick(method, target)
                watched = ('if-none-match', 'if-match', 'content-md5', 'x-amz-mp-object-size', 'x-amz-acl', 'x-amz-storage-class', 'content-type', 'x-amz-checksum-algorithm', 'x-amz-sdk-checksum-algorithm')
                entry = {'method': method, 'target': target[:200], 'bytes': len(body), 'rule': rule.action if rule else None, 'time': time.time(),
                         'headers': {k.lower(): v[:80] for k, v in headers if k.lower() in watched}}
                if rule and rule.action == 'drop':
                    self.record(entry)
                    return
                if rule and rule.action == 'status':
                    reply = ('<?xml version="1.0" encoding="UTF-8"?><Error><Code>%s</Code><Message>injected</Message></Error>' % rule.code).encode()
                    client.sendall(('HTTP/1.1 %d Injected\r\nContent-Type: application/xml\r\nContent-Length: %d\r\n\r\n' % (rule.status, len(reply))).encode() + reply)
                    entry['status'] = rule.status
                    self.record(entry)
                    continue
                if rule and rule.action == 'delay':
                    time.sleep(rule.delay)
                if rule and rule.action == 'tamper' and body:
                    mutable = bytearray(body)
                    mutable[len(mutable) // 2] ^= 1
                    body = bytes(mutable)
                if upstream is None:
                    upstream = socket.create_connection(self.upstream, timeout=120)
                upstream.sendall(raw.replace(b'Expect: 100-continue\r\n', b'').replace(b'expect: 100-continue\r\n', b'') + body)
                reply_head = read_head(upstream)
                if reply_head is None:
                    entry['status'] = None
                    self.record(entry)
                    return
                status_line, reply_headers, reply_raw = reply_head
                entry['status'] = int(status_line.split(' ')[1])
                reply_length = header(reply_headers, 'Content-Length')
                if method == 'HEAD' or entry['status'] in (204, 304) or entry['status'] // 100 == 1:
                    reply_body = b''
                elif (header(reply_headers, 'Transfer-Encoding') or '').lower() == 'chunked':
                    reply_body = None
                else:
                    reply_body = read_exact(upstream, int(reply_length)) if reply_length else b''
                if reply_body is None:
                    # Relay a chunked reply as it is: read to its end.
                    data = bytearray()
                    while True:
                        line = read_line(upstream)
                        data += line
                        size = int(line.split(b';')[0].strip(), 16)
                        data += read_exact(upstream, size + 2)
                        if size == 0:
                            break
                    reply_body = bytes(data)
                if rule and rule.action == 'lose_reply':
                    self.record(entry)
                    return
                client.sendall(reply_raw + reply_body)
                self.record(entry)
                if (header(reply_headers, 'Connection') or '').lower() == 'close':
                    return
        except (ConnectionError, OSError, ValueError):
            pass
        finally:
            for sock in (client, upstream):
                try:
                    if sock:
                        sock.close()
                except OSError:
                    pass

    def record(self, entry):
        with self.lock:
            self.log.append(entry)


if __name__ == '__main__':
    import sys
    proxy = Proxy('127.0.0.1', int(sys.argv[1]), int(sys.argv[2]) if len(sys.argv) > 2 else 0)
    print(proxy.port, flush=True)
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        pass
