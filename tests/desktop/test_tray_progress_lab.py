"""Pure host contracts; no VM, service or graphical acceptance is implied."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('tray_progress_lab', ROOT / 'tools/lab/tray_progress.py')
tray = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tray)


def observation(description, color='#62d69b', title='put.gd — Working'):
    return {'pid': 42, 'hostRegistered': True, 'registered': True,
            'title': title, 'description': description,
            'icons': [{'width': 24, 'height': 24, 'sha256': 'synthetic',
                       'opaqueColors': {color: 120}}]}


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

    def test_proton_encrypted_counter_never_uses_source_as_denominator(self):
        # The SDK may retry and decrease its encrypted byte counter.
        records = [{'phase': 'uploading', 'sourceBytes': 4*1024**2,
                    'bytesTransferred': n*1024**2} for n in (8, 6)]
        result = tray.check_tooltip(observation('Uploading: 7 MiB transferred · Source size: 4 MiB'), records, 'proton-drive', 42)
        self.assertTrue(result['counter'])
        for text in ('Uploading: 7 MiB of 4 MiB (175%)',
                     'Uploading: 7 MiB transferred · Source size: 7 MiB',
                     'Uploading: 12 MiB transferred · Source size: 4 MiB'):
            with self.subTest(text=text), self.assertRaises(tray.TrayProgressError):
                tray.check_tooltip(observation(text), records, 'proton-drive', 42)

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
                       {'icons': observation('Queued', color='#9bbcff')['icons']}):
            with self.subTest(defect=defect), self.assertRaises(tray.TrayProgressError):
                tray.check_tooltip({**observation('Queued'), **defect}, records, 's3', 42)
        idle = observation('', color='#9bbcff', title='put.gd — Share and local capture')
        self.assertTrue(tray.check_tooltip(idle, [], 's3', 42)['idle'])
        with self.assertRaises(tray.TrayProgressError):
            tray.check_tooltip({**idle, 'description': 'Uploading'}, [], 's3', 42)

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

    def test_idle_during_active_work_is_a_mismatch(self):
        idle = observation('', color='#9bbcff', title=tray.IDLE)
        with self.assertRaises(tray.TrayProgressError):
            tray.check_tooltip(idle, [{'phase': 'uploading'}], 's3', 42)

    def test_persistent_idle_fails_after_bounded_batching_grace_and_is_preserved(self):
        from types import SimpleNamespace
        from unittest.mock import Mock, patch
        import json
        replay = object.__new__(tray.Replay)
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


if __name__ == '__main__':
    unittest.main()
