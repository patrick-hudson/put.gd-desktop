#!/usr/bin/env python3
"""Proton smoke test inside a VM that borrows its builder's session (run by proton_lab.py lend).

  proton_lab.py lend RUN ... -- python3 tools/lab/proton_lend_smoke.py
  proton_lab.py lend RUN ... --guest-reference REF -- python3 tools/lab/proton_lend_smoke.py --revoke HISTORY_ID
  proton_lab.py lend RUN ... -- python3 tools/lab/proton_lend_smoke.py --pictures   # plus guest screenshots
  proton_lab.py lend RUN ... -- python3 tools/lab/proton_lend_smoke.py --browser --hold 900
      # a run-owned Firefox profile as the guest's link handler; hold before and after revoke for framing

The run's put.gd must be installed (lab.py install). With the borrowed keychain
reference from PUTGD_LAB_PROTON_REFERENCE, it adds a Proton destination to the
guest's lab configuration, creates a new isolated folder, runs Check account and
folder against it, uploads one synthetic PNG privately through the editor, then
creates, inspects and revokes that upload's own link. It writes proton-smoke.json (nonsecret: names, IDs, states,
no URLs) into the run, and quits the guest's put.gd. Proton files cannot be
deleted through put.gd, so the folder and its one file stay for manual removal.
"""
import datetime
import importlib.util
import json
import os
from pathlib import Path
import sys
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

    def check(self):
        """Settings' Check account and folder (Test connection) against the new folder."""
        tested = self.setup('testDestination')
        self.result['testScope'] = tested.get('testScope')
        self.step('checked', testScope=self.result['testScope'])
        if self.result['testScope'] != 'account-folder':
            raise lab.LabError(f'Check account and folder covered {self.result["testScope"]}, not account-folder')

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

    def links(self, hold=None):
        created = self.expect(self.sharing('history.share'), 'shared')
        inspected = self.expect(self.sharing('history.inspectLink'), 'shared')
        if hold:
            self.hold(hold, 'before-revoke')  # frame the live link and its share page
        revoked = self.expect(self.sharing('history.revokeLink'), 'revoked')
        if hold:
            self.hold(hold, 'after-revoke')  # frame the revoked state
        detail = self.cli('history.detail', {'id': self.result['historyId']})
        self.step('detail', detail={key: detail.get(key) for key in ('remoteStatus', 'shareStatus', 'shareExpiresAt',
                                                                      'uploadStatus')})
        self.result['links'] = {'created': scrub(created), 'inspected': scrub(inspected), 'revoked': scrub(revoked)}

    def expect(self, outcome, status):
        if outcome.get('status') != status:
            raise lab.LabError(f'{outcome.get("action")} ended {outcome.get("status")} '
                               f'({outcome.get("error")}), not {status}')
        return outcome

    def revoke(self, history_id):
        """Finish an earlier run (lend --guest-reference gives back its keychain name): inspect its link,
        which an interrupted create leaves unknown, then revoke it if it exists."""
        self.result['historyId'] = history_id
        inspected = self.sharing('history.inspectLink')
        if inspected.get('status') == 'shared':
            self.expect(self.sharing('history.revokeLink'), 'revoked')
        elif inspected.get('state') != 'completed' or inspected.get('status') == 'unknown':
            raise lab.LabError('The link could not be settled: ' + json.dumps(inspected))
        detail = self.cli('history.detail', {'id': history_id})
        self.step('detail', detail={key: detail.get(key) for key in ('shareStatus', 'uploadStatus')})

    def browser(self):
        """Disposable guests only: a new Firefox profile of this run's own as the http(s) handler, so
        put.gd's Open link shows the share page. First-run pages are skipped; terms are never accepted."""
        profile = lab.guest_session(self.run, '''set -eu
if command -v snap >/dev/null && snap list firefox >/dev/null 2>&1; then base=$HOME/snap/firefox/common; else base=$HOME/.cache; fi
profile=$base/putgd-lab-firefox-$(date +%s)
mkdir -p "$profile" "$HOME/.local/share/applications"
cat > "$profile/user.js" <<'JS'
user_pref("browser.shell.checkDefaultBrowser", false);
user_pref("browser.startup.homepage_override.mstone", "ignore");
user_pref("browser.aboutwelcome.enabled", false);
user_pref("startup.homepage_welcome_url", "");
user_pref("toolkit.telemetry.reportingpolicy.firstRun", false);
JS
cat > "$HOME/.local/share/applications/putgd-lab-firefox.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=Firefox (put.gd lab profile)
Exec=firefox --new-instance --profile $profile %u
MimeType=x-scheme-handler/http;x-scheme-handler/https;text/html;
NoDisplay=true
DESKTOP
xdg-mime default putgd-lab-firefox.desktop x-scheme-handler/https x-scheme-handler/http text/html
update-desktop-database "$HOME/.local/share/applications" 2>/dev/null || true
printf '%s' "$profile"
''').strip().splitlines()[-1]
        self.result['browserProfile'] = profile
        self.step('browser', profile=profile)

    def pictures(self):
        """Evidence frames of the guest's own screen (a disposable VM): Settings with the lent
        destination and its account, then History with the revoked link. The app keeps both open."""
        for view in ('settings', 'history'):
            self.cli('ui.show', {'view': view})
            time.sleep(4)
            frame = lab.screenshot(self.run, f'proton-{view}')
            self.step('picture', view=view, path=frame['path'], sha256=frame['sha256'])

    def hold(self, seconds, stage):
        """Keep the guest's put.gd open (the session stays lent, under the lock) while an operator
        drives and frames the guest screen through QMP, until RUN/proton-hold-release exists."""
        release = self.run / 'proton-hold-release'
        self.step('holding', stage=stage, release=str(release), seconds=seconds)
        deadline = time.monotonic() + seconds
        while not release.exists() and time.monotonic() < deadline:
            time.sleep(2)
        release.unlink(missing_ok=True)
        self.step('released')

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
    revoke = sys.argv[sys.argv.index('--revoke') + 1] if '--revoke' in sys.argv[1:-1] else None
    try:
        if revoke:
            try:
                smoke.cli('capabilities')
            except (lab.LabError, ValueError):
                lab.start(smoke.run)
            smoke.revoke(revoke)
        else:
            smoke.configure()
            smoke.folder()
            smoke.check()
            if '--browser' in sys.argv[1:]:
                smoke.browser()
            smoke.upload()
            smoke.links(int(sys.argv[sys.argv.index('--hold') + 1]) if '--hold' in sys.argv[1:-1] else None)
            if '--pictures' in sys.argv[1:]:
                smoke.pictures()
        smoke.result['passed'] = True
    except (lab.LabError, KeyError, ValueError) as error:
        smoke.result['passed'] = False
        smoke.step('failed', error=str(error)[:400])
    finally:
        smoke.finish()
    return 0 if smoke.result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
