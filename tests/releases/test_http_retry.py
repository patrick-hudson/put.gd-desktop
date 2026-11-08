#!/usr/bin/env python3
import http.client
import io
from pathlib import Path
import ssl
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
    def run_download(self, outcomes, clock=None):
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
                http_retry.download('https://example.invalid/tool', target, timeout=30, opener=opener, sleep=waits.append,
                                    clock=clock or (lambda: 0))
                return target.read_bytes(), waits, None
            except Exception as error:  # the test inspects what escaped
                return None, waits, error

    def test_transient_failures_are_retried_and_the_body_replaces_old_content(self):
        body, waits, error = self.run_download([status(504), TimeoutError(), status(503), b'tool'])
        self.assertIsNone(error)
        self.assertEqual(body, b'tool')
        self.assertEqual(waits, [http_retry.DELAY_SECONDS] * 3)

    def test_every_transient_error_type_is_retried(self):
        for failure in (urllib.error.URLError('down'), ConnectionResetError(), http.client.IncompleteRead(b''),
                        http.client.BadStatusLine('x'), http.client.RemoteDisconnected('gone')):
            with self.subTest(type(failure).__name__):
                body, waits, error = self.run_download([failure, b'tool'])
                self.assertIsNone(error)
                self.assertEqual((body, len(waits)), (b'tool', 1))

    def test_a_connection_lost_mid_body_is_retried_from_the_start(self):
        class Dying(io.BytesIO):
            sent = False

            def read1(self, *_args):
                if not self.sent:
                    self.sent = True
                    return b'partial'
                raise ConnectionResetError('died')
        with_partial = [Dying(b'x')]
        waits = []
        calls = []
        def opener(_request, timeout):
            calls.append(timeout)
            return with_partial.pop() if with_partial else Response(b'whole')
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / 'tool'
            http_retry.download('https://example.invalid/tool', target, timeout=30, opener=opener, sleep=waits.append)
            self.assertEqual(target.read_bytes(), b'whole')
        self.assertEqual((len(calls), len(waits)), (2, 1))

    def test_every_5xx_is_retried_and_ordinary_4xx_is_not(self):
        body, waits, error = self.run_download([status(520), status(599), b'tool'])
        self.assertIsNone(error)
        self.assertEqual((body, len(waits)), (b'tool', 2))
        _body, waits, error = self.run_download([status(403), b'never read'])
        self.assertEqual((error.code, waits), (403, []))

    def test_a_trickling_transfer_stops_at_its_limit_and_is_retried(self):
        now = [0]

        class Trickle:
            def __enter__(self): return self
            def __exit__(self, *_args): return False

            def read1(self, *_args):
                now[0] += http_retry.TRANSFER_SECONDS / 2
                return b'.'
        responses = [Trickle(), Response(b'whole')]
        clock = lambda: now[0]
        waits = []
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / 'tool'
            http_retry.download('https://example.invalid/tool', target, timeout=30, sleep=waits.append,
                                opener=lambda _request, timeout: responses.pop(0), clock=clock)
            self.assertEqual(target.read_bytes(), b'whole')
        self.assertEqual(len(waits), 1)

    def test_a_certificate_failure_fails_at_once(self):
        failure = urllib.error.URLError(ssl.SSLCertVerificationError('bad certificate'))
        _body, waits, error = self.run_download([failure, b'never read'])
        self.assertIs(error, failure)
        self.assertEqual(waits, [])

    def test_an_invalid_url_fails_at_once(self):
        failure = http.client.InvalidURL('bad')
        _body, waits, error = self.run_download([failure, b'never read'])
        self.assertIs(error, failure)
        self.assertEqual(waits, [])

    def test_no_attempt_starts_after_the_deadline(self):
        times = iter([0, http_retry.DEADLINE_SECONDS - 1, http_retry.DEADLINE_SECONDS])
        _body, waits, error = self.run_download([status(504), status(504), b'never read'], clock=lambda: next(times))
        self.assertEqual(error.code, 504)
        self.assertEqual(len(waits), 1)

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
