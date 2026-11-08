#!/usr/bin/env python3
"""Download a build tool, riding out transient GitHub and CDN failures.

Callers verify the result against a pinned SHA-256, so retrying cannot change
what is accepted. Permanent failures, such as a 404 or a bad certificate, fail
at once. No new attempt starts after DEADLINE_SECONDS, which keeps a long outage
inside the calling job's timeout.
"""
import http.client
import shutil
import ssl
import sys
import time
import urllib.error
import urllib.request

ATTEMPTS = 20
DELAY_SECONDS = 15
DEADLINE_SECONDS = 420
RETRYABLE_STATUS = {408, 425, 429, 500, 502, 503, 504}
TRANSIENT = (urllib.error.URLError, TimeoutError, ConnectionError, http.client.IncompleteRead, http.client.BadStatusLine)


def _permanent(error):
    return isinstance(getattr(error, 'reason', None), ssl.SSLCertVerificationError)


def download(request, target, timeout, *, opener=urllib.request.urlopen, sleep=time.sleep, clock=time.monotonic):
    """Write the response body of a URL or Request to target, retrying transient failures."""
    give_up = clock() + DEADLINE_SECONDS
    for attempt in range(1, ATTEMPTS + 1):
        try:
            with opener(request, timeout=timeout) as response, open(target, 'wb') as output:
                shutil.copyfileobj(response, output)
            return
        except urllib.error.HTTPError as error:
            if error.code not in RETRYABLE_STATUS or attempt == ATTEMPTS or clock() >= give_up:
                raise
            reason = f'HTTP {error.code}'
        except TRANSIENT as error:
            if _permanent(error) or attempt == ATTEMPTS or clock() >= give_up:
                raise
            reason = type(error).__name__
        print(f'download attempt {attempt}/{ATTEMPTS} failed ({reason}); retrying in {DELAY_SECONDS} s', file=sys.stderr, flush=True)
        sleep(DELAY_SECONDS)


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit('usage: http_retry.py URL OUTPUT')
    download(sys.argv[1], sys.argv[2], timeout=120)
