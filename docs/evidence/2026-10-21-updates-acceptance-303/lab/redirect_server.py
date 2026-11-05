"""range_server.py from the #293 evidence plus one 302: /appimage.zsync -> a release-style path,
as app.put.gd's /dl/<channel>/appimage.zsync permalink redirects to the GitHub release asset."""
import os, sys
source = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'range_server.py')).read()
body, _, last = source.rstrip().rpartition('\n')
assert last.startswith('http.server.ThreadingHTTPServer')
exec(compile(body, 'range_server.py', 'exec'))
TARGET = os.environ['REDIRECT_TO']
class Redirecting(Handler):
    def _redirect(self):
        if self.path == '/appimage.zsync':
            self.send_response(302); self.send_header('Location', TARGET); self.send_header('Content-Length', '0'); self.end_headers(); return True
        return False
    def do_GET(self):
        if not self._redirect(): super().do_GET()
    def do_HEAD(self):
        if not self._redirect(): super().do_HEAD()
http.server.ThreadingHTTPServer(('127.0.0.1', int(sys.argv[2])), Redirecting).serve_forever()
