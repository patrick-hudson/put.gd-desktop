"""Loopback update files and an allowlisted request ledger; no HTTP raw logs."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import threading
from urllib.parse import urlsplit


class Server(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, root):
        super().__init__(('127.0.0.1', 0), Handler)
        self.root = Path(root).resolve()
        self.ledger = []
        self.lock = threading.Lock()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        path = urlsplit(self.path).path
        if path == '/__ledger':
            with self.server.lock:
                body = json.dumps(self.server.ledger).encode()
            return self.reply(200, body)
        relative = Path(path.lstrip('/'))
        # Serve owned regular files only. Neither raw paths, queries, headers,
        # User-Agent nor browser arguments enter the ledger.
        file = self.server.root / relative
        valid = ('..' not in relative.parts and file.resolve().is_relative_to(self.server.root)
                 and not any(p.is_symlink() for p in (file, *file.parents)) and file.is_file())
        body = file.read_bytes() if valid else b''
        status = 200 if valid else 404
        route = 'feed' if path in ('/stable.json', '/nightly.json') else 'mirror' if path.startswith('/mirror/') else 'release' if path.startswith('/release/') else 'other'
        with self.server.lock:
            self.server.ledger.append(dict(sequence=len(self.server.ledger)+1, method='GET', route=route,
                                           channel=relative.stem if route == 'feed' else None,
                                           status=status, bytes=len(body)))
        self.reply(status, body)

    def reply(self, status, body):
        self.send_response(status)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass


if __name__ == '__main__':
    import sys
    server = Server(sys.argv[1])
    (server.root / 'port').write_text(str(server.server_port))
    server.serve_forever()
