#!/usr/bin/env python3
"""Live Linux tray-progress replay on an already provisioned disposable VM.

Secrets, package installation, console unlocking and VM lifecycle belong to the
operator. This runner retains protocol assertions and QMP rendering witnesses
separately; exit zero is never a human graphical-acceptance receipt.
"""
import argparse
from decimal import Decimal
from urllib.parse import urlsplit
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import time
import uuid

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('putgd_tray_lab', HERE / 'lab.py')
lab = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lab)
TERMINAL = {'completed', 'partial', 'failed', 'cancelled', 'interrupted'}
IDLE = 'put.gd — Share and local capture'
LABELS = {'preflight': 'Checking destination', 'saving_local': 'Saving local copy',
          'sharing': 'Creating share link', 'reconciling': 'Checking previous upload'}


class TrayProgressError(RuntimeError):
    pass


class ObservationMismatch(TrayProgressError):
    """Only asynchronous production/SNI correlation may receive batching grace."""


def observed(condition, message):
    if not condition:
        raise ObservationMismatch(message)


def require(condition, message):
    if not condition:
        raise TrayProgressError(message)


def socket_paths(guest_root, qmp_socket, native_paths=()):
    """Read-only preflight, before mkdir or any process launch."""
    paths = [Path(guest_root).resolve() / 'r/putgd/session.sock',
             Path(qmp_socket).resolve(), *(Path(p).resolve() for p in native_paths)]
    result = {str(path): {'utf8Bytes': len(str(path).encode('utf-8'))} for path in paths}
    require(all(p['utf8Bytes'] <= 107 for p in result.values()),
            'Resolved Unix socket exceeds 107 UTF-8 bytes: ' + json.dumps(result) +
            '; preserve this attempt and choose a short new run path')
    return result


