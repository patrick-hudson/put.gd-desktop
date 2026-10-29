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


def observation(description, color=None, title=None):
    color = color or tray.TRAY_TINTS['dark']['upload' if description == 'Upload complete' else 'encoding']
    title = title or (tray.COMPLETE if description == 'Upload complete' else tray.UPLOADING)
    # A partial progress fill keeps idle pixels beside the encoding colour.
    colors = {color: 120}
    if color == tray.TRAY_TINTS['dark']['encoding']:
        colors[tray.TRAY_TINTS['dark']['idle']] = 60
    return {'pid': 42, 'hostRegistered': True, 'registered': True,
            'title': title, 'description': description,
            'icons': [{'width': 24, 'height': 24, 'sha256': 'synthetic',
                       'opaqueColors': colors}]}


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
        self.assertTrue(tray.check_tooltip(observation('Uploading: 2 MB of 4 MB (50%)'), records, 's3', 42)['counter'])
        for text in ('Uploading: 5 MB of 4 MB (125%)',
                     'Uploading: 2 MB of 4 MB (90%)',
                     'Uploading: 2 MB of 8 MB (25%)'):
            with self.subTest(text=text), self.assertRaises(tray.TrayProgressError):
                tray.check_tooltip(observation(text), records, 's3', 42)

    def test_proton_counter_keeps_real_bytes_and_labels_source_estimate(self):
        # The SDK may retry and decrease its encrypted byte counter.
        records = [{'phase': 'uploading', 'sourceBytes': 4*1024**2,
                    'bytesTransferred': n*1024**2} for n in (8, 6)]
        result = tray.check_tooltip(observation('Uploading: 7 MB transferred · Source size: 4 MB'), records, 'proton-drive', 42)
        self.assertTrue(result['counter'])
        self.assertTrue(tray.check_tooltip(observation('Uploading: 7 MB transferred · Source size: 4 MB · Estimated 99%'), records, 'proton-drive', 42)['counter'])
        for text in ('Uploading: 7 MB of 4 MB (175%)',
                     'Uploading: 7 MB transferred · Source size: 7 MB',
                     'Uploading: 12 MB transferred · Source size: 4 MB'):
            with self.subTest(text=text), self.assertRaises(tray.TrayProgressError):
                tray.check_tooltip(observation(text), records, 'proton-drive', 42)

    def test_source_estimates_are_bounded_and_must_match_observed_counters(self):
        records = [{'phase': 'uploading', 'sourceBytes': 4*1024**2, 'bytesTransferred': 2*1024**2}]
        self.assertTrue(tray.check_tooltip(observation('Uploading: 2 MB transferred · Source size: 4 MB · Estimated 50%'), records, 'proton-drive', 42)['counter'])
        for estimate in (25, 75, 100):
            with self.subTest(estimate=estimate), self.assertRaises(tray.TrayProgressError):
                tray.check_tooltip(observation(f'Uploading: 2 MB transferred · Source size: 4 MB · Estimated {estimate}%'), records, 'proton-drive', 42)
        # At zero fill the entire Lift silhouette retains its idle tint.
        empty = observation('Uploading: 2 MB transferred · Source size: 4 MB · Estimated 50%', color='#d6d6d2')
        self.assertFalse(tray.check_tooltip(empty, records, 'proton-drive', 42)['storageActive'])

    def test_s3_replay_still_requires_its_real_transfer_total(self):
        records = [{'phase': 'uploading', 'sourceBytes': 4*1024**2, 'bytesTransferred': 2*1024**2}]
        with self.assertRaises(tray.TrayProgressError):
            tray.check_tooltip(observation('Uploading: 2 MB transferred · Source size: 4 MB · Estimated 50%'), records, 's3', 42)

    def test_completion_frame_requires_green_without_progress_fill(self):
        self.assertFalse(tray.check_tooltip(observation('Upload complete'), [], 's3', 42)['idle'])
        for snapshot in (observation('Upload complete', color='#d6d6d2'), observation('Upload complete')):
            if snapshot['icons'][0]['opaqueColors'].get(tray.TRAY_TINTS['dark']['upload']):
                snapshot['icons'][0]['opaqueColors'][tray.TRAY_TINTS['dark']['encoding']] = 60
            with self.assertRaises(tray.TrayProgressError):
                tray.check_tooltip(snapshot, [], 's3', 42)

    def test_a_nearly_complete_fill_may_cover_the_whole_glyph(self):
        records = [{'phase': 'uploading', 'sourceBytes': 8*1024**2, 'bytesTransferred': 8*1024**2, 'bytesTotal': 8*1024**2}]
        full = observation('Uploading: 8 MB of 8 MB (100%)')
        full['icons'][0]['opaqueColors'] = {tray.TRAY_TINTS['dark']['encoding']: 400, '#a58656': 30}
        result = tray.check_tooltip(full, records, 's3', 42)
        self.assertTrue(result['storageActive'] and result['counter'])
        empty = observation('Uploading: 8 MB of 8 MB (100%)')
        empty['icons'][0]['opaqueColors'] = {'#a58656': 30}
        with self.assertRaisesRegex(tray.TrayProgressError, 'brand color'):
            tray.check_tooltip(empty, records, 's3', 42)
        sliver = observation('Uploading: 0.02 MB of 8 MB (0%)')
        sliver['icons'] = [{'width': 24, 'sha256': 's', 'opaqueColors': {tray.TRAY_TINTS['dark']['idle']: 128}},
                           {'width': 72, 'sha256': 'l', 'opaqueColors': {tray.TRAY_TINTS['dark']['idle']: 1500,
                                                                          tray.TRAY_TINTS['dark']['encoding']: 12}}]
        records[0]['bytesTransferred'] = 20000
        self.assertFalse(tray.check_tooltip(sliver, records, 's3', 42)['storageActive'])
        idle = observation('', tray.TRAY_TINTS['dark']['encoding'], tray.IDLE)
        with self.assertRaisesRegex(tray.TrayProgressError, 'progress fill'):
            tray.check_tooltip(idle, [], 's3', 42)

    def test_concurrent_rows_cannot_borrow_another_operation_counter(self):
        records = [{'phase': 'uploading', 'sourceBytes': 4*1024**2,
                    'bytesTransferred': n*1024**2, 'bytesTotal': 4*1024**2,
                    'rowIndex': row} for row, ns in ((0, (1, 2)), (1, (2, 3))) for n in ns]
        tray.check_tooltip(observation('Uploading: 1 MB of 4 MB (25%)\nUploading: 3 MB of 4 MB (75%)'), records, 's3', 42)
        with self.assertRaises(tray.TrayProgressError):
            tray.check_tooltip(observation('Uploading: 3 MB of 4 MB (75%)\nUploading: 1 MB of 4 MB (25%)'), records, 's3', 42)

    def test_protocol_identity_color_privacy_and_idle_are_independent_of_rendering(self):
        records = [{'phase': 'queued', 'sourceBytes': 0}]
        self.assertFalse(tray.check_tooltip(observation('Queued'), records, 's3', 42)['counter'])
        for defect in ({'pid': 9}, {'hostRegistered': False},
                       {'description': 'Queued synthetic-1.txt'},
                       {'description': 'Queued https://example.invalid/#private'},
                       {'icons': observation('Queued', color='#ffffff')['icons']}):
            with self.subTest(defect=defect), self.assertRaises(tray.TrayProgressError):
                tray.check_tooltip({**observation('Queued'), **defect}, records, 's3', 42)
        idle = observation('', color='#d6d6d2', title=tray.IDLE)
        self.assertTrue(tray.check_tooltip(idle, [], 's3', 42)['idle'])
        with self.assertRaises(tray.TrayProgressError):
            tray.check_tooltip({**idle, 'description': 'Uploading'}, [], 's3', 42)

    def test_proton_preparation_uses_idle_tint_without_storage_coverage(self):
        records = [{'phase': 'uploading', 'sourceBytes': 4*1024**2}]
        result = tray.check_tooltip(observation('Uploading', color='#d6d6d2'),
                                    records, 'proton-drive', 42)
        self.assertFalse(result['idle'])
        self.assertFalse(result['storageActive'])
        self.assertFalse(result['counter'])
        queued = tray.check_tooltip(observation('Queued', color='#d6d6d2'),
                    [{'phase': 'queued', 'sourceBytes': 0}], 'proton-drive', 42)
        self.assertFalse(queued['storageActive'])
        records[0]['bytesTransferred'] = 1024**2
        green = tray.check_tooltip(observation('Uploading: 1 MB transferred · Source size: 4 MB'),
                                   records, 'proton-drive', 42)
        self.assertTrue(green['storageActive'])
        self.assertTrue(green['counter'])
        # One size idle-only and another fill-only share no colour.
        mixed = observation('Uploading', color='#d6d6d2')
        mixed['icons'].append({'width': 48, 'height': 48, 'sha256': 'fill',
                               'opaqueColors': {tray.TRAY_TINTS['dark']['encoding']: 400}})
        with self.assertRaises(tray.TrayProgressError):
            tray.check_tooltip(mixed, records, 'proton-drive', 42)

    def test_idle_tint_only_proton_replay_cannot_pass_storage_activity_gate(self):
        class PastQueueGate(Exception):
            pass
        for transfer_color in ('#d6d6d2', '#e8b66d'):
            with self.subTest(transfer_color=transfer_color), tempfile.TemporaryDirectory() as directory:
                replay = tray.Replay.__new__(tray.Replay)
                mock_reads(replay)
                replay.args = SimpleNamespace(provider='proton-drive')
                replay.pid = 42
                replay.history, replay.mismatch_since = [], None
                replay.output, replay.result = Path(directory), {'coverage': {}}
                replay.save = replay.witness = lambda *args: None
                for phase, text, color, progress in (
                    ('queued', 'Queued', '#d6d6d2', {}),
                    ('uploading', 'Uploading: 1 MB transferred · Source size: 4 MB',
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
                if transfer_color == '#d6d6d2':
                    with self.assertRaisesRegex(tray.TrayProgressError, 'storage'):
                        replay.execute()
                else:
                    with self.assertRaises(PastQueueGate):
                        replay.execute()

    def test_intermediate_phase_requires_a_real_observation(self):
        for phase, text in (('sharing', 'Creating share link'),
                            ('saving_local', 'Saving local copy'),
                            ('finalizing', 'Finalizing · Source size: 4 MB')):
            record = {'phase': phase, 'sourceBytes': 4*1024**2}
            self.assertIn(phase, tray.check_tooltip(observation(text), [record], 's3', 42)['phases'])
        with self.assertRaises(tray.TrayProgressError):
            tray.check_tooltip(observation('Finalizing · Source size: 4 MB'),
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
        text = observation('Uploading file 1/2: 3 MB of 4 MB (75%)')
        for stale in (old, {**old, 'operationId': 'new'}):
            with self.subTest(stale=stale), self.assertRaises(tray.TrayProgressError):
                tray.check_tooltip(text, tray.correlated_records([stale, current], [current]), 's3', 42)
        with self.assertRaises(tray.TrayProgressError):
            tray.check_tooltip(observation('Uploading file 2/2: 1 MB of 4 MB (25%)'), [current], 's3', 42)

    def test_same_operation_row_shift_keeps_its_own_batched_counter(self):
        row = {'operationId': 'remaining', 'artifactId': 'a', 'phase': 'uploading',
               'sourceBytes': 4*1024**2, 'bytesTransferred': 1024**2,
               'bytesTotal': 4*1024**2, 'rowIndex': 1, 'activeCount': 1}
        current = {**row, 'rowIndex': 0, 'bytesTransferred': 2*1024**2}
        rows = tray.correlated_records([row, current], [current])
        self.assertTrue(tray.check_tooltip(observation('Uploading: 1 MB of 4 MB (25%)'), rows, 's3', 42)['counter'])

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
        self.assertTrue(tray.check_tooltip(observation('Uploading: 4 MB of 8 MB (50%) · 2 uploading, 1 waiting'),
                                           records, 's3', 42)['counter'])
        for wrong in ('Uploading: 4 MB of 8 MB (50%) · 3 uploading', 'Uploading: 6 MB of 8 MB (75%) · 2 uploading, 1 waiting',
                      'Uploading: 4 MB of 9 MB (44%) · 2 uploading, 1 waiting'):
            with self.subTest(wrong=wrong), self.assertRaises(tray.TrayProgressError):
                tray.check_tooltip(observation(wrong), records, 's3', 42)

    def test_idle_during_active_work_is_a_mismatch(self):
        idle = observation('', color='#d6d6d2', title=tray.IDLE)
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
        replay.snapshot = Mock(return_value=observation('', color='#d6d6d2', title=tray.IDLE))
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

    def attention_replay(self, directory, operation=None, kind='local-save', provider='s3'):
        from unittest.mock import Mock
        replay = object.__new__(tray.Replay)
        mock_reads(replay)
        replay.args = SimpleNamespace(provider=provider, timeout=5)
        replay.pid = 42; replay.history = []; replay.mismatch_since = None
        replay.result = {'coverage': {}}; replay.save = Mock(); replay.witness = Mock()
        replay.output = Path(directory)
        replay.expect_attention(['failed'], kind, required=kind == 'local-save')
        operation = operation or {'id': 'failed', 'state': 'failed', 'items': [
            {'uploadStatus': 'local_only', 'local': {'status': 'failed'}}]}
        replay.cli = Mock(return_value={'operations': [operation]})
        return replay

    LOCAL = 'Local save needs attention'

    def attention(self, label=LOCAL, color='#e59b93', rows=''):
        return observation(label + tray.REVIEW + rows, color, tray.ATTENTION)

    def test_local_failure_latches_until_its_attention_item_is_opened(self):
        from unittest.mock import Mock, patch
        with tempfile.TemporaryDirectory() as directory:
            replay = self.attention_replay(directory)
            replay.snapshot = Mock(side_effect=[self.attention(), self.attention(),
                                                observation('', '#d6d6d2', tray.IDLE)])
            with patch.object(tray.time, 'monotonic', side_effect=[10, 14, 15]):
                first = replay.observe('local-failure')[2]
                self.assertEqual(first['attention'], self.LOCAL)
                self.assertFalse(first['idle'])
                replay.observe('local-failure')
                with self.assertRaisesRegex(tray.TrayProgressError, 'before its attention item was opened'):
                    replay.observe('local-failure')
            latch = replay.result['attention'][0]
            self.assertEqual((latch['failureObservedAt'], latch['attentionObservedAt'], latch['lastAttentionAt']), (10, 10, 14))
            self.assertFalse(replay.result['coverage']['local-failure']['active'])
            self.assertIn('local-failure-attention', [call.args[0] for call in replay.witness.call_args_list])
        with tempfile.TemporaryDirectory() as directory:
            replay = self.attention_replay(directory)
            replay.snapshot = Mock(side_effect=[self.attention(), observation('', '#d6d6d2', tray.IDLE)])
            with patch.object(tray.time, 'monotonic', side_effect=[10, 14]):
                replay.observe('local-failure')
                replay.attention['acknowledgedAt'] = 13
                self.assertTrue(replay.observe('local-failure')[2]['idle'])
            self.assertEqual(replay.attention['clearedAt'], 14)

    def test_attention_without_a_runner_created_failure_is_fatal(self):
        from unittest.mock import Mock, patch
        with self.assertRaisesRegex(tray.TrayProgressError, 'without a runner-created failure'):
            tray.check_tooltip(self.attention(), [], 's3', 42)
        with tempfile.TemporaryDirectory() as directory:
            replay = self.attention_replay(directory)
            replay.attention = None
            replay.snapshot = Mock(return_value=self.attention())
            with patch.object(tray.time, 'monotonic', return_value=10):
                with self.assertRaisesRegex(tray.TrayProgressError, 'without a runner-created failure'):
                    replay.observe('local-failure')

    def test_attention_before_its_failure_is_observed_gets_only_batching_grace(self):
        from unittest.mock import Mock, patch
        running = {'id': 'failed', 'state': 'running', 'phase': 'saving_local', 'items': []}
        with tempfile.TemporaryDirectory() as directory:
            replay = self.attention_replay(directory, running)
            replay.snapshot = Mock(return_value=self.attention())
            with patch.object(tray.time, 'monotonic', side_effect=[10, 12.01]):
                self.assertFalse(replay.observe('local-failure')[2]['matched'])
                with self.assertRaisesRegex(tray.TrayProgressError, 'batching grace'):
                    replay.observe('local-failure')
            self.assertNotIn('attentionObservedAt', replay.attention)

    def test_attention_cannot_borrow_another_failure_identity(self):
        from unittest.mock import Mock, patch
        for operation in (
                {'id': 'other', 'state': 'failed', 'items': [{'uploadStatus': 'local_only', 'local': {'status': 'failed'}}]},
                {'id': 'failed', 'state': 'failed', 'items': [{'uploadStatus': 'failed', 'local': {'status': 'saved'}}]}):
            with self.subTest(operation=operation), tempfile.TemporaryDirectory() as directory:
                replay = self.attention_replay(directory, operation)
                replay.snapshot = Mock(return_value=self.attention())
                with patch.object(tray.time, 'monotonic', side_effect=[10, 12.01]):
                    with self.assertRaises(tray.TrayProgressError):
                        replay.observe('local-failure'); replay.observe('local-failure')

    def test_attention_text_and_glyph_must_match_the_latched_failure(self):
        labels = [self.LOCAL]
        self.assertEqual(tray.check_tooltip(self.attention(), [], 's3', 42, attention=labels)['attention'], self.LOCAL)
        for snapshot in (self.attention('Failed uploads'), observation(self.LOCAL, '#e59b93', tray.ATTENTION),
                         self.attention(color='#d6d6d2'), self.attention(color=tray.TRAY_TINTS['dark']['upload'])):
            with self.subTest(snapshot=snapshot['description']), self.assertRaises(tray.TrayProgressError):
                tray.check_tooltip(snapshot, [], 's3', 42, attention=labels)
        progress = self.attention()
        progress['icons'][0]['opaqueColors'][tray.TRAY_TINTS['dark']['encoding']] = 30
        with self.assertRaises(tray.TrayProgressError):
            tray.check_tooltip(progress, [], 's3', 42, attention=labels)
        # Red belongs to the attention state only.
        with self.assertRaisesRegex(tray.TrayProgressError, 'Error tint'):
            tray.check_tooltip(observation('', '#e59b93', tray.IDLE), [], 's3', 42)
        records = [{'phase': 'uploading', 'bytesTransferred': 2*1024**2, 'bytesTotal': 4*1024**2, 'sourceBytes': 4*1024**2}]
        busy = self.attention('Failed uploads', rows='\nUploading: 2 MB of 4 MB (50%)')
        result = tray.check_tooltip(busy, records, 's3', 42, attention=['Failed uploads'])
        self.assertTrue(result['counter'])
        self.assertFalse(result['storageActive'])

    def test_attention_keeps_privacy_and_identity_guards(self):
        from unittest.mock import Mock, patch
        for defect in ({'pid': 7}, {'hostRegistered': False}, {'registered': False},
                       {'description': 'https://example.invalid/sensitive'}, {'title': 'unexpected'}):
            with self.subTest(defect=defect), tempfile.TemporaryDirectory() as directory:
                replay = self.attention_replay(directory)
                replay.snapshot = Mock(return_value={**self.attention(), **defect})
                with patch.object(tray.time, 'monotonic', return_value=10):
                    with self.assertRaises(tray.TrayProgressError): replay.observe('local-failure')
                evidence = (replay.output/'observations.jsonl').read_text()
                self.assertIn('redacted', evidence); self.assertNotIn('sensitive', evidence)

    def test_proton_busy_latch_clears_when_retried_items_upload(self):
        from unittest.mock import Mock, patch
        op = {'id': 'failed', 'state': 'partial', 'items': [
            {'uploadStatus': 'failed', 'shareStatus': 'not_requested', 'error': {'code': 'proton_account_busy'}}]}
        with tempfile.TemporaryDirectory() as directory:
            replay = self.attention_replay(directory, op, 'proton-busy', 'proton-drive')
            replay.snapshot = Mock(side_effect=[self.attention('Failed uploads'), observation('', '#d6d6d2', tray.IDLE)])
            with patch.object(tray.time, 'monotonic', side_effect=[10, 11]):
                self.assertEqual(replay.observe('concurrent')[2]['attention'], 'Failed uploads')
                self.assertTrue(replay.observe('concurrent')[2]['idle'])
            self.assertEqual(replay.attention['clearedAt'], 11)
            replay.expect_attention(['second'], 'proton-busy')
            self.assertEqual(replay.attention['operationIds'], ['second'])

    def test_only_explicit_original_id_proton_busy_can_authorize_attention(self):
        from unittest.mock import Mock, patch
        for code in ('proton_account_busy', 'proton_permission_denied'):
            with self.subTest(code=code), tempfile.TemporaryDirectory() as directory:
                op = {'id': 'failed', 'state': 'partial', 'items': [
                    {'uploadStatus': 'uploaded', 'shareStatus': 'failed', 'error': {'code': code}}]}
                replay = self.attention_replay(directory, op, 'proton-busy', 'proton-drive')
                replay.snapshot = Mock(return_value=self.attention('Failed uploads'))
                with patch.object(tray.time, 'monotonic', return_value=10):
                    if code == 'proton_account_busy':
                        self.assertEqual(replay.observe('concurrent')[2]['attention'], 'Failed uploads')
                    else:
                        with self.assertRaises(tray.TrayProgressError): replay.observe('concurrent')

    def test_acknowledgement_holds_the_latch_then_opens_the_real_menu_item(self):
        from unittest.mock import Mock, patch
        def replay_with(menu_labels, frames):
            replay = object.__new__(tray.Replay)
            replay.args = SimpleNamespace(timeout=5)
            replay.attention = {'kind': 'local-save', 'required': True, 'attentionObservedAt': 10,
                                'attentionLabel': self.LOCAL}
            replay.witness = Mock()
            replay.observe = Mock(side_effect=[(None, {'service': ':1.5', 'path': '/StatusNotifierItem'}, frame)
                                               for frame in frames])
            replay.tray_menu = Mock(side_effect=lambda snapshot, label=None: {'ok': True, 'labels': menu_labels}
                                    if label is None else {'ok': True, 'clicked': 1, 'label': label})
            replay.result = {}
            replay.open_visibly = Mock(side_effect=tray.VisibleMenuFailed('not under test'))
            return replay
        held = {'matched': True, 'attention': self.LOCAL}
        replay = replay_with(['Capture region', self.LOCAL, 'Quit put.gd'], [held] * 3)
        with patch.object(tray.time, 'monotonic', side_effect=[11, 12, 13.5, 20]), patch.object(tray.time, 'sleep'):
            replay.acknowledge_attention('local-failure')
        self.assertEqual(replay.tray_menu.call_args_list[-1].args[1], self.LOCAL)
        self.assertEqual((replay.attention['acknowledgedAt'], replay.attention['menuLabel']), (20, self.LOCAL))
        replay = replay_with(['Capture region', 'Quit put.gd'], [held] * 3)
        with patch.object(tray.time, 'monotonic', side_effect=[11, 12, 13.5]), patch.object(tray.time, 'sleep'):
            with self.assertRaisesRegex(tray.TrayProgressError, 'does not offer'):
                replay.acknowledge_attention('local-failure')
        replay = replay_with([self.LOCAL], [held, {'matched': True, 'idle': True}])
        with patch.object(tray.time, 'monotonic', side_effect=[11, 12, 12.5]), patch.object(tray.time, 'sleep'):
            with self.assertRaisesRegex(tray.TrayProgressError, 'left the tray'):
                replay.acknowledge_attention('local-failure')

    def test_settling_opens_a_required_latch_before_idle_and_keeps_a_busy_latch(self):
        from unittest.mock import Mock, call
        op = {'id': 'failed', 'state': 'failed', 'copyStatus': 'no_result', 'items': [{
            'artifact': {'id': 'a', 'bytes': 1, 'sha256': 'x'}, 'uploadStatus': 'local_only',
            'local': {'status': 'failed'}}]}
        for kind, observed, expected in (('local-save', {}, ['acknowledge', 'idle']),
                                         ('proton-busy', {'attentionObservedAt': 10}, []),
                                         ('proton-busy', {'attentionObservedAt': 10, 'clearedAt': 11}, ['idle'])):
            with self.subTest(kind=kind, observed=observed):
                replay = object.__new__(tray.Replay)
                replay.args = SimpleNamespace(timeout=5)
                replay.attention = {'kind': kind, 'required': kind == 'local-save', 'failureObservedAt': 9, **observed}
                replay.operations = Mock(return_value={'failed': op}); replay.save = Mock()
                order = Mock()
                replay.acknowledge_attention = order.acknowledge; replay.returned_to_idle = order.idle
                self.assertEqual(replay.settled(['failed'], 'label'), {'failed': op})
                self.assertEqual([name for name, *_ in order.mock_calls], expected)
        replay.attention = {'kind': 'local-save', 'required': True}
        with self.assertRaisesRegex(tray.TrayProgressError, 'not independently observed'):
            replay.settled(['failed'], 'label')

    def test_menu_events_keep_only_dbusmenu_calls_to_the_resident(self):
        import json
        def call(member, payload_type, data, destination=':1.5', interface='com.canonical.dbusmenu', kind='method_call'):
            return json.dumps({'type': kind, 'sender': ':1.9', 'destination': destination, 'interface': interface,
                               'member': member, 'payload': {'type': payload_type, 'data': data}})
        lines = [call('AboutToShow', 'i', [0]), 'not json',
                 call('GetLayout', 'iias', [0, -1, []]),
                 call('Event', 'isvu', [22, 'hovered', {'type': 's', 'data': ''}, 0]),
                 call('EventGroup', 'a(isvu)', [[[21, 'hovered', {'type': 's', 'data': ''}, 0], [22, 'clicked', {}, 0]]]),
                 call('AboutToShowGroup', 'ai', [[0, 30]]),
                 call('Event', 'isvu', [30, 'clicked', {}, 0], destination=':1.6'),
                 call('Event', 'isvu', [30, 'clicked', {}, 0], interface='org.example'),
                 call('Event', 'isvu', [30, 'clicked', {}, 0], kind='signal')]
        self.assertEqual(tray.menu_events(lines, ':1.5'), [('show', 0, None), ('event', 22, 'hovered'),
                         ('event', 21, 'hovered'), ('event', 22, 'clicked'), ('show', 0, None), ('show', 30, None)])
        plasma = [call('AboutToShow', 'i', [0], destination='org.kde.StatusNotifierItem.putgd_x'),
                  call('Event', 'isvu', [0, 'opened', {'type': 's', 'data': ''}, 0], destination='org.kde.StatusNotifierItem.putgd_x'),
                  call('Event', 'isvu', [22, 'clicked', {}, 0], destination='org.kde.StatusNotifierItem.other')]
        self.assertEqual(tray.menu_events(plasma, ':1.5'), [])
        self.assertEqual(tray.menu_events(plasma, {':1.5', 'org.kde.StatusNotifierItem.putgd_x'}),
                         [('show', 0, None), ('event', 0, 'opened')])

    def test_menu_monitor_follows_the_items_unique_owner(self):
        import json
        from unittest.mock import Mock, patch
        replay = object.__new__(tray.Replay)
        replay.guest = '/tmp/tp-x'
        calls = []
        def session(body):
            calls.append(body)
            if 'GetNameOwner' in body:
                return json.dumps({'type': 's', 'data': [':1.163']}) + '\n4242\n'
            return '\n'.join(json.dumps({'type': 'method_call', 'destination': ':1.163', 'interface': 'com.canonical.dbusmenu',
                                          'member': 'AboutToShowGroup', 'payload': {'type': 'ai', 'data': [[0]]}}) for _ in (1,))
        replay.session = session
        with patch.object(tray.time, 'sleep'):
            monitor = replay.menu_monitor('org.kde.StatusNotifierItem-2210-1')
        self.assertEqual((monitor[0], monitor[2], monitor[3]), (4242, ':1.163', 'org.kde.StatusNotifierItem-2210-1'))
        self.assertIn("monitor org.kde.StatusNotifierItem-2210-1 >", calls[0])
        self.assertEqual(replay.menu_monitor_events(monitor), [('show', 0, None)])

    def test_the_latched_icon_is_found_by_its_tint_near_the_hint(self):
        from unittest.mock import Mock, patch
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'frame.ppm'
            width, height = 400, 120
            pixels = bytearray(bytes([30, 30, 30]) * (width * height))
            def paint(x0, y0, size, rgb):
                for y in range(y0, y0 + size):
                    for x in range(x0, x0 + size):
                        pixels[(y * width + x) * 3:(y * width + x) * 3 + 3] = bytes(rgb)
            paint(300, 90, 20, (0xe5, 0x9b, 0x93))
            path.write_bytes(f'P6\n{width} {height}\n255\n'.encode() + bytes(pixels))
            self.assertEqual(tray.locate_tinted_icon(path, '#e59b93', (270, 100)), (309, 99))
            self.assertIsNone(tray.locate_tinted_icon(path, '#e59b93', (20, 20), reach=(100, 40)))
            self.assertIsNone(tray.locate_tinted_icon(path, '#a44139', (270, 100)))
        replay = self.visible_replay([[]])
        with patch.object(tray.lab, 'pointer') as pointer, patch.object(tray.lab, 'key'), patch.object(tray.time, 'sleep'), \
                patch.object(tray, 'locate_tinted_icon', return_value=None):
            with self.assertRaisesRegex(tray.VisibleMenuFailed, 'not near the supplied position'):
                replay.open_visibly({'service': ':1.5', 'path': '/S'}, self.LOCAL, 'local-failure')
        # Only the park move away from the panel (Plasma tints a hovered icon); no click.
        self.assertEqual([c.args[1:] for c in pointer.call_args_list], [(640, 400, 1280, 800)])

    def test_menu_events_accept_the_well_known_item_name_too(self):
        import json
        calls = [json.dumps({'type': 'method_call', 'destination': name, 'interface': 'com.canonical.dbusmenu',
                             'member': 'Event', 'payload': {'type': 'isvu', 'data': [0, 'opened', {}, 0]}})
                 for name in ('org.kde.StatusNotifierItem.putgd_1', ':1.300', ':1.9')]
        # Plasma addresses the well-known name, Cinnamon the unique owner.
        self.assertEqual(tray.menu_events(calls, {':1.300', 'org.kde.StatusNotifierItem.putgd_1'}),
                         [('event', 0, 'opened')] * 2)

    def test_prime_open_opens_and_closes_once_before_the_counted_open(self):
        from unittest.mock import Mock, patch
        shown = [('show', 0, None)]
        replay = self.visible_replay([shown, shown, shown + [('event', 22, 'clicked')]],
                                     profile={'tray_menu': {'primeOpen': True}})
        order = Mock()
        with patch.object(tray.lab, 'pointer', order.pointer), patch.object(tray.lab, 'key', order.key), \
                patch.object(tray.time, 'sleep'), patch.object(tray, 'locate_tinted_icon', return_value=(1158, 15)):
            attempt = replay.open_visibly({'service': ':1.5', 'path': '/S'}, self.LOCAL, 'local-failure')
        self.assertTrue(attempt['primedOpen'])
        steps = [(name, call.args[1:3] if name == 'pointer' else call.args[1]) + tuple(call.args[5:])
                 for name, call in ((c[0], c) for c in order.mock_calls)]
        self.assertEqual(steps[:6], [('pointer', (640, 400)), ('pointer', (1158, 15)), ('pointer', (1158, 15), True, 'right'),
                                     ('pointer', (1158, 15), False, 'right'), ('key', ['esc']), ('pointer', (1158, 15))])
        self.assertEqual([step[1] for step in steps if step[0] == 'key'][-3:], [['down'], ['down'], ['ret']])
        self.assertIn('local-failure-menu-primed', [call.args[0] for call in replay.witness.call_args_list])

    def test_menu_presses_count_visible_top_level_rows(self):
        rows = [{'id': 3, 'label': 'Capture region'}, {'id': 22, 'label': 'Failed uploads (2)'}, {'id': 30, 'label': 'Quit put.gd'}]
        self.assertEqual(tray.menu_presses(rows, 'Failed uploads', False), (rows[1], 2))
        self.assertEqual(tray.menu_presses(rows, 'Failed uploads', True), (rows[1], 1))
        self.assertEqual(tray.menu_presses(rows, 'Local save needs attention', False), (None, None))

    def visible_replay(self, events, rows=None, profile=None):
        from unittest.mock import Mock
        replay = object.__new__(tray.Replay)
        replay.args = SimpleNamespace(tray_x=1200, tray_y=10)
        replay.run, replay.width, replay.height = Path('/run'), 1280, 800
        replay.profile = profile or {}
        replay.result = {}
        replay.witness = Mock()
        rows = rows or [{'id': 3, 'label': 'Capture region'}, {'id': 22, 'label': self.LOCAL}, {'id': 30, 'label': 'Quit put.gd'}]
        replay.tray_menu = Mock(return_value={'ok': True, 'rows': rows, 'labels': [r['label'] for r in rows]})
        replay.menu_monitor = Mock(return_value=(41, '/tmp/tp-x/menu-monitor.jsonl', ':1.163'))
        replay.menu_monitor_events = Mock(side_effect=events)
        replay.stop_menu_monitor = Mock()
        replay.tints = tray.TRAY_TINTS['dark']
        return replay

    def test_visible_menu_reaches_the_row_by_keyboard_and_needs_the_desktops_click(self):
        from unittest.mock import patch
        shown = [('show', 0, None)]
        hovered = shown + [('event', 3, 'hovered'), ('event', 22, 'hovered')]
        replay = self.visible_replay([shown, hovered, hovered + [('event', 22, 'clicked')]])
        with patch.object(tray.lab, 'pointer') as pointer, patch.object(tray.lab, 'key') as key, \
                patch.object(tray.time, 'sleep'), patch.object(tray, 'locate_tinted_icon', return_value=(1124, 779)):
            attempt = replay.open_visibly({'service': ':1.5', 'path': '/StatusNotifierItem'}, self.LOCAL, 'local-failure')
        self.assertEqual((attempt['status'], attempt['label'], attempt['downPresses']), ('passed', self.LOCAL, 2))
        self.assertTrue(attempt['hoverReported'])
        self.assertFalse(attempt['calibrated'])
        self.assertEqual([c.args[1:3] for c in pointer.call_args_list], [(640, 400)] + [(1124, 779)] * 3)
        self.assertEqual([c.args[5:] for c in pointer.call_args_list], [(), (), (True, 'right'), (False, 'right')])
        self.assertEqual(attempt['iconAt'], (1124, 779))
        self.assertEqual([c.args[1] for c in key.call_args_list], [['down'], ['down'], ['ret']])
        replay.stop_menu_monitor.assert_called_once_with((41, '/tmp/tp-x/menu-monitor.jsonl', ':1.163'))
        self.assertEqual(attempt['busOwner'], ':1.163')
        self.assertEqual(replay.result['trayMenu'], [attempt])

    def test_visible_menu_failures_close_the_menu_and_report_their_reason(self):
        from unittest.mock import patch
        shown = [('show', 0, None)]
        cases = (([[]], 'did not open'),
                 ([shown, shown + [('event', 30, 'hovered')]], 'highlight is not on the attention row'))
        for events, reason in cases:
            with self.subTest(reason=reason):
                replay = self.visible_replay(events)
                with patch.object(tray.lab, 'pointer'), patch.object(tray.lab, 'key') as key, patch.object(tray.time, 'sleep'), \
                patch.object(tray, 'locate_tinted_icon', return_value=(1124, 779)):
                    with self.assertRaisesRegex(tray.VisibleMenuFailed, reason):
                        replay.open_visibly({'service': ':1.5', 'path': '/S'}, self.LOCAL, 'local-failure')
                self.assertEqual(key.call_args_list[-1].args[1], ['esc'])
                self.assertEqual(replay.result['trayMenu'][0]['status'], 'failed')
                replay.stop_menu_monitor.assert_called_once()
        replay = self.visible_replay([], rows=[{'id': 30, 'label': 'Quit put.gd'}])
        with self.assertRaisesRegex(tray.VisibleMenuFailed, 'not a top-level'):
            replay.open_visibly({'service': ':1.5', 'path': '/S'}, self.LOCAL, 'local-failure')
        # No click within five seconds; then a click on another item is fatal, not a fallback.
        replay = self.visible_replay([shown, shown] + [shown] * 3)
        with patch.object(tray.lab, 'pointer'), patch.object(tray.lab, 'key'), patch.object(tray.time, 'sleep'), \
                patch.object(tray, 'locate_tinted_icon', return_value=(1124, 779)), \
                patch.object(tray.time, 'monotonic', side_effect=[0, 1, 6]):
            with self.assertRaisesRegex(tray.VisibleMenuFailed, 'no click'):
                replay.open_visibly({'service': ':1.5', 'path': '/S'}, self.LOCAL, 'local-failure')
        replay = self.visible_replay([shown, shown, shown + [('event', 30, 'clicked')]],
                                     profile={'tray_menu': {'firstRowSelected': True}})
        with patch.object(tray.lab, 'pointer'), patch.object(tray.lab, 'key') as key, patch.object(tray.time, 'sleep'), \
                patch.object(tray, 'locate_tinted_icon', return_value=(1124, 779)):
            with self.assertRaisesRegex(tray.TrayProgressError, 'activated another item') as raised:
                replay.open_visibly({'service': ':1.5', 'path': '/S'}, self.LOCAL, 'local-failure')
        self.assertNotIsInstance(raised.exception, tray.VisibleMenuFailed)
        self.assertEqual([c.args[1] for c in key.call_args_list], [['down'], ['ret']])
        self.assertTrue(replay.result['trayMenu'][0]['calibrated'])

    def test_a_failed_visible_menu_falls_back_to_the_dbus_event_and_says_so(self):
        from unittest.mock import Mock, patch
        replay = object.__new__(tray.Replay)
        replay.args = SimpleNamespace(timeout=5)
        replay.attention = {'kind': 'local-save', 'required': True, 'attentionObservedAt': 10, 'attentionLabel': self.LOCAL}
        replay.result = {}; replay.witness = Mock()
        held = {'matched': True, 'attention': self.LOCAL}
        replay.observe = Mock(return_value=(None, {'service': ':1.5', 'path': '/S'}, held))
        replay.open_visibly = Mock(side_effect=tray.VisibleMenuFailed('the desktop did not open put.gd\'s menu'))
        replay.tray_menu = Mock(side_effect=lambda snapshot, label=None: {'ok': True, 'labels': [self.LOCAL]}
                                if label is None else {'ok': True, 'clicked': 1, 'label': label})
        with patch.object(tray.time, 'monotonic', side_effect=[11, 12, 13.5, 20]), patch.object(tray.time, 'sleep'):
            replay.acknowledge_attention('local-failure')
        self.assertEqual(replay.attention['openedBy'], 'dbus-event-fallback')
        self.assertEqual(replay.result['visibleMenuFallbacks'],
                         [{'scenario': 'local-failure', 'reason': 'the desktop did not open put.gd\'s menu'}])
        replay.open_visibly = Mock(return_value={'status': 'passed', 'label': self.LOCAL})
        replay.result, replay.attention = {}, {**replay.attention, 'attentionObservedAt': 10}
        replay.tray_menu.reset_mock()
        with patch.object(tray.time, 'monotonic', side_effect=[11, 12, 13.5, 20]), patch.object(tray.time, 'sleep'):
            replay.acknowledge_attention('local-failure')
        self.assertEqual(replay.attention['openedBy'], 'visible-menu')
        self.assertNotIn('visibleMenuFallbacks', replay.result)
        self.assertTrue(all(call.args[1:] == () for call in replay.tray_menu.call_args_list))

    def test_tray_menu_script_opens_exactly_one_matching_item(self):
        import json, os, stat, subprocess, sys
        def node(ident, label, visible=True, children=()):
            return [ident, {'label': {'type': 's', 'data': label}, 'visible': {'type': 'b', 'data': visible}},
                    [{'type': '(ia{sv}av)', 'data': child} for child in children]]
        separator = [24, {'type': {'type': 's', 'data': 'separator'}, 'label': {'type': 's', 'data': '-'}}, []]
        cases = [(None, [node(21, 'Capture region', children=[node(40, 'Region only')]), separator,
                         node(22, 'Failed uploads (2)'), node(23, 'Hidden', False)], None),
                 ('Failed uploads', [node(22, 'Failed uploads (2)'), node(30, 'Quit put.gd')], '22'),
                 ('Local save needs attention', [node(30, 'Quit put.gd')], None)]
        for wanted, children, event in cases:
            with self.subTest(wanted=wanted), tempfile.TemporaryDirectory() as directory:
                log = Path(directory) / 'calls.jsonl'
                fake = Path(directory) / 'busctl'
                layout = json.dumps({'type': 'u(ia{sv}av)', 'data': [7, node(0, '', children=children)]})
                fake.write_text('#!' + sys.executable + '\nimport json,sys\n'
                    'open(' + repr(str(log)) + ',"a").write(json.dumps(sys.argv[1:])+"\\n")\n'
                    'args=sys.argv[1:]\n'
                    'if "get-property" in args: print(json.dumps({"type":"o","data":"/Menu"}))\n'
                    'elif "GetLayout" in args: print(' + repr(layout) + ')\n')
                fake.chmod(fake.stat().st_mode | stat.S_IXUSR)
                script = tray.TRAY_MENU.replace('SERVICE', repr(':1.5')).replace('PATH', repr('/StatusNotifierItem')) \
                                       .replace('LABEL', repr(wanted))
                env = {**os.environ, 'PATH': directory + os.pathsep + os.environ['PATH'],
                       'DBUS_SESSION_BUS_ADDRESS': 'unix:path=/nonexistent'}
                reply = json.loads(subprocess.run([sys.executable, '-c', script], env=env, capture_output=True,
                                                  text=True, check=True).stdout)
                calls = [json.loads(line) for line in log.read_text().splitlines()]
                events = [call for call in calls if 'Event' in call]
                self.assertTrue(all(call[0] == '--address=unix:path=/nonexistent' for call in calls))
                if wanted is None:
                    self.assertEqual(reply['labels'], ['Capture region', 'Region only', '-', 'Failed uploads (2)'])
                    self.assertEqual(reply['rows'], [{'id': 21, 'label': 'Capture region'}, {'id': 22, 'label': 'Failed uploads (2)'}])
                    self.assertEqual(events, [])
                elif event:
                    self.assertEqual((reply['clicked'], reply['label']), (1, 'Failed uploads (2)'))
                    self.assertEqual(events, [['--address=unix:path=/nonexistent', '--json=short', 'call', ':1.5', '/Menu',
                                               'com.canonical.dbusmenu', 'Event', 'isvu', event, 'clicked', 's', '', '0']])
                else:
                    self.assertEqual(reply, {'ok': False, 'error': 'attention_item_not_unique'})
                    self.assertEqual(events, [])

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
        import re
        source = (ROOT / 'src/ui/Theme.cpp').read_text()
        header = (ROOT / 'src/ui/Theme.h').read_text()
        fields = re.search(r'struct Palette \{(.*?)\};', header, re.S)[1]
        fields = [field.strip() for row in re.findall(r'QColor ([^;]+);', fields)
                  for field in row.split(',')]
        body = source[source.index('const TrayTints& trayTints('):source.index('bool setScheme(')]
        members = re.search(r'return TrayTints\{([^}]+)\}', body)
        self.assertIsNotNone(members)
        members = re.findall(r'readable\(p\.(\w+)\)', members[1])
        self.assertEqual(len(members), 5)
        # These palettes already satisfy the adjustment loop. If a future token
        # needs adjustment, require an explicit lab-table update rather than
        # silently treating the raw token as the rendered tint.
        threshold = re.search(r'contrast\(color,panels\[0\]\)<([0-9.]+)', body)[1]
        self.assertIn('contrast(color,panels[1])<' + threshold, body)
        panels = re.search(r'const QColor panels\[\]= \{([^}]+)\}', body)[1]
        panels = re.findall(r'QColor\("(#[0-9A-Fa-f]{6})"\)', panels)
        self.assertEqual(len(panels), 4)
        def luminance(color):
            channels = [int(color[i:i + 2], 16) / 255 for i in (1, 3, 5)]
            return sum(weight * (c / 12.92 if c <= .03928 else ((c + .055) / 1.055)**2.4)
                       for c, weight in zip(channels, (.2126, .7152, .0722)))
        for name, backgrounds in (('dark', panels[1::2]), ('light', panels[::2])):
            found = re.search(r'const Palette ' + name + r'\{(.*?)\};', source, re.S)
            self.assertIsNotNone(found, name)
            colors = re.findall(r'QColor\(([^)]*)\)', found[1])
            self.assertEqual(len(colors), len(fields))
            palette = dict(zip(fields, colors))
            for role, member in zip(('idle', 'upload', 'encoding', 'error', 'live'), members):
                color = palette[member].strip('"').lower()
                self.assertRegex(color, r'^#[0-9a-f]{6}$')
                for background in backgrounds:
                    light, dark = sorted((luminance(color), luminance(background)), reverse=True)
                    self.assertGreaterEqual((light + .05) / (dark + .05), float(threshold),
                                            name + '/' + role + ' now needs the C++ tint adjustment')
                self.assertEqual(tray.TRAY_TINTS[name][role], color, name + '/' + role)
        art = (ROOT / 'src/ui/IconArt.cpp').read_text()
        self.assertIn('p.fillPath(shape,glyph);', art)
        self.assertIn('p.fillPath(shape,tints.encoding);', art)
        self.assertNotIn('color.darker(230)', art)

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
        active = observation('Uploading: 2 MB of 4 MB (50%)', color=light['idle'])
        active['icons'][0]['opaqueColors'][light['encoding']] = 60
        result = tray.check_tooltip(active, records, 's3', 42, tints=light)
        self.assertTrue(result['storageActive'] and result['counter'])
        with self.assertRaises(tray.TrayProgressError):
            tray.check_tooltip(active, records, 's3', 42)
        dark_on_light = observation('Uploading: 2 MB of 4 MB (50%)', color=self.DARK['upload'])
        with self.assertRaises(tray.TrayProgressError):
            tray.check_tooltip(dark_on_light, records, 's3', 42, tints=light)
        self.assertFalse(tray.check_tooltip(observation('Upload complete', color=light['upload']), [], 's3', 42, tints=light)['idle'])
        with self.assertRaises(tray.TrayProgressError):
            tray.check_tooltip(active | {'description': 'Upload complete'}, [], 's3', 42, tints=light)
        latched = 'Local save needs attention' + tray.REVIEW
        red = tray.check_tooltip(observation(latched, light['error'], tray.ATTENTION), [], 's3', 42,
                                 attention=['Local save needs attention'], tints=light)
        self.assertEqual(red['attention'], 'Local save needs attention')
        with self.assertRaises(tray.TrayProgressError):
            tray.check_tooltip(observation(latched, self.DARK['error'], tray.ATTENTION), [], 's3', 42,
                               attention=['Local save needs attention'], tints=light)

    def test_light_panel_attention_uses_the_light_error_tint(self):
        from unittest.mock import Mock, patch
        light = self.LIGHT
        with tempfile.TemporaryDirectory() as directory:
            replay = TrayProgressContracts.attention_replay(self, directory)
            replay.tints = light
            replay.snapshot = Mock(side_effect=[observation(TrayProgressContracts.LOCAL + tray.REVIEW, light['error'], tray.ATTENTION),
                                                observation('', light['idle'], tray.IDLE)])
            with patch.object(tray.time, 'monotonic', side_effect=[10, 14]):
                self.assertEqual(replay.observe('local-failure')[2]['attention'], TrayProgressContracts.LOCAL)
                replay.attention['acknowledgedAt'] = 13
                self.assertTrue(replay.observe('local-failure')[2]['idle'])

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
            replay = self.tone_replay(directory, light_panel, {'panel': 'light', 'source': 'kde-default', 'color': '#383834'})
            self.assertEqual(replay.read_panel_tone()['panel'], 'light')
            self.assertIs(replay.tints, self.LIGHT)
            self.assertEqual(replay.result['panelTone']['reportedByApp']['source'], 'kde-default')
            dark_panel = {'desktop': 'ubuntu:GNOME'}
            replay = self.tone_replay(directory, dark_panel, None)
            self.assertEqual(replay.read_panel_tone()['source'], 'gnome-shell')
            self.assertIs(replay.tints, self.DARK)
            replay = self.tone_replay(directory, light_panel, {'panel': 'dark', 'source': 'default', 'color': '#d6d6d2'})
            with self.assertRaisesRegex(tray.TrayProgressError, 'tray panel'):
                replay.read_panel_tone()


if __name__ == '__main__':
    unittest.main()
