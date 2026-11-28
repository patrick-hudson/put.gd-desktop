"""The offline S3 fixture returns an object's Content-Type as S3 and R2 do (#2129).

Test upload and link's encrypted-page check needs the page served as text/html, so a fixture
that drops the type fails offline runs that pass on a real destination."""
from pathlib import Path
import http.client
import subprocess
import sys
import unittest

FIXTURE = Path(__file__).resolve().parents[2] / 'tests/fixtures/s3_server.py'


class S3FixtureContentType(unittest.TestCase):
    def setUp(self):
        self.server = subprocess.Popen([sys.executable, str(FIXTURE)], stdout=subprocess.PIPE, text=True)
        self.port = int(self.server.stdout.readline())

    def tearDown(self):
        self.server.terminate()
        self.server.wait(timeout=10)
        self.server.stdout.close()

    def send(self, method, key, body=b'', headers=None):
        connection = http.client.HTTPConnection('127.0.0.1', self.port, timeout=10)
        try:
            connection.request(method, '/b/' + key, body=body, headers=headers or {})
            reply = connection.getresponse()
            reply.read()
            return reply.status, reply.getheader('Content-Type')
        finally:
            connection.close()

    def test_stored_type_or_s3_default(self):
        self.assertEqual(self.send('PUT', 'page.html', b'<!doctype html>', {'Content-Type': 'text/html'})[0], 200)
        self.assertEqual(self.send('PUT', 'plain.bin', b'data')[0], 200)
        for method in ('GET', 'HEAD'):
            self.assertEqual(self.send(method, 'page.html'), (200, 'text/html'))
            self.assertEqual(self.send(method, 'plain.bin'), (200, 'binary/octet-stream'))


if __name__ == '__main__':
    unittest.main()
