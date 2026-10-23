#!/usr/bin/env python3
"""Proton smoke test inside a VM that borrows its builder's session (run by proton_lab.py lend).

  proton_lab.py lend RUN ... -- python3 tools/lab/proton_lend_smoke.py

The run's put.gd must be installed (lab.py install). With the borrowed keychain
reference from PUTGD_LAB_PROTON_REFERENCE, it adds a Proton destination to the
guest's lab configuration, creates a new isolated folder, uploads one synthetic
PNG privately through the editor, then creates, inspects and revokes that
upload's own link. It writes proton-smoke.json (nonsecret: names, IDs, states,
no URLs) into the run, and quits the guest's put.gd. Proton files cannot be
deleted through put.gd, so the folder and its one file stay for manual removal.
"""
import datetime
import importlib.util
import json
import os
from pathlib import Path
import time

HERE = Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location('putgd_smoke_lab', HERE / 'lab.py')
lab = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(lab)

PROFILE_ID = 'proton-lent'
DEADLINE = 300
SECRET_KEYS = ('url', 'link', 'shareUrl', 'publicUrl', 'directUrl')


def scrub(value):
    """Drop links (sensitive share URLs with their fragment) before anything is kept."""
    if isinstance(value, dict):
        return {key: scrub(item) for key, item in value.items()
                if not any(word.lower() in key.lower() for word in SECRET_KEYS)}
    if isinstance(value, list):
        return [scrub(item) for item in value]
    if isinstance(value, str) and value.startswith(('http://', 'https://')):
        return '<link>'
    return value


