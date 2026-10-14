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
                      {'publicUrlBase': 'https://example.invalid/?token=synthetic'},
                      {'publicUrlBase': 'https://example.invalid/#synthetic'}]
            for extra in cases:
                changed = {**profile, 'settings': {**profile['settings'], **extra}}
                path.write_text(json.dumps({'profiles': [changed]}))
                with self.subTest(extra=extra), self.assertRaises(tray.TrayProgressError):
                    tray.private_settings(path, 'test', 's3', 'tray-unique')


if __name__ == '__main__':
    unittest.main()
