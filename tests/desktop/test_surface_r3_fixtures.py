"""Round-3 fixture safety and semantic selectors; no desktop/cloud access."""
import ast
import copy
from contextlib import contextmanager, redirect_stdout
import hashlib
import io
import json
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch
from urllib.error import HTTPError
from urllib.request import urlopen

sys.path.insert(0,str(Path(__file__).resolve().parents[2]))
from tests.desktop import test_surface_fixtures as fixture_tests
from tools.lab.surface_driver.api import RecipeBlocked
from tools.lab.surface_driver.catalog import Catalog
from tools.lab.surface_driver.fixtures import FACTORIES, desktop, lifecycle, multipart, settings, updates
from tools.lab.surface_driver.fixtures.base import Fixture
from tools.lab.surface_driver.fixtures.update_server import Server
from tools.lab.surface_driver.probe import selector_matches


class Round3(fixture_tests.Fixtures):
    def test_no_output_includes_defaults_and_all_inherited_still_routes(self):
        value=self.ctx.fixture('settings-no-output')
        self.assertFalse(self.ctx.document['archive']['keepUploads'])
        self.assertIsNone(self.ctx.document['captureDefaults']['profileId'])
        self.assertEqual(set(value.recovery_actions),{'chooseCaptureDestination','enableLocalCopies'})
        self.assertTrue(all(v['destination']=='default' and v['profileId'] is None for k,v in self.ctx.document['actions'].items() if k!='record'))

    def test_known_loopback_choices_and_route_removal_preserve_shortcuts(self):
        baseline=copy.deepcopy(self.ctx.document)
        service=Fixture(profile={'id':'owned','provider':'s3','settings':{'endpoint':'http://127.0.0.1:1234'},'credentials':{'reference':'synthetic'}})
        with patch.dict(FACTORIES,{'destinations':lambda scope:service}):
            value=self.ctx.fixture('settings-action-routes')
        self.assertEqual(value.profile_ids,['synthetic-0','synthetic-1','synthetic-2'])
        self.assertFalse(value.private_supported)
        value.add('region',value.profile_ids[1],'share')
        self.assertEqual(self.ctx.document['actions']['region']['publicationMode'],'share')
        value.remove('region')
        self.assertEqual(self.ctx.document['actions']['region']['destination'],'default')
        self.assertEqual(self.ctx.document['actions']['region']['shortcut'],baseline['actions']['region']['shortcut'])
        with self.assertRaises(ValueError):value.add('region','foreign')
        with self.assertRaises(ValueError):value.add('region',value.profile_ids[0],'private')
        self.ctx.manager.close()
        self.assertEqual(self.ctx.document,baseline)

    def test_save_fault_restores_mode_before_config_finalizer_even_on_expiry(self):
        parent=self.root/'config/putgd';parent.mkdir(mode=0o700)
        scope=self.ctx.manager.scope;original=scope.json
        def metadata(code,**kw):
            # Unit runner may be root. Production keeps the non-root refusal.
            return original(code.replace('assert os.geteuid()!=0',''),**kw)
        with patch.object(scope,'json',metadata):value=self.ctx.fixture('settings-save-failure')
        self.assertEqual(parent.stat().st_mode&0o777,0o500)
        value.restore();self.assertEqual(parent.stat().st_mode&0o777,0o700)
        parent.chmod(0o500);self.ctx._deadline=0
        self.assertEqual(self.ctx.manager.close(),[])
        self.assertEqual(parent.stat().st_mode&0o777,0o700)

    def test_input_fixture_uses_guards_and_releases_on_second_key_failure(self):
        events=[]
        @contextmanager
        def held(*keys):
            events.append(('down',keys))
            try:yield
            finally:events.append(('up',keys))
        self.ctx.held_keys=held
        def keys(key):events.append(('key',key));raise RuntimeError('synthetic input refusal')
        self.ctx.keys=keys
        value=self.ctx.fixture('settings-input-events')
        with self.assertRaises(RuntimeError):value.overlap('1','2')
        self.assertEqual(events,[('down',('Ctrl','Shift','1')),('key','2'),('up',('Ctrl','Shift','1'))])
        events.clear()
        with self.assertRaises(RuntimeError):value.shifted_digit(1)
        self.assertEqual(events,[('down',('Shift',)),('key','1'),('up',('Shift',))])
        with self.assertRaises(ValueError):value.shifted_digit('!')

    def test_unavailable_and_pending_are_never_invented(self):
        for name in ('settings-binding-unavailable','settings-binding-pending','settings-binding-unsupported'):
            with self.assertRaisesRegex(RecipeBlocked,'absent'):self.ctx.fixture(name)

    def test_shortcut_observation_uses_native_snapshot_and_retains_wire_shape(self):
        native={'available':True,'bindings':{'region':{'bound':False,'error':'shortcut_unsupported'}}}
        original=self.ctx.call
        self.ctx.call=lambda method,params=None: {'diagnostics':{'shortcuts':native}} if method=='ui.snapshot' else original(method,params)
        self.assertIs(desktop.shortcut_status(self.ctx),native)
        value=self.ctx.fixture('settings-binding-unsupported')
        self.assertIs(value.observe(),native)
        self.assertFalse(value.controlled)

    def test_recording_label_and_disclosure_are_observations(self):
        nodes=[{'name':'Help','objectName':'help','role':'toggle button','states':{'checked':True,'expanded':False},'text':'Help'},
               {'name':'Visible help content','role':'label','text':'Visible help content'}]
        self.ctx.catalog=Fixture(candidates=lambda *a:[{'name':'help','class':'QToolButton','accessibleName':'Help'}])
        self.ctx._captures={};self.ctx.geometry=lambda role:{'pid':42}
        self.ctx.accessible=lambda query:nodes if query.get('pid') else []
        value=self.ctx.fixture('settings-observations')
        self.assertTrue(value.disclosure('help').expanded)
        self.assertIn('Visible help content',value.disclosure('help').text)
        self.assertIsNone(value.recording_status())
        nodes.append(dict(nodes[0]))
        with self.assertRaisesRegex(RecipeBlocked,'one owned'):value.disclosure('help')

    def test_update_bundle_missing_is_before_profile_or_executable_mutation(self):
        with self.assertRaisesRegex(RecipeBlocked,'authentic tagged'):self.ctx.fixture('settings-update-lifecycle')
        self.assertEqual(self.ctx.calls,[])
        self.assertEqual(self.ctx.target.app_executable,'putgd')

    def test_recording_status_requires_current_accessible_text(self):
        row={'name':'Shortcut recording status','role':'label','text':'','description':''}
        self.ctx.accessible=lambda query:[row]
        value=self.ctx.fixture('settings-observations')
        with self.assertRaises(RecipeBlocked) as error:value.recording_status()
        self.assertEqual(error.exception.issue,862)
        row['description']='Recording canceled. The draft shortcut is unchanged.'
        self.assertEqual(value.recording_status().text,row['description'])

    def test_update_manifest_rejects_tampering_and_dirty_or_mismatched_builds(self):
        # Execute the exact target-side validator, mocking only the immutable
        # release's process response. No test asset is a signed release.
        script=next(v for v in updates.lifecycle.__code__.co_consts
                    if isinstance(v,str) and 'hashlib.file_digest' in v)
        image=self.root/'release.AppImage';image.write_bytes(b'unit validation input')
        build={'version':'26.1026.1416-nightly','dirty':False}
        manifest=self.root/'bundle.json'
        current={'path':str(image),'sha256':hashlib.sha256(image.read_bytes()).hexdigest(),
                 'tag':'v'+build['version'],'build':build}
        def validate(document,reported):
            manifest.write_text(json.dumps(document))
            with patch('subprocess.check_output',return_value=json.dumps(reported)) as run,redirect_stdout(io.StringIO()):
                exec(script.replace('BUNDLE',repr(str(manifest))),{})
                self.assertEqual(run.call_args.kwargs['env']['APPIMAGE_EXTRACT_AND_RUN'],'1')
        validate({'current':current,'releases':[]},build)
        dirty=dict(build,dirty=True)
        with self.assertRaises(AssertionError):validate({'current':dict(current,build=dirty),'releases':[]},dirty)
        with self.assertRaises(AssertionError):validate({'current':dict(current,tag='v26.1026.1-nightly'),'releases':[]},build)
        image.write_bytes(b'tampered unit input')
        with self.assertRaises(AssertionError):validate({'current':current,'releases':[]},build)

    def test_uncached_destination_restarts_only_private_resident(self):
        service=Fixture(profile={'id':'synthetic','provider':'s3','settings':{}},private=True)
        with patch.dict(FACTORIES,{'loopback-s3':lambda scope:service}):
            value=self.ctx.fixture('destinations-uncached')
            self.assertFalse(value.credentials_cached)
            value.reset_credentials()
        from tools.lab.surface_driver.fixtures.base import Scope
        self.assertEqual(Scope.restart.call_args.args,({},))

    def test_external_focus_requests_explicit_raise_and_restores_role(self):
        scope=self.ctx.manager.scope
        self.ctx.fixture('app-profile')
        previous={'settings':{'title':'put.gd'}}
        self.ctx.profile['windows']=previous
        from unittest.mock import Mock
        self.ctx.focus=Mock(return_value={});self.ctx.windows=lambda query:[{'pid':42}]
        with patch.object(scope,'launch',return_value={'pid':42}):value=self.ctx.fixture('settings-focus-loss')
        value.focus();value.restore()
        self.assertEqual(self.ctx.focus.call_args_list[-1].kwargs,{'raise_window':True})
        self.assertIn(value.role,self.ctx.profile['windows'])
        self.ctx.manager.close()
        self.assertIs(self.ctx.profile['windows'],previous)

    def test_recording_lifecycle_tracks_each_fresh_prepared_id(self):
        scene=Fixture(prepare={'target':{'kind':'region','rect':[0,0,640,320]}},command=lambda v:None,output_dir='/owned/out')
        self.ctx.call_original=self.ctx.call
        ids=iter(['r1','r2'])
        self.ctx.call=lambda method,params=None: {'recordingId':next(ids)} if method=='recording.prepare' else self.ctx.call_original(method,params)
        with patch.dict(FACTORIES,{'events.recording-source':lambda s:scene}):
            value=self.ctx.fixture('recording-lifecycle')
            value.prepare_recording();value.prepare_recording()
        self.assertEqual(value.recording_ids,['r1','r2'])
        self.ctx.manager.close()
        self.assertEqual([p['id'] for m,p in self.ctx.calls if m=='recording.cancel'],['r2','r1'])

    def test_missing_gtk_sink_fails_before_recording_or_pattern(self):
        self.ctx.fixture('app-profile')
        with patch.object(self.ctx,'shell',return_value={'returncode':1}),patch('tools.lab.surface_driver.fixtures.recordings.pattern') as pattern:
            with self.assertRaisesRegex(RecipeBlocked,'gstreamer1.0-gtk3'):lifecycle.source(self.ctx.manager.scope)
            pattern.assert_not_called()

    def test_locked_keyring_only_addresses_its_owned_collection(self):
        from tools.lab.surface_driver.fixtures import destinations
        value=Fixture(collection='/org/freedesktop/secrets/collection/owned',private=True)
        scope=self.ctx.manager.scope;commands=[]
        with patch.dict(FACTORIES,{'fake-keyring':lambda s:value}),patch.object(scope,'python',lambda code:commands.append(code)):
            locked=self.ctx.fixture('locked-keyring');locked.unlock();self.ctx.manager.close()
        self.assertTrue(locked.locked)
        self.assertTrue(all(value.collection in code for code in commands))
        self.assertTrue(any("'Lock'" in code for code in commands))
        self.assertTrue(any("'Unlock'" in code for code in commands))
        self.assertFalse(any('SetAlias' in code or 'GetSecret' in code for code in commands))

    def test_multipart_ages_only_owned_stopped_operation_and_uses_automatic_result(self):
        import sqlite3
        scope=self.ctx.manager.scope
        self.ctx.fixture('app-profile')
        database=Path(scope.root)/'data/putgd/history.sqlite';database.parent.mkdir()
        original={'id':'owned-op','state':'failed','created':'2026-10-30T00:00:00.000Z',
                  'items':[{'uploadStatus':'failed','uploadJournal':{'uploadId':'real-sdk-upload'},
                            'error':{'details':{'incompleteUploadReported':True}}}]}
        sibling=dict(original,id='sibling')
        with sqlite3.connect(database) as db:
            db.execute('CREATE TABLE operations(id TEXT,data TEXT)')
            db.executemany('INSERT INTO operations VALUES (?,?)',[(o['id'],json.dumps(o)) for o in (original,sibling)])
        previous_call=self.ctx.call
        def call(method,params=None):
            if method=='setup.unlockCredentials':return {'setupId':'owned-setup'}
            if method=='setup.inspect':return {'state':'completed'}
            if method=='diagnostics':return {'artifactCleanup':{'last':{'trigger':'automatic','incompleteUploads':[{'operationId':'owned-op'}]}}}
            return previous_call(method,params)
        self.ctx.call=call
        previous_json=scope.json
        def metadata(code,**kw):
            ast.parse(code)
            if 'urllib.request' in code:return [{'uploadId':'real-sdk-upload','key':'synthetic'}]
            return previous_json(code,**kw)
        def restart(env=None,*,before=None):
            self.assertEqual(env,{})
            before()
        with (patch.object(multipart,'incomplete_multipart',return_value=Fixture(operationId='owned-op',itemId='owned-item')),
             patch.dict(FACTORIES,{'loopback-s3':lambda s:Fixture(endpoint='http://127.0.0.1:1234',profile_id='synthetic')}),
             patch.object(scope,'json',metadata),patch.object(scope,'restart',restart)):
            value=self.ctx.fixture('events.expired-multipart')
            self.assertEqual(value.automatic_cleanup()['trigger'],'automatic')
            with self.assertRaises(ValueError):value.automatic_cleanup()
        with sqlite3.connect(database) as db:
            aged=json.loads(db.execute('SELECT data FROM operations WHERE id=?',('owned-op',)).fetchone()[0])
            retained=json.loads(db.execute('SELECT data FROM operations WHERE id=?',('sibling',)).fetchone()[0])
        self.assertEqual(retained,sibling)
        self.assertEqual(aged['items'][0]['uploadJournal'],original['items'][0]['uploadJournal'])
        self.assertNotEqual(aged['created'],original['created'])
        self.assertNotIn('incompleteUploadReported',aged['items'][0]['error']['details'])
        self.assertFalse(any(m=='cache.clear' for m,p in self.ctx.calls))