def byte_summary(value):
    unit = 1024**3 if value >= 1024**3 else 1024**2
    hundredths = (value % unit * 100 + unit // 2) // unit
    whole = value // unit
    if value > 0 and not whole and not hundredths:
        return '<0.01 MiB'
    if hundredths == 100:
        whole, hundredths = whole + 1, 0
    fraction = ('.' + str(hundredths).zfill(2).rstrip('0')) if hundredths else ''
    return str(whole) + fraction + (' GiB' if unit == 1024**3 else ' MiB')


def displayed_interval(text):
    """An integer-byte range consistent with the tooltip's rounded byte summary."""
    if text == '<0.01 MiB':
        return 1, (1024**2 + 199) // 200 - 1
    number, suffix = text.split()
    unit = 1024**3 if suffix == 'GiB' else 1024**2
    center = Decimal(number) * unit
    half = Decimal(unit) / 200
    # Include boundary integer bytes; percentage is checked against this same range.
    return max(0, int(center - half)), int(center + half)


SIZE = r'(?:<0\.01|[0-9]+(?:\.[0-9]{1,2})?) (?:MiB|GiB)'
COUNTER = re.compile(r'^(.+?): (' + SIZE + r')(?: of (' + SIZE + r')(?: \(([0-9]+)%\))?| transferred · Source size: (' + SIZE + r'))$')


def check_tooltip(snapshot, records, provider, pid):
    """Compare SNI bytes/phase with recent independent production CLI observations.

    The resident batches observations, and CLI and DBus calls are not atomic.
    Records bracket the SNI read and cover the previous two seconds. Displayed
    counters must fit a real observed range and their own rounded percentage;
    Proton's SDK counter is allowed to decrease. No time/size percentage estimate.
    """
    require(snapshot.get('pid') == pid and snapshot.get('hostRegistered') and snapshot.get('registered'),
            'SNI must be registered with the native host and owned by the expected app PID')
    text = snapshot.get('description', '')
    require(not re.search(r'https?://|file://|synthetic-[12]\.txt|/tmp/|/home/', text, re.IGNORECASE),
            'Tray tooltip exposes a source name, path or link')
    idle = snapshot.get('title') == IDLE
    require(idle or snapshot.get('title') == 'put.gd — Working', 'Unexpected tray title')
    color = '#9bbcff' if idle else '#62d69b'
    icons = snapshot.get('icons', [])
    require(icons and all(icon.get('opaqueColors', {}).get(color, 0) > 0 for icon in icons),
            'SNI icon does not have the required idle/active brand color')
    if idle:
        observed(not text, 'Idle tooltip retains stale work')
        observed(not records, 'Idle tray while production work remains active')
        return {'idle': True, 'counter': False, 'phases': []}
    observed(text, 'Working tray has no activity text')
    labels = {LABELS.get(record['phase'], record['phase'].replace('_', ' ').capitalize()): record['phase']
              for record in records}
    result = {'idle': False, 'counter': False, 'phases': []}
    for row_index, line in enumerate(text.splitlines()):
        overflow = re.fullmatch(r'([0-9]+) more active operations', line)
        if overflow:
            observed(any(record.get('activeCount') == int(overflow[1]) + 3 for record in records),
                    'Tray overflow count differs from observed active operations')
            continue
        match = COUNTER.fullmatch(line)
        prefix = match[1] if match else line.split(' · Source size: ', 1)[0]
        label = re.sub(r' file [0-9]+/[0-9]+$', '', prefix)
        observed(label in labels, 'Tray phase has no matching production observation')
        phase = labels[label]
        result['phases'].append(phase)
        applicable = [record for record in records if record['phase'] == phase and record.get('rowIndex', 0) == row_index]
        ordinal = re.search(r' file ([0-9]+)/([0-9]+)$', prefix)
        if ordinal:
            applicable = [record for record in applicable if record.get('fileIndex') == int(ordinal[1])
                          and record.get('fileCount') == int(ordinal[2])]
        observed(applicable, 'Tray row has no matching ordered production observation')
        if not match:
            source = line.split(' · Source size: ', 1)
            observed(len(source) == 1 or any(source[1] == byte_summary(record['sourceBytes']) for record in applicable),
                    'Tray source size differs from acquired artifact bytes')
            require(':' not in prefix and '%' not in line, 'Unrecognized tray counter')
            continue
        lower, upper = displayed_interval(match[2])
        numeric = [record for record in applicable if isinstance(record.get('bytesTransferred'), int)]
        observed(numeric, 'Tray reports transferred bytes without a production counter')
        low = max(lower, min(record['bytesTransferred'] for record in numeric))
        high = min(upper, max(record['bytesTransferred'] for record in numeric))
        observed(low <= high, 'Tray transferred bytes lie outside production observations')
        if provider == 'proton-drive':
            require(match[5] and not match[3] and not match[4], 'Proton must show encrypted bytes separately, without percentage')
            observed(any(match[5] == byte_summary(record['sourceBytes']) for record in numeric),
                    'Proton source size differs from acquired artifact bytes')
            require(all('bytesTotal' not in record for record in numeric), 'Unexpected Proton transfer-total accounting')
        else:
            totals = {record['bytesTotal'] for record in numeric if record.get('bytesTotal', 0) > 0
                      and byte_summary(record['bytesTotal']) == match[3]}
            observed(totals and match[4] is not None, 'S3 needs a real matching transfer total and percentage')
            percent = int(match[4])
            observed(any(low <= total and low * 100 // total <= percent <= min(high, total) * 100 // total
                        for total in totals), 'Tray percentage contradicts its displayed transfer bytes')
        result['counter'] = True
    return result


def records_from(operations):
    active = sorted((op for op in operations if op.get('state') not in TERMINAL),
                    key=lambda op: (op.get('created', ''), op['id']))
    records = []
    for row_index, op in enumerate(active):
        phase = op.get('phase', op['state'])
        items = op.get('items', [])
        index = next((i for i, item in enumerate(items) if item.get('uploadStatus') == 'uploading'),
                     next((i for i, item in enumerate(items) if item.get('phase') == phase), 0))
        item = items[index] if items else {}
        progress = item.get('uploadProgress') or {}
        records.append({'operationId': op['id'], 'artifactId': item.get('artifact', {}).get('id'),
                        'state': op['state'], 'phase': phase, 'fileIndex': index + 1, 'fileCount': len(items),
                        'sourceBytes': item.get('artifact', {}).get('bytes', 0),
                        'activeCount': len(active), 'rowIndex': row_index, **{key: progress[key] for key in
                         ('bytesTransferred', 'bytesTotal') if key in progress}})
    return records



def correlated_records(history, current):
    """Only the current operation/artifact may supply a row's batched counter.

    An operation moving up a row retains its history. A completed predecessor
    or previous file never supplies bounds for the new owner of that row.
    """
    result = []
    for owner in current:
        for row in history:
            if (row['operationId'], row.get('artifactId')) == (owner['operationId'], owner.get('artifactId')):
                result.append({**row, 'rowIndex': owner['rowIndex'], 'activeCount': owner['activeCount']})
    return result


def concurrent_admission(checked, records):
    running = [row for row in records if row.get('state') == 'running' and row['phase'] != 'queued']
    return len(running) >= 2 and len([phase for phase in checked['phases'] if phase != 'queued']) >= 2

def check_build_manifests(manifests, expected_commit):
    require(manifests and all(manifest.get('gitCommit') == expected_commit for manifest in manifests),
            'App/CLI do not have the expected source commit')
    require(all(manifest == manifests[0] for manifest in manifests), 'App/CLI embedded build manifests differ')
    require(all(manifest.get('dirty') is False or
                (manifest.get('dirty') is None and manifest.get('sourceProvenance') == 'supplied')
                for manifest in manifests), 'Dirty or unknown-source app/CLI identity')


def private_settings(path, profile_id, provider, marker):
    patch = lab.read_json(path)
    require(isinstance(patch, dict), '--settings must be a nonsecret settings patch')
    profiles = patch.get('profiles', [])
    require(isinstance(profiles, list) and len(profiles) == 1,
            'Replay settings must contain exactly one isolated profile')
    selected = [p for p in profiles if p.get('id') == profile_id and p.get('provider') == provider]
    require(len(selected) == 1, 'Selected provider/profile absent or not unique in settings patch')
    profile = selected[0]
    credential = profile.get('credentials', {})
    require(set(credential) == {'mode', 'reference'} and credential['mode'] == 'keychain'
            and isinstance(credential['reference'], str) and credential['reference'],
            'The scenario requires a pre-provisioned keychain reference')
    # Only provider-neutral settings and the selected profile are used. Reject
    # credential values before serializing a patch into any guest command.
    forbidden = {'password', 'accesskey', 'accesskeyid', 'secretkey', 'secretaccesskey',
                 'sessiontoken', 'accesstoken', 'refreshtoken', 'userkeypassword',
                 'cachepassword', 'snapshot', 'token', 'authorization', 'session'}
    def contains_secret(value):
        if isinstance(value, dict):
            names = {re.sub('[^a-z]', '', str(key).lower()) for key in value}
            return bool(names & forbidden) or any(contains_secret(child) for child in value.values())
        if isinstance(value, list):
            return any(contains_secret(child) for child in value)
        if isinstance(value, str) and value.casefold().startswith(('http://', 'https://')):
            try:
                url = urlsplit(value)
                return bool(url.username is not None or url.password is not None or url.query or url.fragment)
            except ValueError:
                return True
        return False
    require(not contains_secret(patch), 'Settings contain a credential value field')
    settings = profile.get('settings', {})
    scope = settings.get('objectPrefix' if provider == 's3' else 'folderName', '')
    root_only = provider == 's3' and scope == ''
    if root_only:
        naming = patch.get('naming', {})
        template = marker + '-{uuid}.{ext}'
        require(marker and naming.get('mode') == 'template' and naming.get('collisionPolicy') == 'rename'
                and all(naming.get(key) == template for key in
                        ('fileTemplate', 'captureTemplate', 'imageTemplate', 'textTemplate')),
                'Root-only S3 needs exact isolated UUID templates for every source kind')
    else:
        require(marker and marker in scope, 'Selected destination lacks the operator-provisioned unique isolation marker')
    require(provider != 'proton-drive' or settings.get('folderUid'), 'Proton isolated folder UID missing')
    return patch


class Replay:
    def __init__(self, args):
        self.args = args
        self.run = Path(args.run).resolve(strict=True)
        self.output = Path(args.output).resolve()
        self.output.mkdir(mode=0o700, exist_ok=False)
        os.chmod(self.output, 0o700)
        self.info = lab.state(self.run)
        self.profile = lab.load_profile(self.info['profile'])
        self.token = uuid.uuid4().hex[:12]
        self.guest = '/tmp/tp-' + self.token
        self.ids = []
        self.pid = None
        self.history = []
        self.mismatch_since = None
        self.witness_number = 0
        self.result = {'schemaVersion': 1, 'protocolPassed': False,
                       'graphicalAcceptance': 'pending-human-inspection',
                       'profile': self.info['profile'], 'provider': args.provider,
                       'operationIds': self.ids, 'guestRoot': self.guest,
                       'vncPort': 5900 + self.info['vnc_display'],
                       'coverage': {}, 'witnesses': [],
                       'remoteCleanup': 'not-performed; exact preparation only; operator owns acknowledgement'}
        self.save('parameters.json', {'expectedCommit': args.expected_commit,
            'expectedPackageSha256': args.expected_package_sha256,
            'expectedProbeSha256': args.expected_probe_sha256,
            'profileId': args.profile_id, 'isolationMarker': args.isolation_marker,
            'payloadMiBPerFile': args.payload_mib, 'fileCount': 2,
            'trayPosition': [args.tray_x, args.tray_y], 'timeoutSeconds': args.timeout,
            'guestRoot': self.guest, 'controllerSha256': lab.hash_file(__file__),
            'arguments': vars(args)})

    def save(self, name, value):
        lab.write_json(self.output / name, value)

    def session(self, body):
        # Preserve a relative Wayland socket before switching the app's private
        # runtime root. The real desktop's bus and display remain in use.
        prefix = 'case "${WAYLAND_DISPLAY:-}" in ""|/*) ;; *) export WAYLAND_DISPLAY="$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY";; esac\n'
        prefix += '\n'.join('export ' + name + '=' + shlex.quote(self.guest + suffix) for name, suffix in
                           [('XDG_CONFIG_HOME', '/c'), ('XDG_DATA_HOME', '/d'), ('XDG_RUNTIME_DIR', '/r')]) + '\n'
        return lab.guest_session(self.run, prefix + body)

    def cli(self, method, params=None, expected_error=None):
        value = json.loads(self.session('/usr/bin/putgd-cli ' + shlex.quote(method) + ' ' +
                                        shlex.quote(json.dumps(params or {})) + ' || test "$?" -eq 1\n'))
        if expected_error:
            require(not value.get('ok') and value.get('error', {}).get('code') == expected_error,
                    'Expected production refusal was not observed: ' + expected_error)
            return {'errorCode': expected_error}
        require(value.get('ok'), 'Production CLI failed: ' + value.get('error', {}).get('code', 'unknown'))
        return value['result']

    def patch(self, patch):
        before = self.cli('settings.get')
        self.cli('settings.patch', {'expectedRevision': before['metadata']['observedRevision'], 'patch': patch})
        after = self.cli('settings.get')
        require(after['metadata']['validation']['ok'], 'Settings patch not accepted')
        self.result['acceptedSettingsRevision'] = after['metadata']['acceptedRevision']

    def activate_credentials(self):
        if self.args.provider != 's3':
            return
        metadata = self.cli('settings.get')['metadata']
        accepted = self.cli('setup.unlockCredentials', {'profileId': self.args.profile_id,
                             'expectedRevision': metadata['observedRevision']})
        setup_id = accepted['setupId']
        self.result['credentialSetupId'] = setup_id
        self.save('results.json', self.result)
        def finished():
            job = self.cli('setup.inspect', {'id': setup_id})
            return job if job['state'] != 'running' else None
        job = self.wait(finished, 'Explicit S3 credential unlock did not settle')
        require(job['state'] == 'completed', 'Explicit S3 credential unlock failed: ' +
                job.get('error', {}).get('code', 'unknown'))

    def get_json(self, remote):
        return json.loads(lab.ssh(self.run, 'cat -- ' + shlex.quote(remote) + '\n'))

    def wait(self, predicate, message):
        deadline = time.monotonic() + self.args.timeout
        while time.monotonic() < deadline:
            value = predicate()
            if value:
                return value
            time.sleep(.1)
        raise TrayProgressError(message)

    def witness(self, label, hover=True):
        self.witness_number += 1
        if hover:
            lab.pointer(self.run, self.args.tray_x, self.args.tray_y, self.width, self.height)
            time.sleep(.8)
        pictures = []
        for head in range(self.profile.get('display_heads', 1)):
            value = lab.screenshot(self.run, f'tp-{self.token}-{self.witness_number}-{label}', head)
            name = f'{self.witness_number:03}-{label}-head{head}.ppm'
            shutil.copyfile(value['path'], self.output / name)
            pictures.append({'file': name, 'sha256': lab.hash_file(self.output / name), 'head': head})
        self.result['witnesses'].append({'label': label, 'hover': hover,
            'rendering': 'requires-human-inspection', 'pictures': pictures})
        self.save('results.json', self.result)
        return self.output / pictures[0]['file']

    def snapshot(self):
        value = json.loads(self.session(shlex.quote(self.args.probe) + ' --pid ' + str(self.pid) + ' || test "$?" -eq 1\n'))
        require(value.get('ok'), 'Real tray probe failed: ' + value.get('error', 'unknown'))
        return value

    def observe(self, label):
        before = self.cli('operation.recent')['operations']
        snapshot = self.snapshot()
        after = self.cli('operation.recent')['operations']
        now = time.monotonic()
        before_rows, after_rows = records_from(before), records_from(after)
        self.history.append((now, before_rows + after_rows))
        self.history = [(stamp, rows) for stamp, rows in self.history if now - stamp <= 2]
        recent = correlated_records([row for _, rows in self.history for row in rows], after_rows)
        try:
            checked = {**check_tooltip(snapshot, recent, self.args.provider, self.pid), 'matched': True}
            self.mismatch_since = None
        except ObservationMismatch as error:
            # Separate DBus/CLI reads may straddle a queued-row, file or phase
            # transition. Preserve the sample, grant no coverage, and bound the
            # entire consecutive mismatch period; persistent stale/idle fails.
            checked = {'idle': False, 'counter': False, 'phases': [], 'matched': False, 'error': str(error)}
            if self.mismatch_since is None:
                self.mismatch_since = now
        except TrayProgressError as error:
            # A privacy/identity/protocol violation is fatal, never a batching
            # race. Retain only bounded redacted evidence, not rejected text.
            with (self.output / 'observations.jsonl').open('a') as stream:
                stream.write(json.dumps({'monotonicSeconds': now, 'scenario': label,
                    'sni': {'redacted': True}, 'cliBefore': before_rows, 'cliAfter': after_rows,
                    'assertions': {'matched': False, 'fatal': True, 'error': str(error)}}) + '\n')
            raise
        entry = {'monotonicSeconds': now, 'scenario': label, 'sni': snapshot,
                 'cliBefore': before_rows, 'cliAfter': after_rows, 'assertions': checked}
        with (self.output / 'observations.jsonl').open('a') as stream:
            stream.write(json.dumps(entry) + '\n')
        if not checked['matched']:
            require(now - self.mismatch_since <= 2, 'Tray mismatch persisted beyond batching grace: ' + checked['error'])
            return after, snapshot, checked
        flags = self.result['coverage'].setdefault(label, {'active': False, 'counter': False,
                  'queued': False, 'concurrentAdmission': False, 'finalizing': False, 'sharing': False})
        new = []
        for key, value in [('active', not checked['idle']), ('counter', checked['counter']),
                           ('queued', 'queued' in checked['phases']),
                           ('concurrentAdmission', concurrent_admission(checked, after_rows)),
                           ('finalizing', 'finalizing' in checked['phases']), ('sharing', 'sharing' in checked['phases'])]:
            if value and not flags[key]:
                flags[key] = True
                new.append(key)
        if new:
            self.witness(label + '-' + '-'.join(new))
        self.save('results.json', self.result)
        return after, snapshot, checked

    def operations(self, ids, label):
        current, _, _ = self.observe(label)
        found = {op['id']: op for op in current if op['id'] in ids}
        for operation_id in ids:
            if operation_id not in found:
                found[operation_id] = self.cli('operation.inspect', {'id': operation_id})
        return found

    def settled(self, ids, label):
        def terminal():
            found = self.operations(ids, label)
            if len(found) == len(ids) and all(op['state'] in TERMINAL and
                    op.get('copyStatus') in ('copied', 'suppressed', 'failed', 'no_result') for op in found.values()):
                return found
        found = self.wait(terminal, 'Operations did not settle: ' + label)
        self.save(label + '-outcomes.json', {key: {
            'state': op['state'], 'copyStatus': op['copyStatus'], 'items': [{
                'historyId': item['artifact']['id'], 'sourceBytes': item['artifact']['bytes'],
                'sha256': item['artifact']['sha256'], 'uploadStatus': item['uploadStatus'],
                'localStatus': item['local']['status'], 'shareStatus': item.get('shareStatus'),
                'errorCode': item.get('error', {}).get('code')}
                for item in op['items']]} for key, op in found.items()})
        def idle():
            _, snapshot, checked = self.observe(label)
            return snapshot if checked['matched'] and checked['idle'] else None
        snapshot = self.wait(idle, 'Tray did not return to idle: ' + label)
        require([icon['sha256'] for icon in snapshot['icons']] == self.idle_hashes,
                'Settled tray did not restore the exact original idle icon')
        self.witness(label + '-idle')
        return found

    def clipboard(self):
        image = self.witness('source-fixture', hover=False)
        paint = lab.locate_fixture_paint(image)
        require(paint, 'Independent native synthetic clipboard fixture is not visibly unobscured')
        # Let the visible guest process the move before pressing; a combined
        # tablet move/button packet can click the previous tray position.
        lab.pointer(self.run, paint['x'] + 100, paint['y'] + 100, self.width, self.height)
        time.sleep(.15)
        lab.pointer(self.run, paint['x'] + 100, paint['y'] + 100, self.width, self.height, True)
        time.sleep(.05)
        lab.pointer(self.run, paint['x'] + 100, paint['y'] + 100, self.width, self.height, False)
        lab.ssh(self.run, 'rm -f -- ' + shlex.quote(self.guest + '/f/source.json') + '\n')
        lab.key(self.run, ['ctrl', 'alt', 'shift', 'f'])
        def acknowledged():
            try:
                value = self.get_json(self.guest + '/f/source.json')
                return value if value.get('synthetic') and value.get('key') == 70 else None
            except lab.LabError:
                return None
        self.wait(acknowledged, 'Native guest clipboard fixture did not acknowledge synthetic files')

    def invoke(self):
        accepted = self.cli('action.invoke', {'action': 'clipboard'})
        operation_id = accepted['operationId']
        self.ids.append(operation_id)
        self.save('results.json', self.result)
        return operation_id

    def setup(self):
        require(self.info['profile'] in ('cinnamon-x11', 'gnome-wayland', 'kde-wayland', 'fedora-gnome-wayland'),
                'Unsupported native desktop; no substitute desktop evidence')
        patch = private_settings(self.args.settings, self.args.profile_id, self.args.provider, self.args.isolation_marker)
        package = lab.read_json(self.run / 'package.json')
        require(package.get('sha256') == self.args.expected_package_sha256, 'Package receipt differs from requested package')
        binaries = package.get('installedBinarySha256', {})
        require(all(path in binaries for path in ('/usr/bin/putgd', '/usr/bin/putgd-cli', '/usr/bin/putgd-desktop-fixture')),
                'Package receipt must include app, CLI and native fixture hashes')
        require(all(Path(path).is_absolute() and re.fullmatch(r'[0-9a-f]{64}', digest) for path, digest in binaries.items()),
                'Package binary receipt malformed')
        hashes = lab.ssh(self.run, 'sha256sum -- ' + ' '.join(shlex.quote(path) for path in binaries) + '\n')
        actual = {path: digest for digest, path in (line.split(maxsplit=1) for line in hashes.splitlines())}
        require(actual == binaries, 'Installed bytes differ from the package receipt')
        probe_hash = lab.ssh(self.run, 'sha256sum -- ' + shlex.quote(self.args.probe) + '\n').split()[0]
        require(probe_hash == self.args.expected_probe_sha256, 'Installed probe hash mismatch')
        manifests = [json.loads(lab.ssh(self.run, path + ' --version-json\n'))
                     for path in ('/usr/bin/putgd', '/usr/bin/putgd-cli')]
        check_build_manifests(manifests, self.args.expected_commit)
        native = json.loads(lab.guest_session(self.run, '''python3 - <<'PY'
import json, os, urllib.parse
paths = []
wayland = os.environ.get('WAYLAND_DISPLAY', '')
if wayland:
    paths.append(wayland if wayland.startswith('/') else os.path.join(os.environ['XDG_RUNTIME_DIR'], wayland))
for address in os.environ.get('DBUS_SESSION_BUS_ADDRESS', '').split(';'):
    if address.startswith('unix:'):
        for field in address[5:].split(','):
            if field.startswith('path='):
                paths.append(urllib.parse.unquote(field[5:]))
print(json.dumps({'paths': [os.path.realpath(path) for path in paths]}))
PY
'''))
        resolved = lab.ssh(self.run, 'python3 -c ' + shlex.quote('import os; print(os.path.realpath(' + repr(self.guest) + '))') + '\n').strip()
        checks = socket_paths(resolved, self.run / 'qmp.sock', native['paths'])
        self.save('socket-preflight.json', {'limitBytesExcludingNul': 107, 'paths': checks})
        require(resolved == self.guest, 'Guest private runtime root unexpectedly resolves elsewhere')
        require(not lab.ssh(self.run, 'pgrep -u "$(id -u)" -x putgd || true\n').strip(),
                'Existing lab resident must be shut down explicitly before this isolated scenario')
        lab.ssh(self.run, 'umask 077\nmkdir -m 700 -- ' + shlex.quote(self.guest) + '\nmkdir -- ' +
                ' '.join(shlex.quote(self.guest + suffix) for suffix in ('/c', '/d', '/r', '/f', '/archive')) + '\n')
        self.session('nohup /usr/bin/putgd --background > ' + shlex.quote(self.guest + '/app.log') +
                     ' 2>&1 < /dev/null &\n')
        def ready():
            try:
                value = self.cli('diagnostics')
                return value if value.get('journalReady') else None
            except (lab.LabError, TrayProgressError):
                return None
        diagnostics = self.wait(ready, 'Isolated resident did not become ready')
        self.patch({**patch, 'actions': {name: {'destination': 'upload', 'profileId': self.args.profile_id,
                                             'shortcut': '', 'publicationMode': 'share'}
                    for name in ('region', 'window', 'desktop', 'clipboard')},
                    'archive': {'root': self.guest + '/archive', 'keepUploads': False,
                                'folders': {'images': None, 'videos': None, 'text': None, 'other': None}},
                    'naming': patch['naming'] if self.args.provider == 's3' and
                              patch['profiles'][0]['settings'].get('objectPrefix') == '' else
                              {'mode': 'generated', 'collisionPolicy': 'rename'},
                    'limits': {'concurrentUploads': 1}, 'retention': {'recentActivityLimit': 100}, 'preferences': {'notifications': False, 'launchAtLogin': False}})
        self.activate_credentials()
        identity = lab.verify_running_app(self.run)
        self.pid = identity['pid']
        self.result['runningApp'] = identity
        self.result['build'] = manifests[0]
        self.save('package-identity.json', package)
        self.save('initial-diagnostics.json', diagnostics)
        self.save('capabilities.json', self.cli('capabilities'))
        self.session('nohup /usr/bin/putgd-desktop-fixture --output ' + shlex.quote(self.guest + '/f') +
                     ' > ' + shlex.quote(self.guest + '/fixture.log') + ' 2>&1 < /dev/null &\n')
        def fixture_ready():
            try:
                return self.get_json(self.guest + '/f/ready.json')
            except lab.LabError:
                return None
        ready = self.wait(fixture_ready, 'Native fixture did not become ready')
        self.result.update(lab.validate_readiness(self.profile, ready, self.info['acceleration']))
        self.save('native-session.json', ready)
        layout = ready['display_layout']
        require(len(layout) == 1, 'This replay requires the provisioned single-head native profile')
        self.width, self.height = layout[0]['nativeWidth'], layout[0]['nativeHeight']
        require(0 <= self.args.tray_x < self.width and 0 <= self.args.tray_y < self.height, 'Tray hover lies outside native head')
        generate = """import hashlib,json,pathlib
root=pathlib.Path(ROOT)
chunk=bytes(range(256))*4096
results=[]
for number in (1,2):
    path=root/('synthetic-%d.txt'%number)
    digest=hashlib.sha256()
    with path.open('wb') as stream:
        for _ in range(MIB):
            stream.write(chunk);digest.update(chunk)
    results.append({'bytes':MIB*1048576,'sha256':digest.hexdigest()})
print(json.dumps(results))
""".replace('ROOT', repr(self.guest + '/f')).replace('MIB', str(self.args.payload_mib))
        self.sources = json.loads(lab.ssh(self.run, 'python3 - <<\'PY\'\n' + generate + 'PY\n'))
        self.save('synthetic-source-identities.json', self.sources)
        idle = self.snapshot()
        require(check_tooltip(idle, [], self.args.provider, self.pid)['idle'], 'Initial tray is not idle')
        self.idle_hashes = [icon['sha256'] for icon in idle['icons']]
        self.save('initial-tray.json', idle)
        self.witness('initial-idle')

    def successful_uploads(self, found):
        for op in found.values():
            require(op['state'] == 'completed' and len(op['items']) == 2 and
                    all(item['uploadStatus'] == 'uploaded' for item in op['items']), 'Synthetic upload did not complete')
            require([{'bytes': item['artifact']['bytes'], 'sha256': item['artifact']['sha256']} for item in op['items']] == self.sources,
                    'Acquired upload artifacts differ from the native synthetic file source')
            require(all(item.get('receipt', {}).get('directUrl', '').startswith(('http://', 'https://'))
                        for item in op['items']), 'Completed shared upload has no confirmed link')

    def independent_paste(self, operation, label):
        self.cli('operation.copy', {'id': operation['id']})
        picture = self.witness(label + '-paste-target', hover=False)
        paint = lab.locate_fixture_paint(picture)
        require(paint, 'Independent native paste fixture is not visibly unobscured')
        # Let the visible guest process the move before pressing; a combined
        # tablet move/button packet can click the previous tray position.
        lab.pointer(self.run, paint['x'] + 100, paint['y'] + 100, self.width, self.height)
        time.sleep(.15)
        lab.pointer(self.run, paint['x'] + 100, paint['y'] + 100, self.width, self.height, True)
        time.sleep(.05)
        lab.pointer(self.run, paint['x'] + 100, paint['y'] + 100, self.width, self.height, False)
        lab.ssh(self.run, 'rm -f -- ' + shlex.quote(self.guest + '/f/paste.json') + '\n')
        lab.key(self.run, ['ctrl', 'v'])
        def pasted():
            try:
                return self.get_json(self.guest + '/f/paste.json')
            except lab.LabError:
                return None
        actual = self.wait(pasted, 'Independent native clipboard paste not acknowledged')
        expected = '\n'.join(item['receipt']['directUrl'] for item in operation['items'])
        require(actual.get('text') == expected, 'Independent pasted links differ from original upload receipts')
        # Link values/access fragments stay in guest memory and the app journal.
        self.save(label + '-paste.json', {'matchedOriginalReceipts': True, 'linkCount': len(operation['items'])})
        self.witness(label + '-paste-completed', hover=False)

    def s3_public_bytes(self, operation):
        # Read original URLs inside guest memory through the production CLI, so
        # no link or account data is interpolated into SSH/process arguments.
        script = """import hashlib,json,subprocess,urllib.request
reply=json.loads(subprocess.check_output(['/usr/bin/putgd-cli','operation.inspect',json.dumps({'id':OPERATION})],text=True))
assert reply['ok']
results=[]
for item in reply['result']['items']:
    expected=item['artifact']['bytes']; received=0; digest=hashlib.sha256()
    with urllib.request.urlopen(item['receipt']['directUrl'],timeout=30) as response:
        while True:
            chunk=response.read(1048576)
            if not chunk:break
            received+=len(chunk)
            assert received<=expected
            digest.update(chunk)
    assert received==expected and digest.hexdigest()==item['artifact']['sha256']
    results.append({'bytes':received,'sha256':digest.hexdigest()})
print(json.dumps(results))
""".replace('OPERATION', repr(operation['id']))
        self.save('s3-public-byte-verification.json', json.loads(self.session("python3 - <<'PY'\n" + script + 'PY\n')))

    def recover_proton_busy(self, initial):
        if self.args.provider != 'proton-drive':
            return initial
        # Same-account SDK calls are serialized by the production account lock.
        # Keep concurrent outcomes, then recover only explicit busy failures on
        # the original IDs with a single worker and the same credential reference.
        self.save('concurrent-original-outcomes-private.json', initial)
        def complete(op):
            return op['state'] == 'completed' and all(item['uploadStatus'] == 'uploaded'
                and item.get('shareStatus') == 'shared' for item in op['items'])
        def allowed(op):
            require(op['items'], 'Concurrent Proton operation has no retained artifacts')
            for item in op['items']:
                if item['uploadStatus'] == 'uploaded' and item.get('shareStatus') == 'shared':
                    continue
                require(item['uploadStatus'] != 'unknown' and item.get('shareStatus') != 'unknown'
                        and item.get('error', {}).get('code') == 'proton_account_busy',
                        'Concurrent Proton outcome requires separate recovery')
        pending = [operation_id for operation_id, op in initial.items() if not complete(op)]
        for operation_id in pending:
            allowed(initial[operation_id])
        if not pending:
            return initial
        self.patch({'limits': {'concurrentUploads': 1}})
        recovered = dict(initial)
        for attempt in range(1, 4):
            for operation_id in pending:
                accepted = self.cli('operation.retry', {'id': operation_id})
                require(accepted.get('operationId') == operation_id and accepted.get('retryAccepted'),
                        'Busy recovery did not retain original operation ID')
                op = self.settled([operation_id], 'concurrent-recovery-' + str(attempt))[operation_id]
                old_items, items = initial[operation_id]['items'], op['items']
                require([item['artifact']['id'] for item in items] == [item['artifact']['id'] for item in old_items]
                        and [item.get('publicationId') for item in items] == [item.get('publicationId') for item in old_items],
                        'Busy recovery changed retained artifact/publication identity')
                require(all(old.get('receipt') == item.get('receipt') for old, item in zip(recovered[operation_id]['items'], items)
                            if old['uploadStatus'] == 'uploaded' and old.get('shareStatus') == 'shared'),
                        'Busy recovery changed an already shared publication receipt')
                recovered[operation_id] = op
                if not complete(op):
                    allowed(op)
            pending = [operation_id for operation_id, op in recovered.items() if not complete(op)]
            if not pending:
                return recovered
        raise TrayProgressError('Bounded original-ID Proton busy recovery did not complete')

    def execute(self):
        self.setup()
        self.clipboard()
        queue_ids = [self.invoke(), self.invoke()]
        queued = self.settled(queue_ids, 'queue')
        self.successful_uploads(queued)
        require(self.result['coverage']['queue']['active'] and self.result['coverage']['queue']['counter']
                and self.result['coverage']['queue']['queued'], 'Queued active work/counters were not observed; preserve this run')
        self.independent_paste(queued[queue_ids[-1]], 'upload')
        if self.args.provider == 's3':
            self.s3_public_bytes(queued[queue_ids[-1]])
        self.patch({'limits': {'concurrentUploads': 2}})
        self.clipboard()
        concurrent_ids = [self.invoke(), self.invoke()]
        concurrent = self.settled(concurrent_ids, 'concurrent')
        concurrent = self.recover_proton_busy(concurrent)
        self.successful_uploads(concurrent)
        require(self.result['coverage']['concurrent']['concurrentAdmission'], 'Two active admitted operations were not observed')
        self.patch({'limits': {'concurrentUploads': 1}})
        self.clipboard()
        cancelled_id = self.invoke()
        def cancellable():
            found = self.operations([cancelled_id], 'cancel')
            op = found.get(cancelled_id)
            if op and any(item.get('uploadStatus') == 'uploading' and
                          (item.get('uploadProgress') or {}).get('bytesTransferred', 0) > 0 for item in op['items']):
                return op
        original = self.wait(cancellable, 'Upload counter not observed in time to cancel')
        self.cli('operation.cancel', {'id': cancelled_id})
        cancelled = self.settled([cancelled_id], 'cancel')[cancelled_id]
        require(cancelled['state'] == 'cancelled', 'Upload did not settle as cancelled')
        accepted = self.cli('operation.retry', {'id': cancelled_id})
        require(accepted.get('operationId') == cancelled_id and accepted.get('retryAccepted'), 'Retry did not retain operation ID')
        retried = self.settled([cancelled_id], 'retry')
        self.successful_uploads(retried)
        require([item['artifact']['id'] for item in original['items']] ==
                [item['artifact']['id'] for item in retried[cancelled_id]['items']], 'Retry changed retained artifact identity')
        require([item['publicationId'] for item in original['items']] ==
                [item['publicationId'] for item in retried[cancelled_id]['items']], 'Retry changed original publication IDs')
        # A regular file blocks mkdir/copy deterministically, inside this new
        # private synthetic root. This never touches an existing user archive.
        blocked = self.guest + '/blocked'
        lab.ssh(self.run, 'umask 077\nprintf synthetic > ' + shlex.quote(blocked) + '\n')
        self.patch({'actions': {'clipboard': {'destination': 'local', 'profileId': None}},
                    'archive': {'root': blocked}})
        self.clipboard()
        local_id = self.invoke()
        failed = self.settled([local_id], 'local-failure')[local_id]
        require(failed['items'] and all(item['local']['status'] == 'failed' and item['uploadStatus'] == 'local_only'
                for item in failed['items']), 'Synthetic local-save failure was not observed')
        lab.ssh(self.run, 'rm -- ' + shlex.quote(blocked) + '\nmkdir -m 700 -- ' + shlex.quote(blocked) + '\n')
        self.cli('operation.retryLocalSave', {'id': local_id})
        local = self.settled([local_id], 'local-retry')[local_id]
        require(all(item['local']['status'] == 'saved' and item['uploadStatus'] == 'local_only' for item in local['items']),
                'Local-save retry did not retain local-only behavior')
        for index, item in enumerate(local['items']):
            target = self.output / ('local-result-' + str(index))
            require(item['local']['path'].startswith(blocked + '/'), 'Saved synthetic result escaped the owned archive')
            lab.get(self.run, item['local']['path'], target)
            require(target.stat().st_size == self.sources[index]['bytes'] and lab.hash_file(target) == self.sources[index]['sha256'],
                    'Actual saved local bytes differ from independent synthetic source')
        upload_ids = queue_ids + concurrent_ids + [cancelled_id]
        retained = [self.cli('operation.inspect', {'id': operation_id}) for operation_id in upload_ids]
        history_ids = [item['artifact']['id'] for op in retained for item in op['items']
                       if item['uploadStatus'] == 'uploaded']
        self.result['historyIds'] = history_ids
        before_restart = {history_id: self.cli('history.detail', {'id': history_id}) for history_id in history_ids}
        require(self.cli('ui.quit').get('quitting'), 'Resident quit not acknowledged')
        self.wait(lambda: not lab.ssh(self.run, 'pgrep -u "$(id -u)" -x putgd || true\n').strip(), 'Resident did not exit')
        self.session('nohup /usr/bin/putgd --background > ' + shlex.quote(self.guest + '/restart.log') + ' 2>&1 < /dev/null &\n')
        def restarted():
            try:
                value = self.cli('diagnostics')
                return value if value.get('journalReady') else None
            except (lab.LabError, TrayProgressError):
                return None
        self.wait(restarted, 'Resident restart did not become ready')
        identity = lab.verify_running_app(self.run)
        require(identity != self.result['runningApp'], 'Resident process identity did not change after restart')
        self.pid = identity['pid']
        self.result['runningAppAfterRestart'] = identity
        self.history = []
        self.mismatch_since = None
        require(all(self.cli('history.detail', {'id': history_id})['receipt'] == old['receipt']
                    for history_id, old in before_restart.items()), 'Restart changed original publication receipts')
        snapshot = self.snapshot()
        require(check_tooltip(snapshot, [], self.args.provider, self.pid)['idle'] and
                [icon['sha256'] for icon in snapshot['icons']] == self.idle_hashes, 'Restart did not restore exact idle tray state')
        self.save('restarted-tray.json', snapshot)
        self.witness('restarted-idle')
        # Preparation is read-only. Do not call delete.confirm or any sharing
        # mutation; Proton safely rejects guarded deletion and retains files.
        preparation = self.cli('delete.prepare', {'ids': history_ids},
                               expected_error='unsupported_capability' if self.args.provider == 'proton-drive' else None)
        preparation['selectedHistoryIds'] = history_ids
        self.save('cleanup-preparation-private.json', preparation)
        require(lab.verify_running_app(self.run) == identity, 'Resident identity changed during final evidence collection')
        self.result['protocolPassed'] = True
        self.result['hostRestart'] = 'not automated; native host remains unchanged; see manual current-state gate'
        self.result['transientPhases'] = {phase: 'observed' if any(flags[phase] for flags in self.result['coverage'].values())
                                        else 'not-observed' for phase in ('finalizing', 'sharing')}
        self.result['status'] = 'protocol-passed-rendering-pending'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('run', 'output', 'settings', 'profile-id', 'isolation-marker', 'expected-commit',
                 'expected-package-sha256', 'probe', 'expected-probe-sha256'):
        parser.add_argument('--' + name, required=True)
    parser.add_argument('--provider', choices=['s3', 'proton-drive'], required=True)
    parser.add_argument('--tray-x', type=int, required=True)
    parser.add_argument('--tray-y', type=int, required=True)
    parser.add_argument('--payload-mib', type=int, default=32, help='Synthetic bytes per file; preserve failed fast runs before retrying larger')
    parser.add_argument('--timeout', type=float, default=180, help='Per-stage observation deadline')
    args = parser.parse_args()
    require(args.payload_mib > 0 and args.timeout > 0, 'Payload and timeout must be positive')
    require(re.fullmatch(r'[0-9a-f]{40}', args.expected_commit), 'Expected commit must be a full SHA')
    require(Path(args.probe).is_absolute(), 'Guest probe must be an absolute path')
    replay = None
    try:
        replay = Replay(args)
        replay.execute()
        return_code = 0
    except (TrayProgressError, lab.LabError, OSError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
        if replay:
            replay.result['status'] = 'failed-preserved'
            replay.result['error'] = str(error)
        else:
            print(json.dumps({'protocolPassed': False, 'error': str(error)}))
        return_code = 1
    finally:
        if replay:
            replay.save('results.json', replay.result)
            replay.save('manifest.json', {'schemaVersion': 1, 'files': {path.name: lab.hash_file(path)
                for path in sorted(replay.output.iterdir()) if path.is_file() and path.name != 'manifest.json'}})
            print(json.dumps({'protocolPassed': replay.result['protocolPassed'],
                'graphicalAcceptance': replay.result['graphicalAcceptance'], 'output': str(replay.output),
                'operationIds': replay.ids, 'status': replay.result.get('status'), 'error': replay.result.get('error')}))
    return return_code


if __name__ == '__main__':
    raise SystemExit(main())