class Smoke:
    def __init__(self, run, reference):
        self.run, self.reference = Path(run), reference
        self.result = {'schemaVersion': 1, 'run': str(self.run.resolve()), 'reference': reference, 'steps': []}

    def cli(self, method, params=None):
        return lab.cli(self.run, method, params)

    def step(self, name, **fields):
        entry = dict(scrub(fields), step=name, at=datetime.datetime.now(datetime.timezone.utc).isoformat())
        self.result['steps'].append(entry)
        lab.write_json(self.run / 'proton-smoke.json', self.result)
        print(json.dumps(entry), flush=True)

    def wait(self, read, done, what):
        deadline = time.monotonic() + DEADLINE
        while True:
            value = read()
            if done(value):
                return value
            if time.monotonic() > deadline:
                raise lab.LabError(f'{what} did not finish in {DEADLINE} s')
            time.sleep(2)

    def revision(self):
        return self.cli('settings.get')['metadata']['observedRevision']

    def setup(self, kind, params=None):
        job = self.cli('setup.' + kind, dict(params or {}, profileId=PROFILE_ID, expectedRevision=self.revision()))
        state = self.wait(lambda: self.cli('setup.inspect', {'id': job['setupId']}),
                          lambda value: value.get('state') != 'running', 'setup.' + kind)
        if state.get('state') != 'completed':
            raise lab.LabError(f'setup.{kind} failed: {json.dumps(scrub(state.get("error")))}')
        return state.get('result') or {}

    def patch_profile(self, **settings):
        document = self.cli('settings.get')
        profiles = []
        for profile in document['document']['profiles']:
            if profile['id'] == PROFILE_ID:
                profile = dict(profile, settings=dict(profile['settings'], **settings))
            profiles.append(profile)
        self.cli('settings.patch', {'patch': {'profiles': profiles},
                                    'expectedRevision': document['metadata']['observedRevision']})

    def configure(self):
        try:
            self.cli('capabilities')
        except (lab.LabError, ValueError):
            lab.start(self.run)
        document = self.cli('settings.get')
        profiles = [profile for profile in document['document']['profiles'] if profile['id'] != PROFILE_ID]
        profiles.append({'id': PROFILE_ID, 'label': 'Proton (lent)', 'provider': 'proton-drive',
                         'settings': {'accountRootUid': '', 'folderUid': '', 'shareByDefault': False,
                                      'linkExpirationDays': 0, 'sharePasswordReference': None,
                                      'requestTimeoutMs': 120000},
                         'credentials': {'mode': 'keychain', 'reference': self.reference}})
        self.cli('settings.patch', {'patch': {'profiles': profiles, 'actions': {'edit': {
            'destination': 'upload', 'profileId': PROFILE_ID, 'publicationMode': 'private'}}},
            'expectedRevision': document['metadata']['observedRevision']})
        self.step('configured', profileId=PROFILE_ID)

    def folder(self):
        account = self.setup('inspectAccount')
        self.patch_profile(accountRootUid=account['accountRootUid'])
        self.step('account', accountEmail=account.get('accountEmail'))
        stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
        name = f'putgd-lab-{lab.state(self.run)["profile"]}-{stamp}'
        folder = self.setup('createFolder', {'input': {'name': name}})
        self.patch_profile(folderUid=folder['uid'], folderName=folder.get('name', name))
        self.result['folderName'] = folder.get('name', name)
        self.step('folder', folderName=self.result['folderName'])

    def upload(self):
        # A synthetic 256x160 gradient, written by the guest's own python3; no host data.
        path = lab.ssh(self.run, '''set -eu
mkdir -p "$HOME/putgd-lab/proton-smoke"
python3 - "$HOME/putgd-lab/proton-smoke" <<'PY'
import struct, sys, time, zlib
w, h = 256, 160
rows = b''.join(b'\\0' + bytes(v for x in range(w) for v in ((x * 255) // w, (y * 255) // h, 160)) for y in range(h))
chunk = lambda kind, data: struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
png = b'\\x89PNG\\r\\n\\x1a\\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + \\
      chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b'')
path = sys.argv[1] + '/putgd-lab-synthetic-%d.png' % time.time()
open(path, 'wb').write(png)
print(path)
PY
''').strip()
        opened = self.cli('editor.open', {'source': 'file', 'path': path})
        operation = opened['operationId']
        self.cli('editor.submit', {'id': operation})
        final = self.wait(lambda: self.cli('operation.inspect', {'id': operation}),
                          lambda value: value.get('state') not in ('accepted', 'running', 'queued', 'editing'),
                          'upload')
        self.result['operationId'] = operation
        self.step('uploaded', operationId=operation, state=final.get('state'), file=Path(path).name)
        if final.get('state') != 'completed':
            raise lab.LabError('The synthetic upload did not complete: ' + json.dumps(scrub(final))[:400])
        entries = self.cli('history.list', {'profileId': PROFILE_ID, 'limit': 10})
        items = entries.get('records') or []
        mine = [item for item in items if item.get('operationId') == operation]
        if len(mine) != 1:
            raise lab.LabError('The synthetic upload has no single history entry')
        self.result['historyId'] = mine[0]['id']

    def sharing(self, method):
        started = self.cli(method, {'id': self.result['historyId']})
        outcome = self.wait(lambda: self.cli('share.inspect', {'id': started['shareOperationId']}),
                            lambda value: value.get('status', value.get('state')) not in ('pending', 'running'),
                            method)
        # Keep the outcome's status, not its receipt (node, revision and destination detail).
        kept = {key: outcome.get(key) for key in ('action', 'state', 'status', 'expiresAt') if key in outcome}
        kept['error'] = (outcome.get('error') or {}).get('code') or None
        self.step(method, outcome=kept)
        return kept

    def links(self):
        created = self.sharing('history.share')
        inspected = self.sharing('history.inspectLink')
        revoked = self.sharing('history.revokeLink')
        detail = self.cli('history.detail', {'id': self.result['historyId']})
        self.step('detail', detail={key: detail.get(key) for key in ('remoteStatus', 'shareStatus', 'shareExpiresAt',
                                                                      'uploadStatus')})
        self.result['links'] = {'created': scrub(created), 'inspected': scrub(inspected), 'revoked': scrub(revoked)}

    def finish(self):
        try:
            self.cli('ui.quit')
        except (lab.LabError, ValueError):
            pass
        self.step('quit')


def main():
    run, reference = os.environ.get('PUTGD_LAB_RUN'), os.environ.get('PUTGD_LAB_PROTON_REFERENCE')
    if not run or not reference:
        print('Run through proton_lab.py lend, which provides PUTGD_LAB_RUN and PUTGD_LAB_PROTON_REFERENCE')
        return 2
    smoke = Smoke(run, reference)
    try:
        smoke.configure()
        smoke.folder()
        smoke.upload()
        smoke.links()
        smoke.result['passed'] = True
    except (lab.LabError, KeyError, ValueError) as error:
        smoke.result['passed'] = False
        smoke.step('failed', error=str(error)[:400])
    finally:
        smoke.finish()
    return 0 if smoke.result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
