#!/usr/bin/env python3
"""Loopback Range server for the zsync2 GitHub-CDN experiment (#296).

  rangeserver.py PORT ROOT MODE
MODE: multipart  - RFC 7233 multipart/byteranges for multi-range requests (like the #295 lab server, CloudFront)
      github     - 501 for a multi-range request, 206 for one range (like release-assets.githubusercontent.com)
      longredirect - /r/NAME answers 302 with a ~1500-byte Location (like github.com's release redirect),
                    then serves like multipart
Every request is logged to stderr: method, path, Range, status.
"""
import http.server, os, sys, re
PORT, ROOT, MODE = int(sys.argv[1]), sys.argv[2], sys.argv[3]

class H(http.server.BaseHTTPRequestHandler):
    def log(self, status):
        sys.stderr.write(f"{self.command} {self.path[:80]} range={self.headers.get('Range')} -> {status}\n"); sys.stderr.flush()
    def do_HEAD(self): self.serve(head=True)
    def do_GET(self): self.serve()
    def serve(self, head=False):
        path = self.path.split('?', 1)[0]
        if MODE == 'longredirect' and path.startswith('/r/'):
            target = '/' + path[3:] + '?' + 'x=' + 'a' * 1500
            self.send_response(302); self.send_header('Location', target); self.send_header('Content-Length', '0'); self.end_headers(); self.log(302); return
        name = os.path.join(ROOT, os.path.basename(path))
        if not os.path.isfile(name):
            self.send_response(404); self.send_header('Content-Length', '0'); self.end_headers(); self.log(404); return
        size = os.path.getsize(name); data = open(name, 'rb')
        rng = self.headers.get('Range')
        if not rng:
            self.send_response(200); self.send_header('Content-Length', str(size)); self.send_header('Accept-Ranges', 'bytes'); self.end_headers(); self.log(200)
            if not head: self.wfile.write(data.read())
            return
        parts = []
        for spec in rng.split('=', 1)[1].split(','):
            a, b = spec.strip().split('-'); a = int(a); b = min(int(b) if b else size - 1, size - 1); parts.append((a, b))
        if len(parts) > 1 and MODE == 'github':
            body = b'501 Unsupported client range\n'
            self.send_response(501); self.send_header('Content-Length', str(len(body))); self.end_headers(); self.log(501); self.wfile.write(body); return
        if len(parts) == 1:
            a, b = parts[0]; data.seek(a); chunk = data.read(b - a + 1)
            self.send_response(206); self.send_header('Content-Range', f'bytes {a}-{b}/{size}'); self.send_header('Content-Length', str(len(chunk))); self.end_headers(); self.log(206)
            if not head: self.wfile.write(chunk)
            return
        boundary = 'r12aiuboundary'; body = b''
        for a, b in parts:
            data.seek(a)
            body += (f'\r\n--{boundary}\r\nContent-Type: application/octet-stream\r\nContent-Range: bytes {a}-{b}/{size}\r\n\r\n').encode() + data.read(b - a + 1)
        body += f'\r\n--{boundary}--\r\n'.encode()
        self.send_response(206); self.send_header('Content-Type', f'multipart/byteranges; boundary={boundary}'); self.send_header('Content-Length', str(len(body))); self.end_headers(); self.log(206)
        if not head: self.wfile.write(body)
    def log_message(self, *a): pass

http.server.ThreadingHTTPServer(('127.0.0.1', PORT), H).serve_forever()
