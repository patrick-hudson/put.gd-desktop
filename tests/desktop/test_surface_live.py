"""Live-lane recipe contracts; synthetic Context/browser, never desktop proof.

Optional --moto-endpoint exercises the fake Context's object lifecycle against a
loopback moto server on Boat, without provider credentials or AWS SDK imports.
"""
from contextlib import contextmanager
from copy import deepcopy
from datetime import datetime, timedelta, timezone
import json
from pathlib import Path
import sys
import types
import unittest
from unittest.mock import patch
from urllib.error import HTTPError
from urllib.request import Request, urlopen

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.lab.surface_driver.api import Context, REGISTRY, RecipeBlocked, RecipeFailed
from tools.lab.surface_driver.recipes import live

MOTO_ENDPOINT = None
if '--moto-endpoint' in sys.argv:
    index = sys.argv.index('--moto-endpoint')
    MOTO_ENDPOINT = sys.argv[index + 1]
    del sys.argv[index:index + 2]
    if not MOTO_ENDPOINT.startswith('http://127.0.0.1:'):
        raise SystemExit('moto endpoint must be loopback')


class FakeContext(Context):
    host, placement, destination = 'boat', 'boat', 'proton'
    desktop, package, profile = 'cinnamon-x11', 'source', {}

    def __init__(self, destination='proton'):
        self.destination = destination
        self.fixture_data = dict(configured=True, isolated=True, sensitive_transport=True, redacted_frames=True,
                                 profile_id='campaign', profile_label='Synthetic campaign', folder_name='fresh-folder',
                                 expected_host='fixture.invalid', object_prefix='fresh-prefix/',
                                 region_start=[10, 10], region_end=[160, 110], window_focus=[90, 40],
                                 region_shortcut='Meta+Shift+R', window_shortcut='Meta+Shift+W',
                                 image_path='/tmp/synthetic.png', editor_mark_start=[40, 40], editor_mark_end=[60, 60],
                                 new_folder_name='new-folder', custom_domain='https://custom.invalid',
                                 public_read_supported=True, bucket_policy_supported=True)
        self.document = {'profiles': [{'id': 'campaign', 'label': 'Synthetic campaign',
            'provider': {'proton': 'proton-drive', 's3': 's3', 'b2': 'backblaze-b2'}[destination],
            'settings': {'folderName': 'fresh-folder', 'folderUid': 'folder-old', 'shareByDefault': True,
                         'linkExpirationDays': 1, 'requestTimeoutMs': 120000, 'sharePasswordReference': None,
                         'objectPrefix': 'fresh-prefix/', 'aclMode': 'omit', 'publicUrlBase': '',
                         'linkMode': 'public', 'linkValidityDays': 7}}], 'actions': {}}
        self.calls, self.inputs, self.notes, self.frames = [], [], [], []
        self.operations, self.details, self.shares = {}, {}, {}
        self.selected, self.current_control, self.text, self.clipboard = None, None, '', 'synthetic sentinel'
        self.editor_id, self.pending_revoke, self.pending_delete = None, False, False
        self.browser_installed, self.browser_restored, self.browser_checks = False, False, []
        self.revision, self.setup_reads = 0, {}
        self.upload_fails, self.wrong_selection, self.delete_outcome = False, False, 'removed'
        self.moto_keys = []
        self.moto_enabled = False
        self.intercept = None

    @property
    def settings(self):
        return self.document['profiles'][0]['settings']

    def fixture(self, name):
        self.expect(name == 'live', 'known fixture')
        return self.fixture_data

    def new_operation(self, editing=False):
        oid = 'operation-' + str(len(self.operations) + 1)
        self.operations[oid] = {'id': oid, 'state': 'editing' if editing else 'completed'}
        if not editing:
            self.finish_upload(oid)
        return oid

    def finish_upload(self, oid):
        self.operations[oid]['state'] = 'failed' if self.upload_fails else 'completed'
        hid = 'history-' + oid
        mode = self.document['actions']['edit' if oid == self.editor_id else 'region']['publicationMode']
        shared = mode == 'share'
        domain = self.settings.get('publicUrlBase') or 'https://fixture.invalid'
        detail = {'id': hid, 'operationId': oid, 'uploadStatus': 'uploaded', 'name': oid + '.png',
                  'shareStatus': 'shared' if shared else 'private', 'linkAvailable': shared,
                  'receipt': {'directUrl': domain + '/synthetic-only' if shared else ''}, 'remoteStatus': 'not_requested'}
        self.details[hid] = detail
        if self.moto_enabled:
            key = 'fresh-prefix/' + hid
            self.http('PUT', '/putgd-live-fixture/' + key, b'synthetic capture')
            self.moto_keys.append(key)

    def http(self, method, path, data=None):
        with urlopen(Request(MOTO_ENDPOINT + path, data=data, method=method,
                                   headers={'Authorization': 'AWS fixture-only:fixture-only'}), timeout=10) as response:
            return response.read()

    def view(self, method, params=None):
        return self.call(method, params)

    def call(self, method, params=None):
        params = params or {}
        self.calls.append((method, deepcopy(params)))
        if self.intercept:
            value = self.intercept(method, params)
            if value is not None:
                return value
        if method == 'settings.get':
            return {'document': deepcopy(self.document), 'metadata': {'observedRevision': str(self.revision)}}
        if method == 'settings.patch':
            self.expect(params['expectedRevision'] == str(self.revision), 'revision precondition')
            self.document['actions'].update(params['patch']['actions'])
            self.revision += 1
            return {}
        if method == 'operation.recent':
            return {'operations': deepcopy(list(self.operations.values()))}
        if method == 'operation.inspect':
            return deepcopy(self.operations[params['id']])
        if method == 'history.list':
            return {'records': deepcopy(list(self.details.values()))}
        if method == 'history.detail':
            return deepcopy(self.details[params['id']])
        if method == 'ui.show':
            return {'shown': params['view']}
        if method == 'ui.history':
            if params['action'] == 'select':
                self.selected = params['ids'][0]
            if params['action'] == 'copyDeletionDetails':
                self.clipboard = json.dumps({'targets': [{'historyId': self.selected}], 'acknowledgement': 'synthetic-digest'})
            return {'selectedIds': ['wrong'] if self.wrong_selection else [self.selected]}
        if method == 'editor.open':
            self.editor_id = self.new_operation(editing=True)
            return {'operationId': self.editor_id}
        if method == 'editor.inspect':
            return {'shapes': [{'id': 1}]}
        if method == 'recording.prepare':
            self.editor_id = self.new_operation(editing=True)
            return {'operationId': self.editor_id, 'recordingId': self.editor_id}
        if method == 'recording.inspect':
            state = 'ready'
            if ('click', 'record', 'bar') in self.inputs:
                state = 'recording'
            if ('click', 'stop', 'bar') in self.inputs:
                state = 'reviewing'
            return {'selecting': True, 'state': state, 'durationUs': 2_000_000}
        if method == 'share.inspect':
            return deepcopy(self.shares[params['id']])
        raise AssertionError('unhandled method ' + method)

    def click(self, control, window, **kwargs):
        self.inputs.append(('click', control, window))
        self.current_control = control
        self.text = ''
        if control in ('editorUpload', 'upload'):
            self.finish_upload(self.editor_id)
        if control == 'shareHistoryLink':
            self.set_share('shared')
        if control == 'inspectHistoryLink':
            self.set_share(self.details[self.selected]['shareStatus'])
        if control == 'revokeHistoryLink':
            self.pending_revoke = True
        if control == 'confirmDiscard':
            self.expect(self.pending_revoke, 'revoke confirmation open')
            self.pending_revoke = False
            self.set_share('revoked')
        if control == 'copyHistoryLink':
            self.clipboard = self.details[self.selected]['receipt']['directUrl']
        if control == 'deleteHistoryRemote':
            self.pending_delete = True
        if control == 'confirmActionButton':
            self.expect(self.pending_delete, 'delete confirmation open')
            d = self.details[self.selected]
            d['remoteStatus'], d['linkAvailable'] = self.delete_outcome, False
            d['deleteOperationId'] = 'deletion-owned'
            if self.moto_enabled and self.delete_outcome == 'removed':
                key = 'fresh-prefix/' + self.selected
                self.http('DELETE', '/putgd-live-fixture/' + key)
                self.moto_keys.remove(key)
                with self.assert_http_missing('/putgd-live-fixture/' + key):
                    pass
        if control in ('protonKeepPrivate', 'protonCreateShareLink'):
            self.settings['shareByDefault'] = control == 'protonCreateShareLink'
        if control == 'clearProtonPassword':
            self.settings['sharePasswordReference'] = None
        if control in ('testDestination', 'inspectProtonAccount'):
            self.setup_reads[control] = 0

    @contextmanager
    def assert_http_missing(self, path):
        try:
            self.http('GET', path)
        except HTTPError as error:
            self.expect(error.code == 404, 'moto origin returned exact not-found')
        else:
            raise RecipeFailed('moto object still exists')
        yield

    def set_share(self, status):
        sid = 'sharing-' + str(len(self.shares) + 1)
        expires = (datetime.now(timezone.utc) + timedelta(days=1)).isoformat()
        d = self.details[self.selected]
        expires = d.get('shareExpiresAt') or expires
        d.update(shareStatus=status, shareOperationId=sid, linkAvailable=status == 'shared', shareExpiresAt=expires)
        d['receipt']['directUrl'] = 'https://fixture.invalid/synthetic-only' if status == 'shared' else ''
        self.shares[sid] = {'state': 'completed', 'status': status, 'expiresAt': expires}

    def keys(self, *sequence):
        self.inputs.append(('keys', sequence))
        if sequence[0] in ('Meta+Shift+R', 'Meta+Shift+W'):
            self.new_operation()
        if sequence == ('Escape',):
            self.pending_revoke = False
        field = {'protonExpiration': ('linkExpirationDays', 1), 'protonRequestTimeout': ('requestTimeoutMs', 120000),
                 'b2LinkDays': ('linkValidityDays', 1)}.get(self.current_control)
        if field and sequence == ('Tab',):
            self.settings[field[0]] = field[1]
        if sequence[-1] == 'Return' and self.current_control == 'b2LinkMode':
            self.settings['linkMode'] = 'public' if sequence[0] == 'Home' else 'authorized'
        if sequence[-1] == 'Return' and self.current_control == 's3AclMode':
            self.settings['aclMode'] = 'omit' if sequence[0] == 'Home' else 'public-read'
        if sequence == ('Tab',) and self.current_control == 's3PublicUrlBase':
            self.settings['publicUrlBase'] = self.text
        if sequence == ('Return',) and self.current_control == 'createProtonFolder':
            self.settings.update(folderName=self.text, folderUid='new-uid')

    def type_text(self, text):
        self.text = text
        # Deliberately do not retain typed content in the event trace.
        self.inputs.append(('type_text', '<private>'))

    def click_at(self, *point, **kwargs):
        self.inputs.append(('click_at', point))

    def drag(self, *points, **kwargs):
        self.inputs.append(('drag', points))

    def geometry(self, window):
        return {'x': 0, 'y': 0, 'width': 300, 'height': 200, 'scale': 1}

    def wait(self, predicate, what, **kwargs):
        for _ in range(5):
            value = predicate()
            if value:
                return value
        raise RecipeFailed(what)

    def accessible(self, query):
        if 'objectName' not in query:
            return [{'name': 'Synthetic modal/browser'}]
        name = query['objectName']
        if name == 'settingsStatus':
            return [{'text': 'Folders loaded.'}]
        if name == 'advancedSettings':
            return [{'checked': False}]
        if name in self.setup_reads:
            count = self.setup_reads[name]
            self.setup_reads[name] += 1
            return [{'enabled': count > 0}]
        return [{'text': 'Connection successful.'}]

    def notifications(self):
        d = list(self.details.values())[-1]
        return [{'summary': 'put.gd complete', 'body': d['name'], 'actions': ['default', 'Open']}]

    def mcp(self, tool, arguments=None):
        self.inputs.append(('mcp', tool, arguments))
        return {'ok': True, 'result': {'opened': True}}

    def clipboard_text(self):
        return self.clipboard

    def frame(self, label, window=None, reference=None):
        self.frames.append((label, window, reference))
        return None

    def note(self, text):
        self.notes.append(text)


