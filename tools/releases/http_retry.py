#!/usr/bin/env python3
"""Download a build tool, riding out transient GitHub and CDN failures.

Callers verify the result against a pinned SHA-256, so retrying cannot change
what is accepted. Permanent failures, such as a 404 or a bad certificate, fail
at once. No new attempt starts after DEADLINE_SECONDS and no transfer runs longer
than TRANSFER_SECONDS, which keeps a long outage inside the calling job's timeout.
"""
import http.client
import ssl
import sys
import time
import urllib.error
import urllib.request

ATTEMPTS = 20
DELAY_SECONDS = 15
DEADLINE_SECONDS = 420
TRANSFER_SECONDS = 300
CHUNK_BYTES = 1 << 20
RETRYABLE_4XX = {408, 425, 429}
TRANSIENT = (urllib.error.URLError, TimeoutError, ConnectionError, http.client.IncompleteRead, http.client.BadStatusLine)


def _permanent(error):
    return isinstance(getattr(error, 'reason', None), ssl.SSLCertVerificationError)


def _retryable_status(code):
    return code in RETRYABLE_4XX or 500 <= code < 600


def _copy(response, output, clock):
    """Copy one response, stopping a transfer that outlasts its limit or ends early."""
    limit = clock() + TRANSFER_SECONDS
    length = getattr(response, 'headers', {}).get('Content-Length', '')
    received = 0
    while True:
        # read1 returns after one receive, so the limit is checked between small reads.
        chunk = response.read1(CHUNK_BYTES)
        if clock() >= limit:
            raise TimeoutError(f'transfer exceeded {TRANSFER_SECONDS} s')
        if not chunk:
            break
        output.write(chunk)
        received += len(chunk)
    # read1 reports a connection closed early as a normal end of the body.
    if length.isdigit() and received < int(length):
        raise http.client.IncompleteRead(b'', int(length) - received)


def download(request, target, timeout, *, opener=urllib.request.urlopen, sleep=time.sleep, clock=time.monotonic):
    """Write the response body of a URL or Request to target, retrying transient failures."""
    give_up = clock() + DEADLINE_SECONDS
    for attempt in range(1, ATTEMPTS + 1):
        try:
            with opener(request, timeout=timeout) as response, open(target, 'wb') as output:
                _copy(response, output, clock)
            return
        except urllib.error.HTTPError as error:
            if not _retryable_status(error.code) or attempt == ATTEMPTS or clock() >= give_up:
                raise
            reason = f'HTTP {error.code}'
            last_error = error
        except TRANSIENT as error:
            if _permanent(error) or attempt == ATTEMPTS or clock() >= give_up:
                raise
            reason = type(error).__name__
            last_error = error
        print(f'download attempt {attempt}/{ATTEMPTS} failed ({reason}); retrying in {DELAY_SECONDS} s', file=sys.stderr, flush=True)
        sleep(DELAY_SECONDS)
        if clock() >= give_up:
            raise last_error


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit('usage: http_retry.py URL OUTPUT')
    download(sys.argv[1], sys.argv[2], timeout=120)
