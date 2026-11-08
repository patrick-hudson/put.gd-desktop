#!/usr/bin/env python3
"""Download a build tool, riding out transient GitHub and CDN failures.

Callers verify the result against a pinned SHA-256, so retrying cannot change
what is accepted. Other HTTP errors, such as a 404, fail at once.
"""
import http.client
import shutil
import sys
import time
import urllib.error
import urllib.request

ATTEMPTS = 20
DELAY_SECONDS = 15
RETRYABLE_STATUS = {408, 425, 429, 500, 502, 503, 504}
TRANSIENT = (urllib.error.URLError, TimeoutError, ConnectionError, http.client.HTTPException)


def download(request, target, timeout, *, opener=urllib.request.urlopen, sleep=time.sleep):
    """Write the response body of a URL or Request to target, retrying transient failures."""
    for attempt in range(1, ATTEMPTS + 1):
        try:
            with opener(request, timeout=timeout) as response, open(target, 'wb') as output:
                shutil.copyfileobj(response, output)
            return
        except urllib.error.HTTPError as error:
            if error.code not in RETRYABLE_STATUS or attempt == ATTEMPTS:
                raise
            reason = f'HTTP {error.code}'
        except TRANSIENT as error:
            if attempt == ATTEMPTS:
                raise
            reason = type(error).__name__
        print(f'download attempt {attempt}/{ATTEMPTS} failed ({reason}); retrying in {DELAY_SECONDS} s', file=sys.stderr, flush=True)
        sleep(DELAY_SECONDS)
