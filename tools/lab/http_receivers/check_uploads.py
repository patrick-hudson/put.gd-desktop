#!/usr/bin/env python3
"""Drive a running put.gd resident against the real receivers through putgd-cli (#1361).

For each .sxcu file in --sxcu-dir (as receivers.py wrote them): import it as a destination
with settings.importUploader, read which values are stored, run Test connection, upload a
synthetic PNG and fetch its link, delete it from History when the receiver gave a
deletion URL, and finally check that no upload token is in any file under the resident's
configuration, data and state folders.

  python3 check_uploads.py --cli /usr/bin/putgd-cli --sxcu-dir ~/putgd-lab/sxcu \\
      [--socket PATH] [--credential-mode session|keychain] [--allow-insecure-http] [--keep]

The resident is the one putgd-cli reaches (the desktop's own, or a private one named by
--socket and the XDG_* variables of this process). The lab receivers speak plain HTTP on
the lab network: --allow-insecure-http says so for each imported destination, and
without it such a file is refused at import (http_insecure_url) and nothing of it is
stored. Session mode stores nothing in the desktop keyring; keychain mode leaves one
keyring entry per destination (putgd/http/rx-<name>), as put.gd does when a destination
is removed in Settings.
Destinations this run imported are removed again unless --keep is given, also when the
run stops early. The report on stdout has names, states and links, never a token. Exit 0
when every receiver passed.
"""
import argparse
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
import zlib


