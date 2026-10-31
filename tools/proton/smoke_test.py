#!/usr/bin/env python3
"""Check a real compiled helper's stdio boundary without keychain or network access."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', type=Path, required=True)
    args = parser.parse_args()
    runtime = str(args.runtime.resolve())
    version = subprocess.run([runtime, '--version'], capture_output=True, check=True, timeout=10)
    identity = json.loads(version.stdout)
    assert not version.stderr and identity['runtime'] == 'putgd-proton-runtime'
    assert identity['appVersion'] == 'external-drive-put_gd@0.1.1-alpha'
    assert identity['bunVersion'] == '1.4.4' and identity['sdkVersion'] == 'js@0.22.2'
    destination = {'providerId': 'proton-drive', 'profileId': 'smoke', 'profileRevision': 'smoke',
                   'settings': {}, 'credentials': {'mode': 'keychain', 'reference': 'smoke'}}
    request = {'schemaVersion': 1, 'method': 'remove', 'destination': destination}
    cases = [(json.dumps(request).encode(), 'unsupported_safe_removal'),
             (json.dumps({**request, 'requestTimeoutMs': 3600000}).encode(), 'unsupported_safe_removal'),
             (json.dumps({**request, 'requestTimeoutMs': 86400000}).encode(), 'unsupported_safe_removal'),
             (json.dumps({**request, 'requestTimeoutMs': 86400001}).encode(), 'invalid_request'),
             (json.dumps({**request, 'requestTimeoutMs': 999}).encode(), 'invalid_request'),
             (b'{}{}', 'invalid_request'), (b'\xff', 'invalid_request'),
             (b'x' * (128 * 1024 + 1), 'invalid_request'),
             (json.dumps({**request, 'destination': {**destination, 'credentials': {'mode': 'unsafe_file', 'reference': 'smoke'}}}).encode(), 'keychain_required'),
             (json.dumps({**request, 'destination': {**destination, 'credentials': {'mode': 'keychain', 'reference': 'smoke', 'password': 'synthetic-secret'}}}).encode(), 'keychain_required')]
    # No case invokes keychain or account initialization; none creates Unix sockets.
    for payload, expected in cases:
        completed = subprocess.run([runtime, '--stdio'], input=payload, capture_output=True, check=True, timeout=10)
        lines = completed.stdout.splitlines()
        assert not completed.stderr and len(lines) == 1
        response = json.loads(lines[0])
        assert response['ok'] is False and response['error']['code'] == expected
        assert response['error']['details'] == {} and b'synthetic-secret' not in completed.stdout
    with tempfile.TemporaryDirectory(prefix='putgd-pr-') as private:
        env = {**os.environ, 'PUTGD_PROTON_DATA_DIR': private}
        payload = {**request, 'method': 'storeSharePassword', 'options': {'reference': 'smoke', 'password': 'synthetic-secret'}}
        completed = subprocess.run([runtime, '--stdio'], input=json.dumps(payload).encode(), env=env,
                                   capture_output=True, check=True, timeout=10)
        assert not completed.stderr and json.loads(completed.stdout)['error']['code'] == 'reference_in_use'
        assert b'synthetic-secret' not in completed.stdout
    print(f'Compiled helper version and {len(cases)+1} no-account stdio cases passed.')

if __name__ == '__main__':
    main()
