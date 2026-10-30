#!/usr/bin/env python3
"""Recipe contract/safety tests. These are not graphical acceptance evidence."""
from copy import deepcopy
import json
import fnmatch
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.lab.surface_driver.api import Context, RecipeBlocked, RecipeFailed, REGISTRY
from tools.lab.surface_driver.recipes import destinations as lane


class FakeContext(Context):
    def __init__(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='sd-')
        self.root = Path(self.temporary.name)
        (self.root / 'putgd').mkdir(mode=0o700)
        self.profile = {'destinations': {'accessible_state_contract': 1, 'capture_state_contract': 1},
                        'catalog_windows': {name: ['unit-only-confirmation'] for name in
                                            ('confirmDestinationRemove', 'confirmDestinationDefault', 'confirmDestinationReload')}}
        self.document = {'profiles': [], 'preferences': {}}
        self.revision = 1
        self.events = []
        self.focus = None
        self.selection = 0
        self.pending = ''
        self.index = 0
        self.dialog = None
        self.checked = False
        self.invalid = False
        self.corrupt_disk = False
        self.private_root = True
        self.conflict = False
        self.available = False
        self.tested = False
        self.f = {'private': True, 'config_path': str(self.root / 'putgd/config.json'),
                  'profile': {'id': 'fixture', 'label': 'Fixture', 'provider': 's3',
                              'credentials': {'mode': 'keychain', 'reference': 'owned-fake-reference'},
                              'settings': {'bucket': 'surface-moto', 'region': 'us-east-1',
                                           'endpoint': 'http://127.0.0.1:5000', 'objectPrefix': '',
                                           'publicUrlBase': None, 'aclMode': 'omit',
                                           'storageClass': 'STANDARD', 'connectTimeoutMs': 5000}}}

    def fixture(self, name):
        self.assert_event('fixture', name)
        if name != 'destinations':
            raise RecipeBlocked('missing fixture')
        return deepcopy(self.f)

    def assert_event(self, *values):
        self.events.append(values)

    def call(self, method, params=None):
        self.assert_event('call', method, deepcopy(params))
        if method == 'settings.get':
            path = Path(self.f['config_path'])
            path.write_text(json.dumps(self.document))
            path.chmod(0o600)
            return {'result': {'document': deepcopy(self.document), 'metadata': {
                'path': self.f['config_path'], 'observedRevision': str(self.revision)}}}
        if method == 'settings.patch':
            if params['expectedRevision'] != str(self.revision):
                return {'error': {'code': 'config_conflict'}}
            for key, value in params['patch'].items():
                if isinstance(value, dict):
                    self.document.setdefault(key, {}).update(deepcopy(value))
                else:
                    self.document[key] = deepcopy(value)
            self.revision += 1
            if self.invalid:
                self.conflict = True
            return {'result': {}}
        if method == 'setup.credentialHint':
            return {'available': self.available}
        raise AssertionError('undeclared resident method: ' + method)

    def view(self, method, params=None):
        self.assert_event('view', method, params)
        if (method, params) != ('ui.show', {'view': 'destinations'}):
            raise AssertionError('wrong view entry point')
        return {'shown': 'destinations'}

    def shell(self, script, timeout=60):
        # Run only the recipe's read-only Python probe against an actual private
        # temporary document. This checks its digest/mode logic independently
        # of the fake resident and real-input model.
        self.assert_event('shell', script)
        self.root.chmod(0o700 if self.private_root else 0o755)
        if self.corrupt_disk:
            Path(self.f['config_path']).write_text('{}')
        done = subprocess.run(['sh', '-s'], input=script, text=True, capture_output=True,
                              env={**os.environ, 'XDG_CONFIG_HOME': str(self.root)}, timeout=timeout)
        return {'returncode': done.returncode, 'stdout': done.stdout, 'stderr': done.stderr}

    def wait(self, predicate, what, timeout=10, interval=.2):
        value = predicate()
        self.expect(value, what)
        return value

    def frame(self, label, window=None, reference=None):
        self.assert_event('frame', label, window, reference)

    def use_catalog(self, window, reference):
        self.assert_event('use_catalog', window, reference)

    def geometry(self, window):
        return {'x': 0, 'y': 0, 'width': 1320, 'height': 880, 'scale': 1}

    def scroll(self, x, y, clicks):
        self.assert_event('scroll', x, y, clicks)

    def note(self, text):
        self.assert_event('note', text)

    def click(self, control, window, **kwargs):
        self.assert_event('click', control, window)
        self.focus = control
        self.pending = ''
        if control == 'confirmDiscard':
            if self.dialog == 'default':
                self.document['captureDefaults'] = {
                    'destination': 'upload', 'profileId': self.document['profiles'][0]['id'],
                    'publicationMode': 'default'}
            elif self.dialog == 'remove':
                self.document['profiles'].pop(self.selection)
            elif self.dialog == 'reload':
                self.conflict = False
                self.invalid = False
            else:
                raise AssertionError('confirmation was not opened')
            self.dialog = None
        if control == 'unlockCredentials':
            self.available = True
        if control == 'testDestination':
            self.tested = True
        if control == 'makeDefaultDestination':
            self.dialog = 'default'
        if control == 'reloadDestinationConflict':
            self.dialog = 'reload'
        if control == 's3BucketHelp':
            self.checked = not self.checked
        if control.endswith('Reset'):
            key, value = {'s3StorageClassReset': ('storageClass', 'STANDARD'),
                          's3ConnectTimeoutMsReset': ('connectTimeoutMs', 5000)}[control]
            self.document['profiles'][self.selection]['settings'][key] = value

    def type_text(self, text):
        self.assert_event('type_text', text)
        self.pending = text

    def keys(self, *sequence):
        self.assert_event('keys', sequence)
        for key in sequence:
            if key == 'Home':
                self.index = 0
                if self.focus == 'destinationList':
                    self.selection = 0
            elif key == 'Down':
                self.index += 1
                if self.focus == 'destinationList':
                    self.selection += 1
            elif key in ('Alt+Up', 'Alt+Down'):
                new = self.selection + (-1 if key == 'Alt+Up' else 1)
                p = self.document['profiles'].pop(self.selection)
                self.document['profiles'].insert(new, p)
                self.selection = new
            elif key == 'Escape':
                self.dialog = None
            elif key == 'Return':
                if self.focus == 'destinationActions':
                    if self.index == 0:
                        self.focus = 'destinationLabel'
                    elif self.index == 2:
                        self.dialog = 'remove'
                elif self.focus == 'addS3Destination':
                    self.dialog = 'add-b2' if self.index == 2 else 'add-s3'
                    self.focus = 'nameDialog'
                elif self.focus == 'nameDialog':
                    p = deepcopy(self.f['profile'])
                    p.update(label=self.pending, id=self.pending.lower().replace(' ', '-'),
                             provider='backblaze-b2' if self.dialog == 'add-b2' else 's3')
                    self.document['profiles'].append(p)
                    self.selection = len(self.document['profiles']) - 1
                    self.dialog = None
            elif key == 'Tab':
                self.commit_field()

    def commit_field(self):
        if not self.document['profiles']:
            return
        p = self.document['profiles'][self.selection]
        if self.focus == 'destinationLabel':
            p['label'] = self.pending
        if self.focus == 'credentialMode':
            p['credentials'] = {'mode': 'session' if self.index else 'keychain',
                                'reference': None if self.index else 'putgd/' + p['id']}
        if self.focus == 's3AclMode':
            p['settings']['aclMode'] = 'public-read' if self.index else 'omit'
        controls = {'s3Bucket': 'bucket', 's3RegionChoices': 'region', 's3Endpoint': 'endpoint',
                    's3ObjectPrefix': 'objectPrefix', 's3PublicUrlBase': 'publicUrlBase',
                    's3StorageClass': 'storageClass', 's3ConnectTimeoutMs': 'connectTimeoutMs'}
        if self.focus in controls:
            self.invalid = self.focus == 's3Bucket' and ' ' in self.pending
            if not self.invalid:
                value = int(self.pending)*1000 if self.focus == 's3ConnectTimeoutMs' else self.pending
                p['settings'][controls[self.focus]] = value

    def accessible(self, query):
        self.assert_event('accessible', query)
        self.expect(query.get('owned') is True, 'accessible reads are owned')
        if query.get('role') == 'list item':
            return [{'bounds': {'x': 10, 'y': i*60, 'width': 200, 'height': 60}}
                    for i in range(len(self.document['profiles']))]
        control = query['objectName']
        if control == 'destinationLabel':
            return [{'text': self.document['profiles'][self.selection]['label']}]
        if control == 's3BucketError':
            return [{'text': 'Invalid bucket' if self.invalid else ''}]
        if control == 'destinationConflictBanner':
            return [{'visible': self.conflict}]
        if control == 'addDestinationCredentials':
            return [{'visible': True}]
        if control == 'credentialAccessKey':
            return [{'states': {'focused': self.focus == 'addDestinationCredentials'}}]
        if control == 'destinationsEmpty':
            return [{'visible': not self.document['profiles']}]
        if control == 's3BucketHelp':
            return [{'states': {'checked': self.checked}}]
        if control == 'destinationStatus':
            return [{'text': 'Connection successful.' if self.tested else ''}]
        if control == 'destinationList':
            return [{'rect': [0, 0, 200, 240]}]
        raise AssertionError('unclassified accessibility read: ' + control)

    def drag(self, start, end, **kwargs):
        self.assert_event('drag', start, end)
        self.document['profiles'].append(self.document['profiles'].pop(0))

    def cli(self, *args, **kwargs):
        self.assert_event('cli', args)
        return {'returncode': 1, 'stdout': '{"error":{"code":"credentials_require_settings"}}', 'stderr': ''}