@contextmanager
def browser_stub(ctx, browser):
    ctx.expect(browser == 'firefox', 'Firefox selected')
    ctx.browser_installed = True
    try:
        yield
    finally:
        ctx.browser_installed = False
        ctx.browser_restored = True


def verify_stub(ctx, trigger, expect):
    ctx.expect(ctx.browser_installed, 'temporary handler installed before trigger')
    trigger()
    ctx.browser_checks.append(expect)
    return {'matched': True, 'url_host': 'custom.invalid' if ctx.settings['publicUrlBase'] else 'fixture.invalid',
            'title': 'synthetic fixture', 'frames': ['full-desktop.png']}


class LiveRecipes(unittest.TestCase):
    def setUp(self):
        browser = types.ModuleType('tools.lab.surface_driver.browser')
        browser.default_browser, browser.open_and_verify = browser_stub, verify_stub
        self.browser_patch = patch.dict(sys.modules, {'tools.lab.surface_driver.browser': browser})
        self.browser_patch.start()
        self.addCleanup(self.browser_patch.stop)

    def test_all_owned_ids_have_recipes_and_all_claims_are_catalog_ids(self):
        owners = json.loads((ROOT / 'tools/lab/surface_driver/ownership.json').read_text())['owners']
        rows = [r for r in REGISTRY if r.function.__module__ == live.__name__]
        claimed = {i for r in rows for i in r.interactions}
        self.assertLessEqual({i for i, owner in owners.items() if owner == 'live'}, claimed)
        self.assertLessEqual(claimed, set(owners))

    def test_guard_blocks_before_any_input_or_resident_call(self):
        for flag in ('configured', 'isolated', 'sensitive_transport', 'redacted_frames'):
            ctx = FakeContext()
            ctx.fixture_data[flag] = False
            with self.subTest(flag=flag), self.assertRaises(RecipeBlocked):
                live.upload_region(ctx)
            self.assertEqual(ctx.inputs, [])
            self.assertEqual(ctx.calls, [])

    def test_proton_needs_same_builder_guest_and_lock(self):
        ctx = FakeContext()
        ctx.host, ctx.placement = 'f44', 'guest'
        with self.assertRaises(RecipeBlocked):
            live.upload_region(ctx)
        ctx.fixture_data.update(proton_lock_held=True, bridge_lent=True)
        live.upload_region(ctx)
        ctx.host = 'k72'
        with self.assertRaises(RecipeBlocked):
            live.upload_region(ctx)

    def test_four_sources_submit_via_real_input(self):
        for destination in ('proton', 's3', 'b2'):
            for function in (live.upload_region, live.upload_window, live.upload_editor, live.upload_recording):
                with self.subTest(destination=destination, function=function.__name__):
                    ctx = FakeContext(destination)
                    function(ctx)
                    self.assertEqual(len(ctx.fixture_data['owned_history_ids']), 1)
                    self.assertTrue(ctx.inputs)
                    self.assertTrue(ctx.frames)
                    self.assertFalse(any(method in ('action.invoke', 'editor.submit', 'recording.submit') for method, _ in ctx.calls))

    def test_failed_upload_is_never_credited(self):
        ctx = FakeContext('s3')
        ctx.upload_fails = True
        with self.assertRaises(RecipeFailed):
            live.upload_region(ctx)

    def test_each_browser_entry_point_and_full_desktop_contract(self):
        for function in (live.notification_link, live.history_link, live.row_menu_link, live.mcp_link):
            ctx = FakeContext()
            with self.subTest(function=function.__name__):
                function(ctx)
                self.assertTrue(ctx.browser_restored)
                self.assertEqual(len(ctx.browser_checks), 1)
                self.assertTrue(ctx.browser_checks[0]['preview'])
                self.assertNotIn('http', json.dumps(ctx.notes + ctx.frames))

    def test_clipboard_entry_never_types_url_into_unfocused_window(self):
        ctx = FakeContext()
        with self.assertRaises(RecipeBlocked):
            live.clipboard_link(ctx)
        self.assertTrue(ctx.browser_restored)
        self.assertNotIn(('keys', ('Ctrl+L',)), ctx.inputs)

    def test_current_core_accessibility_shape_is_blocked(self):
        ctx = FakeContext()
        ctx.accessible = lambda query: [{'name': 'status', 'role': 'label', 'rect': [1, 2, 30, 40], 'attributes': {}, 'pid': 123}]
        with self.assertRaises(RecipeBlocked):
            live.account_folder(ctx)

    def test_browser_failure_restores_handler_and_cannot_pass(self):
        ctx = FakeContext()
        module = sys.modules['tools.lab.surface_driver.browser']
        module.open_and_verify = lambda *a, **k: {'matched': False, 'frames': ['failure.png']}
        with self.assertRaises(RecipeFailed):
            live.history_link(ctx)
        self.assertTrue(ctx.browser_restored)

    def test_mcp_rejected_open_is_not_browser_success(self):
        ctx = FakeContext()
        ctx.mcp = lambda *a, **k: {'ok': False, 'error': {'code': 'link_not_opened'}}
        with self.assertRaises(RecipeFailed):
            live.mcp_link(ctx)
        self.assertTrue(ctx.browser_restored)

    def test_extra_destination_blocks_settings_mutation(self):
        ctx = FakeContext()
        ctx.document['profiles'].append({'id': 'unrelated', 'provider': 's3'})
        with self.assertRaises(RecipeFailed):
            live.upload_region(ctx)
        self.assertFalse(any(method == 'settings.patch' for method, _ in ctx.calls))

    def test_wrong_history_selection_stops_before_link_input(self):
        ctx = FakeContext()
        ctx.wrong_selection = True
        with self.assertRaises(RecipeFailed):
            live.history_link(ctx)
        self.assertNotIn(('click', 'openHistoryLink', 'history'), ctx.inputs)

    def test_private_upload_inspected_and_clipboard_unchanged(self):
        ctx = FakeContext()
        live.private_upload(ctx)
        self.assertEqual(ctx.clipboard, 'synthetic sentinel')
        self.assertIn(('click', 'inspectHistoryLink', 'history'), ctx.inputs)
        self.assertEqual(list(ctx.shares.values())[-1]['status'], 'private')

    def test_unsupported_providers_do_not_upload_to_fake_private_or_sharing(self):
        for destination in ('s3', 'b2'):
            for function in (live.private_upload, live.history_sharing):
                ctx = FakeContext(destination)
                with self.subTest(destination=destination), self.assertRaises(RecipeBlocked):
                    function(ctx)
                self.assertEqual(ctx.operations, {})

    def test_history_sharing_verifies_expiry_and_revokes_on_browser_failure(self):
        ctx = FakeContext()
        module = sys.modules['tools.lab.surface_driver.browser']
        def fail(*a, **kw):
            raise RecipeFailed('synthetic browser failure')
        module.open_and_verify = fail
        with self.assertRaises(RecipeFailed):
            live.history_sharing(ctx)
        self.assertEqual(list(ctx.details.values())[-1]['shareStatus'], 'revoked')
        self.assertIn(('keys', ('Escape',)), ctx.inputs)
        self.assertIn(('click', 'confirmDiscard', 'confirmAction'), ctx.inputs)

    def test_bad_expiry_revokes_created_link_before_failure(self):
        ctx = FakeContext()
        original = ctx.set_share
        def wrong_expiry(status):
            original(status)
            if status == 'shared':
                ctx.details[ctx.selected]['shareExpiresAt'] = '2000-01-01T00:00:00Z'
                ctx.shares[list(ctx.shares)[-1]]['expiresAt'] = '2000-01-01T00:00:00Z'
        ctx.set_share = wrong_expiry
        with self.assertRaises(RecipeFailed):
            live.history_sharing(ctx)
        self.assertEqual(list(ctx.details.values())[-1]['shareStatus'], 'revoked')

    def test_revoked_browser_gate_stays_blocked(self):
        ctx = FakeContext()
        with self.assertRaises(RecipeBlocked):
            live.revoke_confirmation(ctx)
        self.assertEqual(list(ctx.details.values())[-1]['shareStatus'], 'revoked')

    def test_setup_requires_observed_busy_and_fresh_success(self):
        ctx = FakeContext()
        live.account_folder(ctx)
        self.assertEqual(ctx.setup_reads['testDestination'], 2)
        ctx.accessible = lambda query: [{'enabled': True, 'text': 'Connection successful.'}]
        with self.assertRaises(RecipeFailed):
            live.account_folder(ctx)

    def test_no_signin_or_password_entry(self):
        ctx = FakeContext()
        with self.assertRaises(RecipeBlocked):
            live.proton_connect(ctx)
        self.assertEqual(ctx.inputs, [])
        with self.assertRaises(RecipeBlocked):
            live.proton_sharing(ctx)
        self.assertFalse(any(e[:2] == ('click', 'connectProtonAccount') for e in ctx.inputs))
        self.assertEqual(ctx.settings['linkExpirationDays'], 1)

    def test_refresh_preserves_folder_then_creates_new_identity(self):
        ctx = FakeContext()
        live.proton_folder(ctx)
        self.assertEqual(ctx.settings['folderName'], 'new-folder')
        self.assertEqual(ctx.settings['folderUid'], 'new-uid')

    def test_b2_missing_configuration_blocks(self):
        ctx = FakeContext('b2')
        ctx.fixture_data['configured'] = False
        with self.assertRaises(RecipeBlocked):
            live.b2_links(ctx)
        self.assertEqual(ctx.inputs, [])

    def test_b2_access_and_expiry_inputs(self):
        ctx = FakeContext('b2')
        live.b2_links(ctx)
        self.assertEqual(ctx.settings['linkMode'], 'authorized')
        self.assertEqual(ctx.settings['linkValidityDays'], 1)

    def test_cleanup_exact_preparation_and_negative_gate(self):
        ctx = FakeContext('s3')
        with self.assertRaises(RecipeBlocked):
            live.cleanup(ctx)
        self.assertEqual(list(ctx.details.values())[-1]['remoteStatus'], 'removed')
        self.assertIn(('click', 'confirmActionButton', 'confirmAction'), ctx.inputs)
        ctx = FakeContext()
        with self.assertRaises(RecipeBlocked):
            live.cleanup(ctx)
        self.assertEqual(ctx.operations, {})

    def test_cleanup_foreign_selection_and_unknown_outcome_fail(self):
        for outcome in ('unknown', 'changed', 'hidden_version_retained'):
            ctx = FakeContext('s3')
            ctx.delete_outcome = outcome
            with self.subTest(outcome=outcome), self.assertRaises(RecipeFailed):
                live.cleanup(ctx)
        ctx = FakeContext('s3')
        ctx.clipboard_text = lambda: json.dumps({'targets': [{'historyId': 'foreign'}], 'acknowledgement': 'digest'})
        with self.assertRaises(RecipeFailed):
            live.cleanup(ctx)
        self.assertNotIn(('click', 'confirmActionButton', 'confirmAction'), ctx.inputs)

    def test_s3_modes_and_custom_domain_restore_settings(self):
        ctx = FakeContext('s3')
        live.s3_access(ctx)
        self.assertEqual(ctx.settings['aclMode'], 'omit')
        self.assertEqual(ctx.settings['publicUrlBase'], '')
        self.assertEqual(len(ctx.browser_checks), 3)
        self.assertTrue(ctx.browser_restored)
        self.assertFalse(any('bucketPolicy' in json.dumps(p) for _, p in ctx.calls))

    @unittest.skipUnless(MOTO_ENDPOINT, 'optional Boat moto fixture')
    def test_moto_object_deleted_via_recipe_fake_app(self):
        ctx = FakeContext('s3')
        ctx.moto_enabled = True
        ctx.http('PUT', '/putgd-live-fixture', b'')
        try:
            with self.assertRaises(RecipeBlocked):
                live.cleanup(ctx)
            self.assertEqual(ctx.moto_keys, [])
        finally:
            for key in ctx.moto_keys:
                ctx.http('DELETE', '/putgd-live-fixture/' + key)
            ctx.http('DELETE', '/putgd-live-fixture')


if __name__ == '__main__':
    unittest.main()
