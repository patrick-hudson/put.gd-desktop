"""Pure host contracts; no VM, service or graphical acceptance is implied."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('tray_progress_lab', ROOT / 'tools/lab/tray_progress.py')
tray = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tray)


def observation(description, color='#62d69b', title='put.gd — Working'):
    return {'pid': 42, 'hostRegistered': True, 'registered': True,
            'title': title, 'description': description,
            'icons': [{'width': 24, 'height': 24, 'sha256': 'synthetic',
                       'opaqueColors': {color: 120}}]}


def mock_reads(replay):
    replay.read_observation = lambda: (replay.cli('operation.recent')['operations'],
        replay.snapshot(), replay.cli('operation.recent')['operations'])


class TrayProgressContracts(unittest.TestCase):
    def test_resolved_socket_byte_limit_includes_guest_runtime_suffix(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            short = root / 'tp'
            paths = tray.socket_paths(short, root / 'qmp.sock')
            self.assertIn(str(short / 'r/putgd/session.sock'), paths)
            self.assertTrue(all(item['utf8Bytes'] <= 107 for item in paths.values()))
            # Character count is insufficient for Unix-domain sockets.
            with self.assertRaisesRegex(tray.TrayProgressError, '107'):
                tray.socket_paths(root / ('é' * 50), root / 'qmp.sock')
            self.assertFalse(short.exists())

    def test_s3_counter_is_bounded_by_real_observations_and_percentage_matches(self):
        records = [{'phase': 'uploading', 'sourceBytes': 4*1024**2,
                    'bytesTransferred': n*1024**2, 'bytesTotal': 4*1024**2}
                   for n in (1, 2, 3)]
        self.assertTrue(tray.check_tooltip(observation('Uploading: 2 MiB of 4 MiB (50%)'), records, 's3', 42)['counter'])
        for text in ('Uploading: 5 MiB of 4 MiB (125%)',
                     'Uploading: 2 MiB of 4 MiB (90%)',
                     'Uploading: 2 MiB of 8 MiB (25%)'):
            with self.subTest(text=text), self.assertRaises(tray.TrayProgressError):
                tray.check_tooltip(observation(text), records, 's3', 42)

    def test_proton_counter_keeps_real_bytes_and_labels_source_estimate(self):
        # The SDK may retry and decrease its encrypted byte counter.
        records = [{'phase': 'uploading', 'sourceBytes': 4*1024**2,
                    'bytesTransferred': n*1024**2} for n in (8, 6)]
        result = tray.check_tooltip(observation('Uploading: 7 MiB transferred · Source size: 4 MiB'), records, 'proton-drive', 42)
        self.assertTrue(result['counter'])
        self.assertTrue(tray.check_tooltip(observation('Uploading: 7 MiB transferred · Source size: 4 MiB · Estimated 99%'), records, 'proton-drive', 42)['counter'])
        for text in ('Uploading: 7 MiB of 4 MiB (175%)',
                     'Uploading: 7 MiB transferred · Source size: 7 MiB',
                     'Uploading: 12 MiB transferred · Source size: 4 MiB'):
            with self.subTest(text=text), self.assertRaises(tray.TrayProgressError):
                tray.check_tooltip(observation(text), records, 'proton-drive', 42)

    def test_source_estimates_are_bounded_and_must_match_observed_counters(self):
        records = [{'phase': 'uploading', 'sourceBytes': 4*1024**2, 'bytesTransferred': 2*1024**2}]
        self.assertTrue(tray.check_tooltip(observation('Uploading: 2 MiB transferred · Source size: 4 MiB · Estimated 50%'), records, 'proton-drive', 42)['counter'])
        for estimate in (25, 75, 100):
            with self.subTest(estimate=estimate), self.assertRaises(tray.TrayProgressError):
                tray.check_tooltip(observation(f'Uploading: 2 MiB transferred · Source size: 4 MiB · Estimated {estimate}%'), records, 'proton-drive', 42)
        # At zero fill the entire Lift silhouette uses the dim green base.
        dim = observation('Uploading: 2 MiB transferred · Source size: 4 MiB · Estimated 50%', color='#2b5d43')
        self.assertTrue(tray.check_tooltip(dim, records, 'proton-drive', 42)['storageActive'])

    def test_s3_replay_still_requires_its_real_transfer_total(self):
        records = [{'phase': 'uploading', 'sourceBytes': 4*1024**2, 'bytesTransferred': 2*1024**2}]
        with self.assertRaises(tray.TrayProgressError):
            tray.check_tooltip(observation('Uploading: 2 MiB transferred · Source size: 4 MiB · Estimated 50%'), records, 's3', 42)

    def test_completion_frame_requires_full_bright_green_not_empty_fill(self):
        self.assertFalse(tray.check_tooltip(observation('Upload complete'), [], 's3', 42)['idle'])
        for snapshot in (observation('Upload complete', color='#2b5d43'), observation('Upload complete')):
            if snapshot['icons'][0]['opaqueColors'].get('#62d69b'):
                snapshot['icons'][0]['opaqueColors']['#2b5d43'] = 60
            with self.assertRaises(tray.TrayProgressError):
                tray.check_tooltip(snapshot, [], 's3', 42)

    def test_concurrent_rows_cannot_borrow_another_operation_counter(self):
        records = [{'phase': 'uploading', 'sourceBytes': 4*1024**2,
                    'bytesTransferred': n*1024**2, 'bytesTotal': 4*1024**2,
                    'rowIndex': row} for row, ns in ((0, (1, 2)), (1, (2, 3))) for n in ns]
        tray.check_tooltip(observation('Uploading: 1 MiB of 4 MiB (25%)\nUploading: 3 MiB of 4 MiB (75%)'), records, 's3', 42)
        with self.assertRaises(tray.TrayProgressError):
            tray.check_tooltip(observation('Uploading: 3 MiB of 4 MiB (75%)\nUploading: 1 MiB of 4 MiB (25%)'), records, 's3', 42)

    def test_protocol_identity_color_privacy_and_idle_are_independent_of_rendering(self):
        records = [{'phase': 'queued', 'sourceBytes': 0}]
        self.assertFalse(tray.check_tooltip(observation('Queued'), records, 's3', 42)['counter'])
        for defect in ({'pid': 9}, {'hostRegistered': False},
                       {'description': 'Queued synthetic-1.txt'},
                       {'description': 'Queued https://example.invalid/#private'},
                       {'icons': observation('Queued', color='#ffffff')['icons']}):
            with self.subTest(defect=defect), self.assertRaises(tray.TrayProgressError):
                tray.check_tooltip({**observation('Queued'), **defect}, records, 's3', 42)
        idle = observation('', color='#9bbcff', title='put.gd — Share and local capture')
        self.assertTrue(tray.check_tooltip(idle, [], 's3', 42)['idle'])
        with self.assertRaises(tray.TrayProgressError):
            tray.check_tooltip({**idle, 'description': 'Uploading'}, [], 's3', 42)

    def test_proton_preparation_is_working_blue_without_storage_coverage(self):
        records = [{'phase': 'uploading', 'sourceBytes': 4*1024**2}]
        result = tray.check_tooltip(observation('Uploading', color='#9bbcff'),
                                    records, 'proton-drive', 42)
        self.assertFalse(result['idle'])
        self.assertFalse(result['storageActive'])
        self.assertFalse(result['counter'])
        queued = tray.check_tooltip(observation('Queued', color='#9bbcff'),
                    [{'phase': 'queued', 'sourceBytes': 0}], 'proton-drive', 42)
        self.assertFalse(queued['storageActive'])
        records[0]['bytesTransferred'] = 1024**2
        green = tray.check_tooltip(observation('Uploading: 1 MiB transferred · Source size: 4 MiB'),
                                   records, 'proton-drive', 42)
        self.assertTrue(green['storageActive'])
        self.assertTrue(green['counter'])
        mixed = observation('Uploading', color='#9bbcff')
        mixed['icons'].append(observation('Uploading')['icons'][0])
        with self.assertRaises(tray.TrayProgressError):
            tray.check_tooltip(mixed, records, 'proton-drive', 42)

    def test_blue_only_proton_replay_cannot_pass_storage_activity_gate(self):
        class PastQueueGate(Exception):
            pass
        for transfer_color in ('#9bbcff', '#62d69b'):
            with self.subTest(transfer_color=transfer_color), tempfile.TemporaryDirectory() as directory:
                replay = tray.Replay.__new__(tray.Replay)
                mock_reads(replay)
                replay.args = SimpleNamespace(provider='proton-drive')
                replay.pid = 42
                replay.history, replay.mismatch_since = [], None
                replay.output, replay.result = Path(directory), {'coverage': {}}
                replay.save = replay.witness = lambda *args: None
                for phase, text, color, progress in (
                    ('queued', 'Queued', '#9bbcff', {}),
                    ('uploading', 'Uploading: 1 MiB transferred · Source size: 4 MiB',
                     transfer_color, {'bytesTransferred': 1024**2})):
                    op = {'id': 'operation', 'state': 'running', 'phase': phase,
                          'items': [{'artifact': {'id': 'artifact', 'bytes': 4*1024**2},
                                     'uploadProgress': progress}]}
                    replay.cli = lambda *args, op=op: {'operations': [op]}
                    replay.snapshot = lambda text=text, color=color: observation(text, color)
                    replay.observe('queue')
                replay.setup = replay.clipboard = lambda: None
                replay.invoke = lambda: 'operation'
                replay.settled = lambda *args: {'operation': {}}
                replay.successful_uploads = lambda *args: None
                def past_gate(*args):
                    raise PastQueueGate()
                replay.independent_paste = past_gate
                if transfer_color == '#9bbcff':
                    with self.assertRaisesRegex(tray.TrayProgressError, 'storage'):
                        replay.execute()
                else:
                    with self.assertRaises(PastQueueGate):
                        replay.execute()

    def test_intermediate_phase_requires_a_real_observation(self):
        for phase, text in (('sharing', 'Creating share link'),
                            ('saving_local', 'Saving local copy'),
                            ('finalizing', 'Finalizing · Source size: 4 MiB')):
            record = {'phase': phase, 'sourceBytes': 4*1024**2}
            self.assertIn(phase, tray.check_tooltip(observation(text), [record], 's3', 42)['phases'])
        with self.assertRaises(tray.TrayProgressError):
            tray.check_tooltip(observation('Finalizing · Source size: 4 MiB'),
                               [{'phase': 'queued', 'sourceBytes': 4*1024**2}], 's3', 42)

    def test_package_bound_source_provenance_preserves_supplied_dirty_unknown(self):
        commit = '1' * 40
        git = {'gitCommit': commit, 'dirty': False, 'sourceProvenance': 'git'}
        supplied = {'gitCommit': commit, 'dirty': None, 'sourceProvenance': 'supplied'}
        tray.check_build_manifests([git, git.copy()], commit)
        tray.check_build_manifests([supplied, supplied.copy()], commit)
        for manifests in ([git, supplied], [{**git, 'dirty': True}],
                          [{**supplied, 'sourceProvenance': 'unknown'}]):
            with self.subTest(manifests=manifests), self.assertRaises(tray.TrayProgressError):
                tray.check_build_manifests(manifests, commit)

    def test_settings_take_only_preprovisioned_keychain_and_isolated_destination(self):
        import json
        profile = {'id': 'test', 'provider': 's3', 'settings': {'objectPrefix': 'tray-unique/'},
                   'credentials': {'mode': 'keychain', 'reference': 'putgd/synthetic'}}
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'settings.json'
            path.write_text(json.dumps({'profiles': [profile]}))
            self.assertEqual(tray.private_settings(path, 'test', 's3', 'tray-unique')['profiles'], [profile])
            for changed in ({**profile, 'settings': {'objectPrefix': 'preexisting/'}},
                            {**profile, 'credentials': {'mode': 'keychain', 'reference': 'putgd/synthetic', 'secretAccessKey': 'synthetic'}},
                            {**profile, 'settings': {'objectPrefix': 'tray-unique/', 'Access-Key-ID': 'synthetic'}}):
                path.write_text(json.dumps({'profiles': [changed]}))
                with self.subTest(changed=changed), self.assertRaises(tray.TrayProgressError):
                    tray.private_settings(path, 'test', 's3', 'tray-unique')


    def test_private_settings_reject_actual_secret_fields_and_credential_urls_before_argv(self):
        import json
        profile = {'id': 'test', 'provider': 's3',
                   'settings': {'objectPrefix': 'tray-unique/'},
                   'credentials': {'mode': 'keychain', 'reference': 'putgd/synthetic'}}
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'settings.json'
            cases = [{key: 'synthetic'} for key in
                     ('accessKey', 'secretKey', 'accessToken', 'refreshToken', 'userKeyPassword', 'snapshot')]
            cases += [{'endpoint': 'https://synthetic:synthetic@example.invalid/'},
                      {'endpoint': 'HTTPS://synthetic:synthetic@example.invalid/'},
                      {'publicUrlBase': 'https://example.invalid/?token=synthetic'},
                      {'publicUrlBase': 'https://example.invalid/#synthetic'}]
            for extra in cases:
                changed = {**profile, 'settings': {**profile['settings'], **extra}}
                path.write_text(json.dumps({'profiles': [changed]}))
                with self.subTest(extra=extra), self.assertRaises(tray.TrayProgressError):
                    tray.private_settings(path, 'test', 's3', 'tray-unique')


    def test_s3_explicitly_unlocks_preprovisioned_credentials_before_live_work(self):
        from types import SimpleNamespace
        from unittest.mock import Mock
        replay = object.__new__(tray.Replay)
        mock_reads(replay)
        replay.args = SimpleNamespace(provider='s3', profile_id='test')
        replay.result = {}
        replay.save = Mock()
        replay.wait = lambda predicate, message: predicate()
        replay.cli = Mock(side_effect=[{'metadata': {'observedRevision': 'revision'}},
                                      {'setupId': 'setup'}, {'state': 'completed', 'error': {}}])
        replay.activate_credentials()
        self.assertEqual(replay.cli.call_args_list[1].args,
                         ('setup.unlockCredentials', {'profileId': 'test', 'expectedRevision': 'revision'}))
        self.assertEqual(replay.cli.call_args_list[2].args, ('setup.inspect', {'id': 'setup'}))
        replay.args.provider = 'proton-drive'
        replay.cli.reset_mock()
        replay.activate_credentials()
        replay.cli.assert_not_called()


    def test_root_only_s3_scope_requires_unique_templates_for_every_source_kind(self):
        import json
        template = 'tray-unique-{uuid}.{ext}'
        naming = {'mode': 'template', 'collisionPolicy': 'rename', **{key: template for key in
                  ('fileTemplate', 'captureTemplate', 'imageTemplate', 'textTemplate')}}
        profile = {'id': 'test', 'provider': 's3', 'settings': {'objectPrefix': ''},
                   'credentials': {'mode': 'keychain', 'reference': 'putgd/synthetic'}}
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'settings.json'
            patch = {'profiles': [profile], 'naming': naming}
            path.write_text(json.dumps(patch))
            self.assertEqual(tray.private_settings(path, 'test', 's3', 'tray-unique'), patch)
            path.write_text(json.dumps({**patch, 'profiles': [profile, {**profile, 'id': 'other'}]}))
            with self.assertRaises(tray.TrayProgressError):
                tray.private_settings(path, 'test', 's3', 'tray-unique')
            for defect in ({'mode': 'original'}, {'collisionPolicy': 'overwrite'},
                           {'fileTemplate': 'tray-unique.{ext}'}, {'imageTemplate': '{uuid}.{ext}'}):
                path.write_text(json.dumps({**patch, 'naming': {**naming, **defect}}))
                with self.subTest(defect=defect), self.assertRaises(tray.TrayProgressError):
                    tray.private_settings(path, 'test', 's3', 'tray-unique')


    def test_current_row_cannot_borrow_prior_operation_or_file_bytes(self):
        old = {'operationId': 'old', 'artifactId': 'a', 'phase': 'uploading',
               'sourceBytes': 4*1024**2, 'bytesTransferred': 3*1024**2,
               'bytesTotal': 4*1024**2, 'rowIndex': 0, 'activeCount': 1, 'fileIndex': 1, 'fileCount': 2}
        current = {**old, 'operationId': 'new', 'artifactId': 'b', 'bytesTransferred': 1024**2}
        text = observation('Uploading file 1/2: 3 MiB of 4 MiB (75%)')
        for stale in (old, {**old, 'operationId': 'new'}):
            with self.subTest(stale=stale), self.assertRaises(tray.TrayProgressError):
                tray.check_tooltip(text, tray.correlated_records([stale, current], [current]), 's3', 42)
        with self.assertRaises(tray.TrayProgressError):
            tray.check_tooltip(observation('Uploading file 2/2: 1 MiB of 4 MiB (25%)'), [current], 's3', 42)

    def test_same_operation_row_shift_keeps_its_own_batched_counter(self):
        row = {'operationId': 'remaining', 'artifactId': 'a', 'phase': 'uploading',
               'sourceBytes': 4*1024**2, 'bytesTransferred': 1024**2,
               'bytesTotal': 4*1024**2, 'rowIndex': 1, 'activeCount': 1}
        current = {**row, 'rowIndex': 0, 'bytesTransferred': 2*1024**2}
        rows = tray.correlated_records([row, current], [current])
        self.assertTrue(tray.check_tooltip(observation('Uploading: 1 MiB of 4 MiB (25%)'), rows, 's3', 42)['counter'])

    def test_concurrent_batch_row_matches_combined_production_counters(self):
        mib = 1024**2
        def item(status, phase, sent=None, total=4*mib):
            value = {'uploadStatus': status, 'phase': phase, 'artifact': {'id': status + phase, 'bytes': total}}
            if sent is not None:
                value['uploadProgress'] = {'phase': 'uploading', 'bytesTransferred': sent, 'bytesTotal': total}
            return value
        op = {'id': 'batch', 'state': 'running', 'phase': 'uploading', 'created': '2026-10-20T06:00:00Z',
              'items': [item('uploading', 'uploading', 3*mib), item('uploading', 'uploading', mib), item('pending', 'queued')]}
        records = tray.records_from([op])
        self.assertEqual((records[0]['running'], records[0]['waiting'], records[0]['batchTransferred']), (2, 1, 4*mib))
        self.assertTrue(tray.check_tooltip(observation('Uploading: 4 MiB of 8 MiB (50%) · 2 uploading, 1 waiting'),
                                           records, 's3', 42)['counter'])
        for wrong in ('Uploading: 4 MiB of 8 MiB (50%) · 3 uploading', 'Uploading: 6 MiB of 8 MiB (75%) · 2 uploading, 1 waiting',
                      'Uploading: 4 MiB of 9 MiB (44%) · 2 uploading, 1 waiting'):
            with self.subTest(wrong=wrong), self.assertRaises(tray.TrayProgressError):
                tray.check_tooltip(observation(wrong), records, 's3', 42)

    def test_idle_during_active_work_is_a_mismatch(self):
        idle = observation('', color='#9bbcff', title=tray.IDLE)
        with self.assertRaises(tray.TrayProgressError):
            tray.check_tooltip(idle, [{'phase': 'uploading'}], 's3', 42)

    def test_persistent_idle_fails_after_bounded_batching_grace_and_is_preserved(self):
        from types import SimpleNamespace
        from unittest.mock import Mock, patch
        import json
        replay = object.__new__(tray.Replay)
        mock_reads(replay)
        replay.args = SimpleNamespace(provider='s3')
        replay.pid = 42; replay.history = []; replay.mismatch_since = None
        replay.result = {'coverage': {}}; replay.save = Mock(); replay.witness = Mock()
        op = {'id': 'active', 'state': 'running', 'phase': 'uploading', 'items': []}
        replay.cli = Mock(return_value={'operations': [op]})
        replay.snapshot = Mock(return_value=observation('', color='#9bbcff', title=tray.IDLE))
        with tempfile.TemporaryDirectory() as directory:
            replay.output = Path(directory)
            with patch.object(tray.time, 'monotonic', side_effect=[10, 11, 12.01]):
                self.assertFalse(replay.observe('test')[2]['matched'])
                self.assertFalse(replay.observe('test')[2]['matched'])
                with self.assertRaises(tray.TrayProgressError): replay.observe('test')
            records = [json.loads(line) for line in (replay.output/'observations.jsonl').read_text().splitlines()]
            self.assertEqual(len(records), 3)
            self.assertTrue(all(not row['assertions']['matched'] for row in records))
            self.assertFalse(replay.result['coverage'].get('test', {}).get('active', False))

    def error_replay(self, directory, operation=None):
        from unittest.mock import Mock
        replay = object.__new__(tray.Replay)
        mock_reads(replay)
        replay.args = SimpleNamespace(provider='s3')
        replay.pid = 42; replay.history = []; replay.mismatch_since = None
        replay.result = {'coverage': {}}; replay.save = Mock(); replay.witness = Mock()
        replay.output = Path(directory)
        replay.error_pulse = {'operationIds': ['failed'], 'kind': 'local-save', 'required': True}
        operation = operation or {'id': 'failed', 'state': 'failed', 'items': [
            {'uploadStatus': 'local_only', 'local': {'status': 'failed'}}]}
        replay.cli = Mock(return_value={'operations': [operation]})
        return replay

    def test_expected_local_error_requires_red_blue_and_bounded_idle_recovery(self):
        from unittest.mock import Mock, patch
        with tempfile.TemporaryDirectory() as directory:
            replay = self.error_replay(directory)
            replay.snapshot = Mock(side_effect=[
                observation('', '#ffb4ab', tray.IDLE), observation('', '#9bbcff', tray.IDLE),
                observation('', '#ffb4ab', tray.IDLE), observation('', '#9bbcff', tray.IDLE),
                observation('', '#9bbcff', tray.IDLE)])
            with patch.object(tray.time, 'monotonic', side_effect=[10, 10.31, 10.62, 12.99, 13.01]):
                red = replay.observe('local-failure')[2]
                blue = replay.observe('local-failure')[2]
                self.assertFalse(red['idle']); self.assertFalse(blue['idle'])
                self.assertEqual(red['errorColor'], '#ffb4ab')
                self.assertEqual(blue['errorColor'], '#9bbcff')
                self.assertEqual(replay.observe('local-failure')[2]['errorColor'], '#ffb4ab')
                self.assertFalse(replay.observe('local-failure')[2]['idle'])
                self.assertTrue(replay.observe('local-failure')[2]['idle'])
            pulse = replay.result['errorPulses'][0]
            self.assertEqual(pulse['operationIds'], ['failed'])
            self.assertEqual(pulse['failureObservedAt'], 10)
            self.assertEqual(pulse['deadline'], 13)
            self.assertEqual(pulse['redObservedAt'], 10)
            self.assertEqual(pulse['blueAfterRedObservedAt'], 10.31)
            self.assertEqual(pulse['redAfterBlueObservedAt'], 10.62)
            self.assertEqual(pulse['recoveredAt'], 13.01)
            self.assertFalse(replay.result['coverage']['local-failure']['active'])
            labels = [call.args[0] for call in replay.witness.call_args_list]
            self.assertIn('local-failure-error-red', labels)
            self.assertIn('local-failure-error-blue', labels)
            self.assertIn('local-failure-error-recovered', labels)

    def test_green_cannot_override_an_established_error_pulse(self):
        from unittest.mock import Mock, patch
        with tempfile.TemporaryDirectory() as directory:
            replay = self.error_replay(directory)
            replay.snapshot = Mock(side_effect=[observation('', '#ffb4ab', tray.IDLE),
                observation('Upload complete', '#62d69b')])
            with patch.object(tray.time, 'monotonic', side_effect=[10, 10.31]):
                replay.observe('local-failure')
                with self.assertRaises(tray.TrayProgressError):
                    replay.observe('local-failure')

    def test_green_grace_does_not_earn_upload_or_error_coverage(self):
        from unittest.mock import Mock, patch
        with tempfile.TemporaryDirectory() as directory:
            replay = self.error_replay(directory)
            replay.snapshot = Mock(side_effect=[observation('', '#ffb4ab', tray.IDLE),
                observation('', '#9bbcff', tray.IDLE), observation('Upload complete', '#62d69b')])
            with patch.object(tray.time, 'monotonic', side_effect=[10, 10.31, 12.6]):
                replay.observe('local-failure'); replay.observe('local-failure')
                checked = replay.observe('local-failure')[2]
            self.assertFalse(checked['matched'])
            self.assertFalse(checked['idle'])
            self.assertFalse(replay.result['coverage']['local-failure']['active'])

    def test_error_recovery_requires_red_to_return_after_blue(self):
        from unittest.mock import Mock, patch
        with tempfile.TemporaryDirectory() as directory:
            replay = self.error_replay(directory)
            replay.snapshot = Mock(side_effect=[observation('', '#ffb4ab', tray.IDLE),
                observation('', '#9bbcff', tray.IDLE), observation('', '#9bbcff', tray.IDLE)])
            with patch.object(tray.time, 'monotonic', side_effect=[10, 10.31, 13.01]):
                replay.observe('local-failure'); replay.observe('local-failure')
                with self.assertRaises(tray.TrayProgressError): replay.observe('local-failure')

    def test_new_failure_cannot_reuse_an_earlier_blink_sequence(self):
        from unittest.mock import Mock, patch
        with tempfile.TemporaryDirectory() as directory:
            replay = self.error_replay(directory)
            replay.args.provider = 'proton-drive'
            replay.error_pulse = {'operationIds': ['failed', 'second'], 'kind': 'proton-busy', 'required': False}
            def operation(oid):
                return {'id': oid, 'state': 'partial', 'items': [{'uploadStatus': 'failed',
                    'shareStatus': 'not_requested', 'error': {'code': 'proton_account_busy'}}]}
            first, second = operation('failed'), operation('second')
            replay.cli = Mock(side_effect=[{'operations': [first]}]*6 + [{'operations': [first, second]}]*4)
            replay.snapshot = Mock(side_effect=[observation('', color, tray.IDLE) for color in
                ('#ffb4ab', '#9bbcff', '#ffb4ab', '#9bbcff', '#9bbcff')])
            with patch.object(tray.time, 'monotonic', side_effect=[10, 10.31, 10.62, 12.5, 15.51]):
                for _ in range(4): replay.observe('concurrent')
                with self.assertRaises(tray.TrayProgressError): replay.observe('concurrent')

    def test_error_pulse_cannot_borrow_failure_identity_or_outlive_fixed_window(self):
        from unittest.mock import Mock, patch
        invalid = [
            {'id': 'other', 'state': 'failed', 'items': [{'uploadStatus': 'local_only', 'local': {'status': 'failed'}}]},
            {'id': 'failed', 'state': 'running', 'phase': 'saving_local', 'items': []},
            {'id': 'failed', 'state': 'failed', 'items': [{'uploadStatus': 'failed', 'local': {'status': 'saved'}}]}]
        for operation in invalid:
            with self.subTest(operation=operation), tempfile.TemporaryDirectory() as directory:
                replay = self.error_replay(directory, operation)
                replay.snapshot = Mock(return_value=observation('', '#ffb4ab', tray.IDLE))
                with patch.object(tray.time, 'monotonic', return_value=10):
                    with self.assertRaises(tray.TrayProgressError): replay.observe('local-failure')
        with tempfile.TemporaryDirectory() as directory:
            replay = self.error_replay(directory)
            replay.snapshot = Mock(return_value=observation('', '#ffb4ab', tray.IDLE))
            with patch.object(tray.time, 'monotonic', side_effect=[10, 13.01]):
                replay.observe('local-failure')
                with self.assertRaises(tray.TrayProgressError): replay.observe('local-failure')

    def test_error_recovery_rejects_missing_red_or_missing_blue_transition(self):
        from unittest.mock import Mock, patch
        for first_color in ('#9bbcff', '#ffb4ab'):
            with self.subTest(first_color=first_color), tempfile.TemporaryDirectory() as directory:
                replay = self.error_replay(directory)
                replay.snapshot = Mock(side_effect=[observation('', first_color, tray.IDLE),
                                                    observation('', '#9bbcff', tray.IDLE)])
                with patch.object(tray.time, 'monotonic', side_effect=[10, 13.01]):
                    replay.observe('local-failure')
                    with self.assertRaises(tray.TrayProgressError): replay.observe('local-failure')

    def test_expected_error_keeps_privacy_identity_and_color_guards(self):
        from unittest.mock import Mock, patch
        for defect in ({'pid': 7}, {'hostRegistered': False}, {'registered': False},
                       {'description': 'https://example.invalid/sensitive'},
                       {'title': 'unexpected'},
                       {'icons': observation('', '#ffffff', tray.IDLE)['icons']},
                       {'icons': [observation('', '#ffb4ab', tray.IDLE)['icons'][0],
                                  observation('', '#9bbcff', tray.IDLE)['icons'][0]]}):
            with self.subTest(defect=defect), tempfile.TemporaryDirectory() as directory:
                replay = self.error_replay(directory)
                replay.snapshot = Mock(return_value={**observation('', '#ffb4ab', tray.IDLE), **defect})
                with patch.object(tray.time, 'monotonic', return_value=10):
                    with self.assertRaises(tray.TrayProgressError): replay.observe('local-failure')
                evidence = (replay.output/'observations.jsonl').read_text()
                self.assertIn('redacted', evidence); self.assertNotIn('sensitive', evidence)
        with self.assertRaises(tray.TrayProgressError):
            tray.check_tooltip(observation('', '#ffb4ab', tray.IDLE), [], 's3', 42)

    def test_only_explicit_original_id_proton_busy_can_authorize_optional_pulse(self):
        from unittest.mock import Mock, patch
        for code in ('proton_account_busy', 'proton_permission_denied'):
            with self.subTest(code=code), tempfile.TemporaryDirectory() as directory:
                op = {'id': 'failed', 'state': 'partial', 'items': [
                    {'uploadStatus': 'uploaded', 'shareStatus': 'failed', 'error': {'code': code}}]}
                replay = self.error_replay(directory, op); replay.args.provider = 'proton-drive'
                replay.error_pulse = {'operationIds': ['failed'], 'kind': 'proton-busy', 'required': False}
                replay.snapshot = Mock(return_value=observation('', '#ffb4ab', tray.IDLE))
                with patch.object(tray.time, 'monotonic', return_value=10):
                    if code == 'proton_account_busy':
                        self.assertEqual(replay.observe('concurrent')[2]['errorColor'], '#ffb4ab')
                    else:
                        with self.assertRaises(tray.TrayProgressError): replay.observe('concurrent')

    def test_concurrency_requires_two_running_nonqueued_rows(self):
        checked = {'idle': False, 'phases': ['uploading', 'queued']}
        rows = [{'state': 'running', 'phase': 'uploading'}, {'state': 'stored', 'phase': 'queued'}]
        self.assertFalse(tray.concurrent_admission(checked, rows))
        checked['phases'][1] = 'preflight'; rows[1] = {'state': 'running', 'phase': 'preflight'}
        self.assertTrue(tray.concurrent_admission(checked, rows))

    def test_proton_busy_recovery_retries_original_ids_sequentially(self):
        from types import SimpleNamespace
        from unittest.mock import Mock
        import copy
        item = {'artifact': {'id': 'artifact'}, 'publicationId': 'publication',
                'uploadStatus': 'failed', 'shareStatus': 'not_requested', 'error': {'code': 'proton_account_busy'}}
        initial = {'original': {'id': 'original', 'state': 'partial', 'items': [item]}}
        final = copy.deepcopy(initial); final['original']['state'] = 'completed'
        final['original']['items'][0].update(uploadStatus='uploaded', shareStatus='shared', error={})
        replay = object.__new__(tray.Replay); replay.args = SimpleNamespace(provider='proton-drive')
        mock_reads(replay)
        replay.save = Mock(); replay.patch = Mock(); replay.successful_uploads = Mock()
        replay.cli = Mock(return_value={'operationId': 'original', 'retryAccepted': True})
        replay.settled = Mock(return_value=final)
        self.assertEqual(replay.recover_proton_busy(initial), final)
        replay.cli.assert_called_once_with('operation.retry', {'id': 'original'})
        replay.settled.assert_called_once_with(['original'], 'concurrent-recovery-1')
        for defect in ({'error': {'code': 'proton_permission_denied'}}, {'uploadStatus': 'unknown'}):
            bad = copy.deepcopy(initial); bad['original']['items'][0].update(defect)
            replay.cli.reset_mock()
            with self.subTest(defect=defect), self.assertRaises(tray.TrayProgressError): replay.recover_proton_busy(bad)
            replay.cli.assert_not_called()
        final['original']['items'][0]['publicationId'] = 'changed'
        with self.assertRaises(tray.TrayProgressError): replay.recover_proton_busy(initial)


    def test_privacy_failure_is_fatal_and_evidence_is_redacted(self):
        from types import SimpleNamespace
        from unittest.mock import Mock
        replay = object.__new__(tray.Replay); replay.args = SimpleNamespace(provider='s3')
        mock_reads(replay)
        replay.pid = 42; replay.history = []; replay.mismatch_since = None
        replay.result = {'coverage': {}}; replay.save = Mock(); replay.witness = Mock()
        replay.cli = Mock(return_value={'operations': []})
        for text in ('Queued https://example.invalid/#sensitive',
                     'Queued HTTPS://example.invalid/#sensitive', 'Queued hTtPs://example.invalid/#sensitive',
                     'Queued FILE:///tmp/sensitive'):
            replay.snapshot = Mock(return_value=observation(text))
            with self.subTest(text=text), tempfile.TemporaryDirectory() as directory:
                replay.output = Path(directory)
                with self.assertRaises(tray.TrayProgressError): replay.observe('privacy')
                evidence = (replay.output/'observations.jsonl').read_text()
                self.assertNotIn('sensitive', evidence); self.assertNotIn('example.invalid', evidence)
                self.assertIn('redacted', evidence)

    def test_later_busy_pass_cannot_change_a_receipt_established_in_first_pass(self):
        import copy
        from types import SimpleNamespace
        from unittest.mock import Mock
        items = [{'artifact': {'id': key}, 'publicationId': 'pub-'+key, 'uploadStatus': 'failed',
                  'shareStatus': 'not_requested', 'error': {'code': 'proton_account_busy'}} for key in ('a', 'b')]
        initial = {'original': {'id': 'original', 'state': 'partial', 'items': items}}
        first = copy.deepcopy(initial)
        first['original']['items'][0].update(uploadStatus='uploaded', shareStatus='shared', receipt={'identity': 'first'}, error={})
        second = copy.deepcopy(first); second['original']['state'] = 'completed'
        second['original']['items'][0]['receipt'] = {'identity': 'changed'}
        second['original']['items'][1].update(uploadStatus='uploaded', shareStatus='shared', error={})
        replay = object.__new__(tray.Replay); replay.args = SimpleNamespace(provider='proton-drive')
        mock_reads(replay)
        replay.save = Mock(); replay.patch = Mock()
        replay.cli = Mock(return_value={'operationId': 'original', 'retryAccepted': True})
        replay.settled = Mock(side_effect=[first, second])
        with self.assertRaisesRegex(tray.TrayProgressError, 'receipt'): replay.recover_proton_busy(initial)


    def test_wayland_display_uses_updated_session_environment_before_private_runtime(self):
        import os, subprocess
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); binary = root/'systemctl'
            binary.write_text("#!/bin/sh\nprintf '%s\\n' 'WAYLAND_DISPLAY=wayland-0'\n")
            binary.chmod(0o700)
            env = {**os.environ, 'PATH': str(root)+':'+os.environ['PATH'],
                   'XDG_SESSION_TYPE': 'wayland', 'XDG_RUNTIME_DIR': str(root/'native')}
            env.pop('WAYLAND_DISPLAY', None)
            command = tray.native_display_prefix()+"export XDG_RUNTIME_DIR=/tmp/private\nprintf '%s' \"$WAYLAND_DISPLAY\"\n"
            result = subprocess.check_output(['sh', '-c', command], env=env, text=True)
            self.assertEqual(result, str(root/'native/wayland-0'))


def cpp_string(literal):
    return literal.encode().decode('unicode_escape')


class TrayPanelToneContracts(unittest.TestCase):
    """The lab reads the panel tone and picks tints the way put.gd does (#229)."""

    LIGHT = tray.TRAY_TINTS['light']
    DARK = tray.TRAY_TINTS['dark']

    def test_tint_table_matches_theme_and_icon_art(self):
        import colorsys, re
        source = (ROOT / 'src/ui/Theme.cpp').read_text()
        for name in ('dark', 'light'):
            found = re.search(r'static const TrayTints ' + name + r'\{([^}]*)\}', source)
            self.assertIsNotNone(found, name)
            idle, upload, encoding, error = [c.lower() for c in re.findall(r'QColor\("(#[0-9A-Fa-f]{6})"\)', found[1])]
            tints = tray.TRAY_TINTS[name]
            self.assertEqual((tints['idle'], tints['upload'], tints['encoding'], tints['error']),
                             (idle, upload, encoding, error))
            # IconArt::liftTray draws the unfilled remainder as color.darker(230).
            self.assertIn('p.fillPath(shape,color.darker(230));', (ROOT / 'src/ui/IconArt.cpp').read_text())
            h, sat, v = colorsys.rgb_to_hsv(*(int(upload[i:i + 2], 16) / 255 for i in (1, 3, 5)))
            dim = '#' + ''.join('%02x' % round(c * 255) for c in colorsys.hsv_to_rgb(h, sat, v * 100 / 230))
            self.assertEqual(tints['uploadDim'], dim)
        self.assertEqual(self.LIGHT['uploadDim'], '#092f1d')

    def test_panel_tone_rules_match_the_cpp_rule_table(self):
        import re
        source = (ROOT / 'tests/gui/test_theme.cpp').read_text()
        body = source[source.index('void panelToneRules_data()'):source.index('void panelToneRules()')]
        rows = re.findall(r'QTest::newRow\("([^"]*)"\)((?: << "(?:[^"\\]|\\.)*"){5}) << (true|false) << "([a-z-]+)"', body)
        self.assertGreaterEqual(len(rows), 11)
        for name, fields, light, source_name in rows:
            desktop, plasmarc, kdeglobals, cinnamon, style = [cpp_string(v) for v in re.findall(r'"((?:[^"\\]|\\.)*)"', fields)]
            with self.subTest(name):
                self.assertEqual(tray.panel_tone({'desktop': desktop, 'plasmarc': plasmarc, 'kdeglobals': kdeglobals,
                                                  'cinnamonTheme': cinnamon, 'styleColors': style}),
                                 (light == 'true', source_name))

    def run_reader(self, root, desktop, files, data=()):
        import os, subprocess, json
        config, system, share = root / 'config', root / 'etc-xdg', root / 'share'
        for path, text in list(files) + list(data):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
        env = {'PATH': os.environ['PATH'], 'HOME': str(root / 'home'), 'XDG_CURRENT_DESKTOP': desktop,
               'XDG_CONFIG_HOME': str(config), 'XDG_CONFIG_DIRS': str(system),
               'XDG_DATA_HOME': str(root / 'data-home'), 'XDG_DATA_DIRS': str(share)}
        return json.loads(subprocess.check_output(['python3', '-I', '-c', tray.PANEL_READER], env=env, text=True))

    def test_reader_follows_the_kde_configuration_cascade(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            # d69: Breeze Dark from kdedefaults wins over a light colour scheme, through the style's colours.
            inputs = self.run_reader(root / 'd69', 'KDE', [
                (root / 'd69/config/kdedefaults/plasmarc', '[Theme]\nname=breeze-dark\n'),
                (root / 'd69/config/kdeglobals', '[Colors:Header]\nBackgroundNormal=222,224,226\n')],
                [(root / 'd69/share/plasma/desktoptheme/breeze-dark/colors', '[Colors:Window]\nBackgroundNormal=32,35,38\n')])
            self.assertEqual(inputs['plasmaStyle'], 'breeze-dark')
            self.assertEqual(tray.panel_tone(inputs), (False, 'plasma-style'))
            # The lab's private, empty XDG_CONFIG_HOME falls through to XDG_CONFIG_DIRS.
            inputs = self.run_reader(root / 'private', 'KDE', [
                (root / 'private/etc-xdg/kdeglobals', '[Colors:Window]\nBackgroundNormal=239,240,241\n')])
            self.assertEqual(tray.panel_tone(inputs), (True, 'kdeglobals'))
            # A user file that sets the key shadows the system one.
            inputs = self.run_reader(root / 'user', 'KDE', [
                (root / 'user/config/kdeglobals', '[Colors:Header]\nBackgroundNormal=32,35,38\n'),
                (root / 'user/etc-xdg/kdeglobals', '[Colors:Header]\nBackgroundNormal=239,240,241\n')])
            self.assertEqual(tray.panel_tone(inputs), (False, 'kdeglobals'))
            self.assertEqual(tray.panel_tone(self.run_reader(root / 'bare', 'KDE', [])), (True, 'kde-default'))
            self.assertEqual(tray.panel_tone(self.run_reader(root / 'gnome', 'GNOME', [])), (False, 'gnome-shell'))

    def test_light_panel_validates_against_the_light_tints(self):
        light = self.LIGHT
        idle = observation('', color=light['idle'], title=tray.IDLE)
        self.assertTrue(tray.check_tooltip(idle, [], 's3', 42, tints=light)['idle'])
        # The #207 failure: a correct light-panel icon checked against the dark set.
        with self.assertRaisesRegex(tray.TrayProgressError, 'brand color'):
            tray.check_tooltip(idle, [], 's3', 42)
        records = [{'id': 'op', 'phase': 'uploading', 'bytesTransferred': 2097152, 'bytesTotal': 4194304, 'bytesEstimated': False}]
        active = observation('Uploading: 2 MiB of 4 MiB (50%)', color=light['upload'])
        active['icons'][0]['opaqueColors'][light['uploadDim']] = 60
        result = tray.check_tooltip(active, records, 's3', 42, tints=light)
        self.assertTrue(result['storageActive'] and result['counter'])
        with self.assertRaises(tray.TrayProgressError):
            tray.check_tooltip(active, records, 's3', 42)
        dark_on_light = observation('Uploading: 2 MiB of 4 MiB (50%)', color=self.DARK['upload'])
        with self.assertRaises(tray.TrayProgressError):
            tray.check_tooltip(dark_on_light, records, 's3', 42, tints=light)
        self.assertFalse(tray.check_tooltip(observation('Upload complete', color=light['upload']), [], 's3', 42, tints=light)['idle'])
        with self.assertRaises(tray.TrayProgressError):
            tray.check_tooltip(active | {'description': 'Upload complete'}, [], 's3', 42, tints=light)
        red = tray.check_tooltip(observation('', light['error'], tray.IDLE), [], 's3', 42, error_pulse=True, tints=light)
        self.assertEqual(red['errorColor'], light['error'])
        with self.assertRaises(tray.TrayProgressError):
            tray.check_tooltip(observation('', self.DARK['error'], tray.IDLE), [], 's3', 42, error_pulse=True, tints=light)

    def test_light_panel_error_pulse_uses_the_light_red_and_blue(self):
        from unittest.mock import Mock, patch
        light = self.LIGHT
        with tempfile.TemporaryDirectory() as directory:
            replay = TrayProgressContracts.error_replay(self, directory)
            replay.tints = light
            replay.snapshot = Mock(side_effect=[observation('', light[c], tray.IDLE)
                                                for c in ('error', 'idle', 'error', 'idle', 'idle')])
            with patch.object(tray.time, 'monotonic', side_effect=[10, 10.31, 10.62, 12.99, 13.01]):
                self.assertEqual(replay.observe('local-failure')[2]['errorColor'], light['error'])
                self.assertEqual(replay.observe('local-failure')[2]['errorColor'], light['idle'])
                replay.observe('local-failure'); replay.observe('local-failure')
                self.assertTrue(replay.observe('local-failure')[2]['idle'])
            self.assertEqual(replay.result['errorPulses'][0]['recoveredAt'], 13.01)

    def test_session_takes_the_desktop_sessions_config_and_data_dirs(self):
        import os, subprocess
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            shell = subprocess.Popen(['sleep', '30'], env={'PATH': os.environ['PATH'],
                                     'XDG_CONFIG_DIRS': '/home/lab/.config/kdedefaults:/etc/xdg',
                                     'XDG_DATA_DIRS': '/usr/share/plasma-data:/usr/share'})
            try:
                # Stands in for pgrep, so no real desktop shell on this host is read.
                (root / 'pgrep').write_text('#!/bin/sh\necho ' + str(shell.pid) + '\n')
                (root / 'pgrep').chmod(0o700)
                command = tray.session_dirs_prefix('plasmashell') + 'printf "%s|%s" "$XDG_CONFIG_DIRS" "$XDG_DATA_DIRS"\n'
                result = subprocess.check_output(['sh', '-c', command], text=True,
                                                 env={'PATH': str(root) + ':' + os.environ['PATH']})
            finally:
                shell.kill(); shell.wait()
            self.assertEqual(result, '/home/lab/.config/kdedefaults:/etc/xdg|/usr/share/plasma-data:/usr/share')
        with self.assertRaises(tray.TrayProgressError):
            tray.session_dirs_prefix('plasmashell; true')

    def tone_replay(self, directory, inputs, reported):
        import json
        from unittest.mock import Mock
        replay = object.__new__(tray.Replay)
        replay.args = SimpleNamespace(timeout=0.3)
        replay.result = {}; replay.save = Mock(); replay.output = Path(directory)
        replay.session = Mock(return_value=json.dumps(inputs))
        replay.cli = Mock(side_effect=lambda method, params=None: {'diagnostics': {'trayTint': reported}}
                          if method == 'ui.snapshot' else self.fail(method))
        return replay

    def test_replay_takes_the_tint_set_both_readers_agree_on(self):
        light_panel = {'desktop': 'KDE', 'plasmarc': '', 'kdeglobals': '', 'styleColors': ''}
        with tempfile.TemporaryDirectory() as directory:
            replay = self.tone_replay(directory, light_panel, {'panel': 'light', 'source': 'kde-default', 'color': '#2759b8'})
            self.assertEqual(replay.read_panel_tone()['panel'], 'light')
            self.assertIs(replay.tints, self.LIGHT)
            self.assertEqual(replay.result['panelTone']['reportedByApp']['source'], 'kde-default')
            dark_panel = {'desktop': 'ubuntu:GNOME'}
            replay = self.tone_replay(directory, dark_panel, None)
            self.assertEqual(replay.read_panel_tone()['source'], 'gnome-shell')
            self.assertIs(replay.tints, self.DARK)
            replay = self.tone_replay(directory, light_panel, {'panel': 'dark', 'source': 'default', 'color': '#9bbcff'})
            with self.assertRaisesRegex(tray.TrayProgressError, 'tray panel'):
                replay.read_panel_tone()


if __name__ == '__main__':
    unittest.main()