class DestinationsContracts(unittest.TestCase):
    def context(self):
        ctx = FakeContext()
        self.addCleanup(ctx.temporary.cleanup)
        return ctx

    def recipes(self):
        return [r for r in REGISTRY if r.function.__module__ == lane.__name__]

    def test_exact_ownership_once(self):
        owners = json.loads((Path(__file__).resolve().parents[2] /
                             'tools/lab/surface_driver/ownership.json').read_text())['owners']
        actual = [i for r in self.recipes() for i in r.interactions]
        self.assertEqual(set(actual), {i for i, owner in owners.items() if owner == 'destinations'})
        self.assertEqual(len(actual), len(set(actual)))

    def test_all_recipes_input_assertions_and_referenced_frames(self):
        blocked = {'credential_lifecycle'}
        for r in self.recipes():
            with self.subTest(recipe=r.name):
                ctx = self.context()
                if r.function.__name__ in blocked:
                    with self.assertRaises(RecipeBlocked) as caught:
                        r.function(ctx)
                    self.assertEqual(caught.exception.issue, 666)
                else:
                    r.function(ctx)
                    self.assertTrue(any(e[0] in ('click', 'cli', 'drag') for e in ctx.events))
                frames = [e for e in ctx.events if e[0] == 'frame']
                self.assertEqual(frames[0][1], 'before')
                self.assertIn(frames[-1][1], ('after', 'after-failure'))
                self.assertTrue(all(e[3] for e in frames))
                self.assertTrue(any(e[0] == 'shell' for e in ctx.events))
                calls = [e[1] for e in ctx.events if e[0] == 'call']
                self.assertFalse(any(x in ('setup.storeCredentials', 'setup.revealCredentialSecret') for x in calls))

    def test_refuse_live_or_ambiguous_fixture_before_mutation(self):
        for endpoint in ('https://s3.us-east-1.amazonaws.com', 'http://localhost:5000',
                         'http://127.0.0.1:5000/?secret=value', 'http://user@127.0.0.1:5000'):
            with self.subTest(endpoint=endpoint):
                ctx = self.context()
                ctx.f['profile']['settings']['endpoint'] = endpoint
                with self.assertRaises(RecipeBlocked):
                    lane.rename(ctx)
                self.assertFalse(any(e[:2] == ('call', 'settings.patch') for e in ctx.events))

    def test_private_config_required(self):
        ctx = self.context()
        ctx.f['private'] = False
        with self.assertRaises(RecipeBlocked):
            lane.rename(ctx)
        self.assertFalse(any(e[0] in ('click', 'view') for e in ctx.events))

    def test_target_private_root_verified_before_patch(self):
        ctx = self.context()
        ctx.private_root = False
        with self.assertRaises(RecipeBlocked):
            lane.rename(ctx)
        self.assertFalse(any(e[:2] == ('call', 'settings.patch') for e in ctx.events))

    def test_disk_disagreement_is_failure(self):
        ctx = self.context()
        ctx.corrupt_disk = True
        with self.assertRaises(RecipeFailed):
            lane.rename(ctx)

    def test_only_config_digest_enters_shell(self):
        ctx = self.context()
        lane.rename(ctx)
        scripts = [e[1] for e in ctx.events if e[0] == 'shell']
        self.assertTrue(scripts)
        self.assertTrue(all('Renamed surface destination' not in s for s in scripts))
        self.assertTrue(all('print(json.dumps' in s for s in scripts))

    def test_current_accessibility_uses_owned_object_names_and_states(self):
        ctx = self.context()
        lane.add_credentials(ctx)
        queries = [e[1] for e in ctx.events if e[0] == 'accessible']
        self.assertTrue(queries)
        self.assertTrue(all(q.get('owned') is True and 'objectName' in q for q in queries))
        self.assertFalse(any('control' in q or 'window' in q for q in queries))

    def test_missing_fixture_is_not_replaced_by_offline_glue(self):
        ctx = self.context()
        def missing(_name):
            raise RecipeBlocked('unknown fixture: destinations')
        ctx.fixture = missing
        with self.assertRaises(RecipeBlocked):
            lane.rename(ctx)
        self.assertFalse(any(e[:2] == ('call', 'settings.patch') for e in ctx.events))

    def test_ignored_bucket_input_cannot_pass_against_fixture_baseline(self):
        ctx = self.context()
        ctx.commit_field = lambda: None
        with self.assertRaisesRegex(RecipeFailed, 'bucket autosaved'):
            lane.s3_fields(ctx)

    def test_credential_entry_and_reveal_block_without_reading_fake_material(self):
        ctx = self.context()
        with self.assertRaises(RecipeBlocked) as caught:
            lane.credential_lifecycle(ctx)
        self.assertEqual(caught.exception.issue, 666)
        self.assertFalse(any(e[0] == 'type_text' for e in ctx.events))
        self.assertFalse(any(e[0] == 'shell' and 'fake-input' in e[1] for e in ctx.events))

    def test_lane_runs_with_local_destination(self):
        self.assertTrue(all(r.destinations == ('local',) for r in self.recipes()))

    def test_missing_confirmation_inventory_blocks_before_dialog_input(self):
        ctx = self.context()
        ctx.profile['catalog_windows'] = {}
        with self.assertRaises(RecipeBlocked) as caught:
            lane.make_default(ctx)
        self.assertEqual(caught.exception.issue, 722)
        self.assertFalse(any(e[:2] == ('click', 'makeDefaultDestination') for e in ctx.events))

    def test_dialog_titles_are_portable_and_select_real_inventories(self):
        root = Path(__file__).resolve().parents[2] / 'tools/lab/surface_driver/profiles'
        expected = {'confirmDestinationRemove': 'Remove destination',
                    'confirmDestinationDefault': 'Use an untested destination',
                    'confirmDestinationReload': 'Reload settings from file'}
        fragments = list(root.glob('*.d/destinations.json'))
        self.assertEqual(len(fragments), 9)
        for path in fragments:
            profile = json.loads(path.read_text())
            for role, title in expected.items():
                self.assertEqual(profile['window_titles'][role], title)
                self.assertTrue(fnmatch.fnmatchcase(title, '*' + profile['window_titles'][role] + '*'))
                self.assertEqual(profile['catalog_windows'][role], [{
                    'confirmDestinationRemove': 'confirm-remove-destination',
                    'confirmDestinationDefault': 'confirm-make-default-untested',
                    'confirmDestinationReload': '53-reload-confirm'}[role]])


if __name__ == '__main__':
    unittest.main()
