#!/usr/bin/env python3
"""Static file server with HTTP Range support (single and multipart), as GitHub's
release CDN offers; logs each request and the bytes it sent. Lab use only.
usage: range_server.py DIRECTORY PORT"""
import http.server
import os
import re
import sys
import uuid

ROOT = os.path.abspath(sys.argv[1])


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=ROOT, **kwargs)

    def log_message(self, fmt, *args):
        sys.stderr.write('%s %s\n' % (self.log_date_time_string(), fmt % args))
        sys.stderr.flush()

    def do_GET(self):
        header = self.headers.get('Range')
        path = self.translate_path(self.path)
        if not header or not os.path.isfile(path):
            return super().do_GET()
        size = os.path.getsize(path)
        ranges = []
        for part in header.removeprefix('bytes=').split(','):
            first, last = re.fullmatch(r'\s*(\d*)-(\d*)\s*', part).groups()
            if first == '':
                first, last = size - int(last), size - 1
            else:
                first, last = int(first), min(int(last) if last else size - 1, size - 1)
            ranges.append((first, last))
        sent = 0
        with open(path, 'rb') as stream:
            def chunk(first, last):
                stream.seek(first)
                return stream.read(last - first + 1)
            if len(ranges) == 1:
                first, last = ranges[0]
                body = chunk(first, last)
                self.send_response(206)
                self.send_header('Content-Range', f'bytes {first}-{last}/{size}')
                self.send_header('Content-Type', 'application/octet-stream')
            else:
                boundary = uuid.uuid4().hex
                body = b''
                for first, last in ranges:
                    body += (f'--{boundary}\r\nContent-Type: application/octet-stream\r\n'
                             f'Content-Range: bytes {first}-{last}/{size}\r\n\r\n').encode()
                    body += chunk(first, last) + b'\r\n'
                body += f'--{boundary}--\r\n'.encode()
                self.send_response(206)
                self.send_header('Content-Type', 'multipart/byteranges; boundary=' + boundary)
            sent = sum(last - first + 1 for first, last in ranges)
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Accept-Ranges', 'bytes')
            self.end_headers()
            self.wfile.write(body)
        self.log_message('  %d range(s), %d file bytes', len(ranges), sent)

    def end_headers(self):
        if 'Range' not in self.headers:
            self.send_header('Accept-Ranges', 'bytes')
        super().end_headers()


http.server.ThreadingHTTPServer(('127.0.0.1', int(sys.argv[2])), Handler).serve_forever()