class Selectors(unittest.TestCase):
    def test_combo_class_rejects_same_named_label_but_keeps_exact_id(self):
        candidate={'name':'lineWidth','class':'QComboBox','accessibleNames':['Line width']}
        self.assertIsNone(selector_matches({'name':'Line width','role':'label'},'lineWidth',[candidate]))
        self.assertEqual(selector_matches({'name':'Line width','role':'combo box'},'lineWidth',[candidate]),1)
        self.assertEqual(selector_matches({'objectName':'lineWidth','name':'different','role':'combo box'},'lineWidth',[candidate]),0)

    def test_dynamic_recent_colour_hints_are_strict_and_editor_scoped(self):
        with tempfile.TemporaryDirectory() as root:
            catalog=Catalog(root,{})
            candidates=catalog.candidates('recentColorSwatch_123456','editor')
            self.assertEqual(len(candidates),1)
            self.assertEqual(selector_matches({'name':'Color #123456','role':'toggle button'},'recentColorSwatch_123456',candidates),1)
            self.assertIsNone(selector_matches({'name':'Color #123456','role':'label'},'recentColorSwatch_123456',candidates))
            for selector in ('recentColorSwatch_12345','recentColorSwatch_ABCDEF','recentColorSwatch_123456extra'):
                self.assertEqual(catalog.candidates(selector,'editor'),[])
            self.assertEqual(catalog.candidates('recentColorSwatch_123456','settings'),[])


class Ledger(unittest.TestCase):
    def test_request_ledger_excludes_raw_urls_headers_queries_and_traversal(self):
        with tempfile.TemporaryDirectory() as root:
            root=Path(root);(root/'nightly.json').write_text('{}')
            server=Server(root);thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
            url='http://127.0.0.1:'+str(server.server_port)
            try:
                self.assertEqual(urlopen(url+'/nightly.json?private=must-not-log').read(),b'{}')
                with self.assertRaises(HTTPError):urlopen(url+'/../foreign')
                ledger=json.load(urlopen(url+'/__ledger'))
                self.assertEqual({k:v for k,v in ledger[0].items() if k!='privacy'},
                                 {'sequence':1,'method':'GET','route':'feed','channel':'nightly','status':200,'bytes':2})
                self.assertFalse(ledger[0]['privacy']['queryEmpty'])
                self.assertTrue(ledger[0]['privacy']['authorizationAbsent'])
                self.assertNotIn('private',json.dumps(ledger))
                self.assertEqual(ledger[1]['status'],404)
            finally:server.shutdown();server.server_close();thread.join()


if __name__=='__main__':unittest.main()