def synthetic_png(width=96, height=64):
    rows = b''.join(b'\x00' + bytes(channel for x in range(width) for channel in (x * 255 // width, y * 255 // height, (x * y) % 256)) for y in range(height))
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b'')


class Resident:
    def __init__(self, cli, socket):
        self.command = [cli] + (['--socket', socket] if socket else [])

    def call(self, method, params=None, timeout=180):
        # Parameters travel on stdin, as the MCP helper sends them.
        result = subprocess.run(self.command + ['--params-stdin', method], input=json.dumps(params or {}), capture_output=True, text=True, timeout=timeout)
        return json.loads(result.stdout)

    def revision(self):
        return self.call('settings.get')['result']['metadata']['observedRevision']

    def until(self, method, params, done, seconds=60):
        deadline = time.monotonic() + seconds
        while True:
            reply = self.call(method, params)
            if (reply['ok'] and done(reply['result'])) or time.monotonic() > deadline:
                return reply
            time.sleep(0.2)


def fetch(link):
    try:
        with urllib.request.urlopen(link, timeout=20) as reply:
            return reply.status, reply.read()
    except urllib.error.HTTPError as error:
        return error.code, b''
    except OSError as error:
        return 0, str(error).encode()


def tokens(path):
    """Every value of a .sxcu that put.gd stores as a secret: header, parameter and argument
    values that are not a bare placeholder, the Data body, and key-like runs in its addresses."""
    document = {key.lower(): value for key, value in json.loads(Path(path).read_text()).items()}
    found = []
    for section in ('headers', 'parameters', 'arguments'):
        for value in (document.get(section) or {}).values():
            if isinstance(value, str) and len(value) >= 8 and value.strip().lower() not in ('{filename}', '{input}', '$filename$', '$input$'):
                found.append(value)
    if isinstance(document.get('data'), str) and len(document['data']) >= 8:
        found.append(document['data'])
    for field in ('requesturl', 'deletionurl'):
        address = document.get(field) if isinstance(document.get(field), str) else ''
        path_start = address.find('/', address.find('://') + 3) if '://' in address else 0
        for run in re.findall(r'[A-Za-z0-9_-]{20,}', address[max(path_start, 0):]):
            if re.search(r'[0-9]', run) and re.search(r'[A-Za-z]', run):
                found.append(run)
    return found


def check(resident, sxcu, mode, payload, keep, allow_http, imported_profiles):
    name = Path(sxcu).stem
    profile = 'rx-' + name
    report = {'receiver': name, 'profileId': profile}
    document = resident.call('settings.get')['result']['document']
    if any(entry['id'] == profile for entry in document['profiles']):
        report['error'] = 'a destination named %s already exists; remove it first' % profile
        return report
    request = {'path': str(sxcu), 'expectedRevision': resident.revision(), 'profileId': profile, 'credentialMode': mode}
    if allow_http:
        request['allowInsecureHttp'] = True
    imported = resident.call('settings.importUploader', request)
    if not imported['ok']:
        report['error'] = 'import: ' + imported['error']['code'] + ': ' + imported['error']['message']
        return report
    # Only what this run created is removed afterwards.
    imported_profiles.append(profile)
    report['label'], report['storedValues'], report['notes'] = imported['result']['label'], imported['result']['storedValues'], imported['result']['notes']
    settings = next(entry for entry in resident.call('settings.get')['result']['document']['profiles'] if entry['id'] == profile)['settings']
    report['deletionUrl'], report['accepts'] = settings['deletionUrl'], settings['accepts']
    revision = resident.revision()
    hint = resident.call('setup.credentialHint', {'profileId': profile, 'expectedRevision': revision})['result']
    report['credentials'] = {'mode': hint.get('mode'), 'stored': hint.get('storedValues'), 'missing': hint.get('missingValues')}
    started = resident.call('setup.testDestination', {'profileId': profile, 'expectedRevision': revision})
    job = resident.until('setup.inspect', {'id': started['result'].get('setupId', '')}, lambda result: result['state'] != 'running', 90)['result'] if started['ok'] else {}
    report['test'] = {'state': job.get('state'), 'error': (job.get('error') or {}).get('code') or (started['error']['code'] if not started['ok'] else ''),
                      **{key: (job.get('result') or {}).get(key) for key in ('uploadStatus', 'publicReachable', 'cleanupStatus', 'linkReturnsFile')}}
    accepted = resident.call('upload.files', {'paths': [str(payload)], 'profileId': profile})
    if not accepted['ok']:
        report['error'] = 'upload: ' + accepted['error']['code'] + ': ' + accepted['error']['message']
        return report
    operation = accepted['result']['operationId']
    finished = resident.until('operation.inspect', {'id': operation}, lambda result: result['state'] in ('completed', 'partial', 'failed', 'cancelled'), 120)['result']
    record = next(entry for entry in resident.call('history.list', {'limit': 50})['result']['records'] if entry['operationId'] == operation)
    link = (record.get('receipt') or {}).get('directUrl', '')
    status, body = fetch(link) if link else (0, b'')
    report['upload'] = {'state': finished['state'], 'uploadStatus': record['uploadStatus'], 'error': (record.get('error') or {}).get('code', ''), 'link': link,
                        'linkStatus': status, 'linkReturnsFile': body == Path(payload).read_bytes(), 'historyId': record['id']}
    capabilities = resident.call('history.detail', {'id': record['id']})['result'].get('capabilities', {})
    report['deletable'] = bool(capabilities.get('delete'))
    if capabilities.get('delete') and not keep:
        prepared = resident.call('delete.prepare', {'ids': [record['id']]})
        if prepared['ok']:
            resident.call('delete.confirm', {'ids': [record['id']], 'acknowledgement': prepared['result']['acknowledgement']})
            removed = resident.until('history.detail', {'id': record['id']}, lambda result: result['remoteStatus'] not in ('present', 'not_requested', 'pending', 'deleting'), 60)['result']
            report['deletion'] = {'remoteStatus': removed['remoteStatus'], 'linkStatusAfter': fetch(link)[0]}
        else:
            report['deletion'] = {'error': prepared['error']['code']}
    elif not capabilities.get('delete'):
        refused = resident.call('delete.prepare', {'ids': [record['id']]})
        report['deletion'] = {'offered': False, 'refusal': refused['error']['code'], 'reason': capabilities.get('removeUnavailable', '')}
    report['passed'] = (report['test']['state'] == 'completed' and report['upload']['uploadStatus'] == 'uploaded' and status == 200 and
                        (not report['deletable'] or keep or report.get('deletion', {}).get('remoteStatus') == 'removed'))
    return report


def remove_profiles(resident, profiles):
    """Removes the named destinations; returns what went wrong, or None."""
    document = resident.call('settings.get')['result']['document']
    kept = [entry for entry in document['profiles'] if entry['id'] not in profiles]
    if len(kept) == len(document['profiles']):
        return None
    patched = resident.call('settings.patch', {'patch': {'profiles': kept}, 'expectedRevision': resident.revision()})
    return None if patched['ok'] else patched['error']['code'] + ': ' + patched['error']['message']


def files_with(root, needles):
    found = []
    for path in Path(root).rglob('*'):
        if path.is_file():
            try:
                data = path.read_bytes()
            except OSError:
                continue
            if any(needle.encode() in data for needle in needles):
                found.append(str(path))
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--cli', required=True)
    parser.add_argument('--sxcu-dir', required=True)
    parser.add_argument('--socket')
    parser.add_argument('--credential-mode', choices=('session', 'keychain'), default='session')
    parser.add_argument('--allow-insecure-http', action='store_true',
                        help='let each imported destination send its file and token over plain HTTP (the lab receivers need it)')
    parser.add_argument('--keep', action='store_true', help='leave the imported destinations and uploads in place')
    options = parser.parse_args()
    resident = Resident(options.cli, options.socket)
    files = sorted(Path(options.sxcu_dir).expanduser().glob('*.sxcu'))
    if not files:
        parser.error('no .sxcu files in ' + options.sxcu_dir)
    secrets = [value for path in files for value in tokens(path)]
    imported, reports, roots, leaked, packaged, cleanup = [], [], [], [], False, None
    try:
        with tempfile.TemporaryDirectory(prefix='putgd-rx-') as temporary:
            payload = Path(temporary) / 'putgd-receiver-check.png'
            payload.write_bytes(synthetic_png())
            for path in files:
                reports.append(check(resident, path, options.credential_mode, payload, options.keep, options.allow_insecure_http, imported))
        # A diagnostics package is made from the same resident; it is scanned with the rest.
        locations = resident.call('diagnostics')['result'].get('locations', {})
        roots = [entry['path'] for entry in locations.values() if isinstance(entry, dict) and entry.get('path')]
        state = os.environ.get('XDG_STATE_HOME') or str(Path.home() / '.local/state')
        roots.append(str(Path(state) / 'putgd'))
        with tempfile.TemporaryDirectory(prefix='putgd-rx-diag-') as temporary:
            package = resident.call('diagnostics.bundle', {'output': str(Path(temporary) / 'package.zip'), 'includeNames': True})
            leaked = files_with(temporary, secrets) + [path for root in roots for path in files_with(root, secrets)]
            packaged = package['ok']
    finally:
        # Also after a timeout or an unreadable reply: nothing this run imported stays behind.
        if not options.keep and imported:
            try:
                cleanup = remove_profiles(resident, set(imported))
            except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
                cleanup = type(error).__name__
            if cleanup:
                print('could not remove the imported destinations: ' + cleanup, file=sys.stderr)
    summary = {'receivers': reports, 'scannedRoots': roots, 'diagnosticsPackageScanned': packaged, 'filesHoldingAToken': leaked,
               'cleanupError': cleanup, 'passed': all(report.get('passed') for report in reports) and not leaked and packaged and not cleanup}
    if options.credential_mode == 'keychain' and imported:
        summary['keyringEntriesLeft'] = ['putgd/http/' + profile for profile in imported]
    for value in secrets:
        if value in json.dumps(summary):
            raise SystemExit('refusing to print a report that holds a token')
    print(json.dumps(summary, indent=2))
    return 0 if summary['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
