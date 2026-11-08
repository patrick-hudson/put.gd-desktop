#!/usr/bin/env python3
import io
from pathlib import Path
import sys
import tempfile
import unittest
import urllib.error

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools' / 'releases'))
import http_retry  # noqa: E402


class Response(io.BytesIO):
    pass


def status(code):
    return urllib.error.HTTPError('https://example.invalid/tool', code, 'error', {}, None)


class Download(unittest.TestCase):
    def run_download(self, outcomes):
        waits = []
        def opener(_request, timeout):
            self.assertEqual(timeout, 30)
            outcome = outcomes.pop(0)
            if isinstance(outcome, Exception):
                raise outcome
            return Response(outcome)
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / 'tool'
            target.write_bytes(b'stale partial content')
            try:
                http_retry.download('https://example.invalid/tool', target, timeout=30, opener=opener, sleep=waits.append)
                return target.read_bytes(), waits, None
            except Exception as error:  # the test inspects what escaped
                return None, waits, error

    def test_transient_failures_are_retried_and_the_body_replaces_old_content(self):
        body, waits, error = self.run_download([status(504), TimeoutError(), status(503), b'tool'])
        self.assertIsNone(error)
        self.assertEqual(body, b'tool')
        self.assertEqual(waits, [http_retry.DELAY_SECONDS] * 3)

    def test_a_missing_asset_fails_at_once(self):
        _body, waits, error = self.run_download([status(404), b'never read'])
        self.assertEqual(error.code, 404)
        self.assertEqual(waits, [])

    def test_the_last_failure_escapes_after_every_attempt(self):
        _body, waits, error = self.run_download([status(504) for _ in range(http_retry.ATTEMPTS)])
        self.assertEqual(error.code, 504)
        self.assertEqual(len(waits), http_retry.ATTEMPTS - 1)


if __name__ == '__main__':
    unittest.main()
