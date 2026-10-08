"""Host contracts only: these tests never establish compositor acceptance."""
import importlib.util
import contextlib
import io
import json
from pathlib import Path
import socket
import tempfile
import threading
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('putgd_lab', ROOT / 'tools/lab/lab.py')
lab = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lab)


class LabContracts(unittest.TestCase):
    def test_failed_install_cannot_reuse_previous_package_receipt(self):
        with tempfile.TemporaryDirectory() as directory:
            receipt = Path(directory) / 'package.json'
            for failed_stage in ('upload', 'verification'):
                with self.subTest(failed_stage=failed_stage):
                    receipt.write_text(json.dumps({'installedBinarySha256': {
                        '/usr/bin/putgd': 'a' * 64}}))
                    with patch('sys.argv', ['lab.py', 'install', '--run', directory,
                                           '--deb', '/tmp/synthetic-new-package.deb']), \
                            patch.object(lab, 'put', side_effect=lab.LabError('upload failed')
                                         if failed_stage == 'upload' else None), \
                            patch.object(lab, 'ssh', return_value=''), \
                            patch.object(lab, 'verify_installed_binaries',
                                         side_effect=lab.LabError('verification failed')), \
                            contextlib.redirect_stdout(io.StringIO()):
                        self.assertEqual(lab.main(), 1)
                    self.assertFalse(receipt.exists())
                    with self.assertRaisesRegex(lab.LabError, 'package receipt'):
                        lab.verify_running_app(Path(directory))

    def test_running_app_must_match_installed_package(self):
        with tempfile.TemporaryDirectory() as directory:
            run = Path(directory)
            expected = 'a' * 64
            (run / 'package.json').write_text(json.dumps({
                'installedBinarySha256': {'/usr/bin/putgd': expected}}))
            current = {'pid': 42, 'startTicks': 123, 'sha256': expected}
            with patch.object(lab, 'ssh', return_value=json.dumps([current])):
                self.assertEqual(lab.verify_running_app(run), current)
            # An upgrade can replace /usr/bin/putgd while the old inode runs.
            old = {**current, 'sha256': 'b' * 64}
            with patch.object(lab, 'ssh', return_value=json.dumps([old])):
                with self.assertRaisesRegex(lab.LabError, 'running.*package'):
                    lab.verify_running_app(run)
            for processes in ([], [current, {**current, 'pid': 43}]):
                with patch.object(lab, 'ssh', return_value=json.dumps(processes)):
                    with self.assertRaisesRegex(lab.LabError, 'exactly one'):
                        lab.verify_running_app(run)

    def test_running_app_requires_verified_install_receipt(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(lab.LabError, 'package receipt'):
                lab.verify_running_app(Path(directory))

    def test_test_journal_retains_each_attempt_as_valid_json(self):
        with tempfile.TemporaryDirectory() as directory:
            run = Path(directory)
            lab.test_event(run, {'event': 'started', 'testId': 'first'})
            lab.test_event(run, {'event': 'finished', 'testId': 'first', 'ok': False})
            lab.test_event(run, {'event': 'started', 'testId': 'second'})
            entries = [json.loads(line) for line in (run / 'test-events.jsonl').read_text().splitlines()]
            self.assertEqual([entry['testId'] for entry in entries], ['first', 'first', 'second'])
            self.assertFalse(entries[1]['ok'])
            self.assertTrue(all(entry.get('timestampUtc') for entry in entries))

    def test_launch_is_disposable_and_control_channels_are_local(self):
        profile = lab.load_profile('cinnamon-x11')
        command = lab.qemu_command(profile, Path('/tmp/run'), Path('/tmp/run/disk.qcow2'), 22001, 9, 'kvm')
        joined = ' '.join(command)
        self.assertIn('hostfwd=tcp:127.0.0.1:22001-:22', joined)
        self.assertIn('127.0.0.1:9', joined)
        self.assertIn('unix:/tmp/run/qmp.sock', joined)
        self.assertIn('/tmp/run/disk.qcow2', joined)
        self.assertNotIn('spice', joined)
        self.assertNotIn('virtio-serial', joined)
        self.assertNotIn('base.qcow2', joined)
        self.assertIn('-daemonize', command)
        self.assertIn('/tmp/run/qemu.pid', command)

    def test_wayland_readiness_rejects_xwayland_and_tcg_cannot_certify_performance(self):
        profile = {'session': 'wayland', 'qt_platform': 'wayland', 'desktop': 'GNOME'}
        actual = {'session': 'x11', 'qt_platform': 'xcb', 'desktop': 'GNOME', 'display_layout': [{}]}
        with self.assertRaisesRegex(lab.LabError, 'session'):
            lab.validate_readiness(profile, actual, 'kvm')
        actual.update(session='wayland', qt_platform='wayland')
        result = lab.validate_readiness(profile, actual, 'tcg')
        self.assertEqual(result['evidence_class'], 'functional-only')
        self.assertFalse(result['performance_eligible'])
        kvm = lab.validate_readiness(profile, actual, 'kvm')
        self.assertEqual(kvm['evidence_class'], 'graphical-kvm')
        self.assertFalse(kvm['performance_eligible'])

    def test_exact_pixels_reject_wrong_crop_and_clipboard(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'actual.ppm'
            path.write_bytes(b'P6\n2 1\n255\n' + bytes([220, 30, 40, 20, 180, 70]))
            expected = {'width': 2, 'height': 1, 'samples': [{'x': 0, 'y': 0, 'rgb': [220, 30, 40]}]}
            self.assertTrue(lab.compare_ppm(path, expected)['ok'])
            expected['samples'][0]['rgb'] = [20, 180, 70]
            self.assertFalse(lab.compare_ppm(path, expected)['ok'])
            expected['width'] = 3
            self.assertFalse(lab.compare_ppm(path, expected)['ok'])
            self.assertFalse(lab.compare_paste({'text': 'wrong'}, {'text': 'https://fixture.invalid/object'})['ok'])

    def test_qmp_handles_unsolicited_events_without_losing_command_reply(self):
        with tempfile.TemporaryDirectory() as directory:
            path = str(Path(directory) / 'qmp.sock')
            server = socket.socket(socket.AF_UNIX)
            server.bind(path)
            server.listen(1)
            errors = []
            def peer():
                try:
                    connection, _ = server.accept()
                    with connection, connection.makefile('rwb') as stream:
                        stream.write(b'{"QMP":{"version":{"qemu":{"major":8}},"capabilities":[]}}\n')
                        stream.flush()
                        for _ in range(2):
                            request = json.loads(stream.readline())
                            stream.write(json.dumps({'event': 'STOP'}).encode() + b'\n')
                            stream.write(json.dumps({'return': {} if request['execute'] == 'qmp_capabilities' else {'status': 'running'}, 'id': request['id']}).encode() + b'\n')
                            stream.flush()
                except Exception as error:
                    errors.append(error)
            thread = threading.Thread(target=peer)
            thread.start()
            with lab.QMP(path, timeout=2) as qmp:
                self.assertEqual(qmp.execute('query-status'), {'status': 'running'})
                self.assertEqual(len(qmp.events), 2)
            thread.join(3)
            server.close()
            self.assertFalse(thread.is_alive())
            self.assertEqual(errors, [])

    def test_wayland_fixture_origin_requires_complete_independent_paint(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'witness.ppm'
            width, height, x, y = 700, 400, 10, 20
            pixels = bytearray(width * height * 3)
            top = bytes([220, 30, 40]) * 320 + bytes([20, 180, 70]) * 320
            bottom = bytes([35, 70, 220]) * 320 + bytes([235, 190, 20]) * 320
            for row in range(320):
                offset = ((y + row) * width + x) * 3
                pixels[offset:offset + 640 * 3] = top if row < 160 else bottom
            header = f'P6\n{width} {height}\n255\n'.encode()
            path.write_bytes(header + pixels)
            self.assertEqual(lab.locate_fixture_paint(path),
                             {'x': x, 'y': y, 'width': 640, 'height': 320})
            pixels[((y + 200) * width + x + 100) * 3] = 0
            path.write_bytes(header + pixels)
            self.assertIsNone(lab.locate_fixture_paint(path))

    def test_public_bytes_and_independent_link_paste_must_match_receipt(self):
        with tempfile.TemporaryDirectory() as directory:
            artifact = Path(directory) / 'artifact'
            public = Path(directory) / 'public'
            artifact.write_bytes(b'synthetic bytes')
            public.write_bytes(b'synthetic bytes')
            import hashlib
            digest = hashlib.sha256(artifact.read_bytes()).hexdigest()
            operation = {'state': 'completed', 'items': [{'artifact': {'sha256': digest},
                'uploadStatus': 'uploaded', 'receipt': {'directUrl': 'https://fixture.invalid/object'}}]}
            self.assertTrue(lab.verify_publication(operation, [artifact], [public],
                {'text': 'https://fixture.invalid/object'})['ok'])
            public.write_bytes(b'wrong object')
            self.assertFalse(lab.verify_publication(operation, [artifact], [public],
                {'text': 'https://fixture.invalid/object'})['ok'])
            public.write_bytes(b'synthetic bytes')
            self.assertFalse(lab.verify_publication(operation, [artifact], [public],
                {'text': 'https://fixture.invalid/wrong'})['ok'])

    def test_locked_cinnamon_iso_matches_authenticated_bootstrap(self):
        lock = json.loads((ROOT / 'tools/lab/image-lock.json').read_text())
        image = lock['images']['cinnamon-x11']
        self.assertEqual(image['sha256'], 'a081ab202cfda17f6924128dbd2de8b63518ac0531bcfe3f1a1b88097c459bd4')
        self.assertEqual(image['signingFingerprint'], '27DEB15644C6B3CF3BD7D291300F846BA25BAE09')


if __name__ == '__main__':
    unittest.main()
