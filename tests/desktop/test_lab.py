"""Host contracts only: these tests never establish compositor acceptance."""
import importlib.util
import contextlib
import io
import json
import hashlib
from types import SimpleNamespace
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
        # The test generates pixels independently of the harness oracle.
        def crop(x, y, width=80, height=60):
            colors = ((220, 30, 40), (20, 180, 70), (35, 70, 220), (235, 190, 20))
            return bytes(channel for row in range(y, y + height)
                         for column in range(x, x + width)
                         for channel in colors[(row >= 160) * 2 + (column >= 320)])

        region = {'width': 80, 'height': 60, 'areas': [
            {'x': 0, 'y': 0, 'width': 40, 'height': 30, 'rgb': [220, 30, 40]},
            {'x': 40, 'y': 0, 'width': 40, 'height': 30, 'rgb': [20, 180, 70]},
            {'x': 0, 'y': 30, 'width': 40, 'height': 30, 'rgb': [35, 70, 220]},
            {'x': 40, 'y': 30, 'width': 40, 'height': 30, 'rgb': [235, 190, 20]}]}
        self.assertEqual(lab.read_json(ROOT / 'tests/desktop/common.yaml')['oracle'], region)
        clipboard = {'width': 96, 'height': 64, 'areas': [
            {'x': 0, 'y': 0, 'width': 96, 'height': 64, 'rgb': [220, 30, 40]}]}
        with tempfile.TemporaryDirectory() as directory:
            for witness in ('artifact.ppm', 'paste.ppm'):
                path = Path(directory) / witness
                for kind, expected, correct in (
                        ('region', region, crop(280, 130)),
                        ('clipboard-image', clipboard, bytes([220, 30, 40]) * 96 * 64)):
                    header = f"P6\n{expected['width']} {expected['height']}\n255\n".encode()
                    with self.subTest(witness=witness, kind=kind, defect='none'):
                        path.write_bytes(header + correct)
                        self.assertTrue(lab.compare_ppm(path, expected)['ok'])
                    with self.subTest(witness=witness, kind=kind, defect='unsampled corruption'):
                        corrupted = bytearray(correct)
                        corrupted[-3:] = bytes([0, 0, 0])
                        path.write_bytes(header + corrupted)
                        self.assertFalse(lab.compare_ppm(path, expected)['ok'])
                with self.subTest(witness=witness, defect='same-size shifted crop'):
                    path.write_bytes(b'P6\n80 60\n255\n' + crop(300, 150))
                    self.assertFalse(lab.compare_ppm(path, region)['ok'])
            for kind, width, height in (('window', 680, 370), ('desktop', 800, 600)):
                expected = {'width': width, 'height': height, 'areas': [
                    {'x': 10, 'y': 20, 'width': 320, 'height': 160, 'rgb': [220, 30, 40]},
                    {'x': 330, 'y': 20, 'width': 320, 'height': 160, 'rgb': [20, 180, 70]},
                    {'x': 10, 'y': 180, 'width': 320, 'height': 160, 'rgb': [35, 70, 220]},
                    {'x': 330, 'y': 180, 'width': 320, 'height': 160, 'rgb': [235, 190, 20]}]}
                self.assertEqual(lab.fixture_oracle(
                    {'x': 100, 'y': 200, 'width': width, 'height': height},
                    {'x': 110, 'y': 220}), expected)
                correct = bytearray(width * height * 3)
                for row in range(320):
                    offset = ((20 + row) * width + 10) * 3
                    correct[offset:offset + 640 * 3] = crop(0, row, 640, 1)
                for witness in ('artifact.ppm', 'paste.ppm'):
                    path = Path(directory) / witness
                    header = f'P6\n{width} {height}\n255\n'.encode()
                    with self.subTest(witness=witness, kind=kind, defect='none'):
                        path.write_bytes(header + correct)
                        self.assertTrue(lab.compare_ppm(path, expected)['ok'])
                    with self.subTest(witness=witness, kind=kind, defect='far paint corruption'):
                        corrupted = bytearray(correct)
                        offset = ((20 + 319) * width + 10 + 639) * 3
                        corrupted[offset:offset + 3] = bytes([0, 0, 0])
                        path.write_bytes(header + corrupted)
                        self.assertFalse(lab.compare_ppm(path, expected)['ok'])
            self.assertFalse(lab.compare_paste({'text': 'wrong'},
                {'text': 'https://fixture.invalid/object'})['ok'])

    def test_region_contract_crosses_all_fixture_quadrants(self):
        contract = lab.read_json(ROOT / 'tests/desktop/common.yaml')
        x, y = contract['input']['drag']['paintOffset']
        width, height = contract['input']['drag']['size']
        self.assertLess(x, 320)
        self.assertGreater(x + width, 320)
        self.assertLess(y, 160)
        self.assertGreater(y + height, 160)

    def test_attempt_bundles_retain_all_current_witness_bytes_and_hashes(self):
        with tempfile.TemporaryDirectory() as directory:
            run = Path(directory)
            lab.write_json(run / 'state.json', {'profile': 'cinnamon-x11',
                'acceleration': 'kvm', 'guest_user': 'fixture', 'id': 'synthetic'})
            settings = run / 'synthetic-settings.json'
            lab.write_json(settings, {})
            ready = {'session': 'x11', 'qt_platform': 'xcb', 'desktop': 'X-Cinnamon',
                'qt_version': '6.4.2', 'display_layout': [
                    {'x': 0, 'y': 0, 'width': 800, 'height': 600}],
                'paint': {'x': 10, 'y': 20, 'width': 640, 'height': 320}}
            saved = []
            for sequence, scenario in enumerate(('clipboard-text', 'region-cancel-escape', 'clipboard-text')):
                for number in (1, 2):
                    marker = f'{scenario}-{sequence}-{number}'
                    url = 'https://fixture.invalid/' + marker
                    payload = b'putgd synthetic clipboard text'
                    cancelled = scenario.startswith('region-cancel-')
                    recent_calls = 0
                    produced = {}
                    screenshot_count = 0
                    visibility_checks = 0
                    operation = {'id': marker, 'state': 'cancelled' if cancelled else 'completed',
                        'copyStatus': 'suppressed' if cancelled else 'copied', 'items': [] if cancelled else [
                            {'local': {'status': 'saved', 'path': '/synthetic/result'},
                             'artifact': {'id': marker, 'sha256': hashlib.sha256(payload).hexdigest(),
                                          'bytes': len(payload)},
                             'uploadStatus': 'uploaded', 'receipt': {'directUrl': url}}]}
                    def fake_cli(_run, method, _params=None):
                        nonlocal recent_calls
                        if method == 'settings.get':
                            return {'metadata': {'acceptedRevision': 1}}
                        if method == 'diagnostics':
                            return {'journalReady': True}
                        if method == 'capabilities':
                            return {'source': {'shortcuts': {'bindings': {
                                action: {'bound': True} for action in ('region', 'window', 'desktop', 'clipboard')}}}}
                        if method == 'operation.recent':
                            recent_calls += 1
                            return {'operations': [] if recent_calls == 1 else [operation]}
                        if method == 'operation.inspect':
                            return operation
                        if method == 'history.detail':
                            return {'receipt': {'directUrl': url}}
                        return {}
                    def fake_get(_run, remote, target):
                        target = Path(target)
                        if target.name == 'ready.json':
                            lab.write_json(target, ready)
                        elif remote == '/synthetic/result':
                            target.write_bytes(payload)
                        else:
                            lab.write_json(target, {'text': 'putgd synthetic clipboard text' if cancelled else url,
                                                    'syntheticAttempt': marker})
                        produced[target.name] = target.read_bytes()
                    def fake_screenshot(_run, name='witness', head=0):
                        nonlocal screenshot_count
                        screenshot_count += 1
                        path = run / f'{name}-head{head}.ppm'
                        path.write_bytes(marker.encode() + b':' + name.encode() + str(screenshot_count).encode())
                        produced[path.name] = path.read_bytes()
                        return {'path': str(path), 'sha256': lab.hash_file(path)}
                    def fake_visibility(*_args):
                        nonlocal visibility_checks
                        visibility_checks += 1
                        return {'ok': visibility_checks > 1}
                    args = SimpleNamespace(run=str(run), scenario=scenario, clipboard_client='qt',
                        settings=None if cancelled else str(settings), history=not cancelled,
                        timeout=3, keep=True)
                    with contextlib.ExitStack() as stack:
                        for name, replacement in (
                                ('cli', fake_cli), ('get', fake_get), ('screenshot', fake_screenshot),
                                ('guest_session', lambda *_args: ''), ('verify_running_app', lambda *_args: {'pid': 42}),
                                ('compare_ppm', fake_visibility), ('pointer', lambda *_args: None),
                                ('key', lambda *_args: None), ('collect', lambda *_args: None)):
                            stack.enter_context(patch.object(lab, name, side_effect=replacement))
                        stack.enter_context(patch.object(lab.time, 'sleep'))
                        stack.enter_context(patch('urllib.request.urlopen', return_value=io.BytesIO(payload)))
                        result = lab.fixture_run(args)
                    self.assertTrue(result['ok'], result)
                    evidence = run / result['evidenceDirectory']
                    manifest = lab.read_json(evidence / 'manifest.json')['files']
                    with self.subTest(attempt=marker, defect='overwritten focus poll'):
                        self.assertEqual(sum(name.endswith('.ppm') for name in produced), screenshot_count)
                    for name, data in produced.items():
                        with self.subTest(attempt=marker, witness=name):
                            self.assertTrue((evidence / name).is_file(), name)
                            if (evidence / name).is_file():
                                self.assertEqual((evidence / name).read_bytes(), data)
                                self.assertEqual(manifest[name], hashlib.sha256(data).hexdigest())
                    if cancelled:
                        self.assertNotIn('history-paste.json', manifest)
                        for stale in ('history-paste.json', 'artifact-0', 'public-0'):
                            self.assertFalse((run / stale).exists(), stale)
                            self.assertNotIn(stale, manifest)
                    else:
                        self.assertNotIn('selector-head0.ppm', manifest)
                        self.assertFalse((run / 'selector-head0.ppm').exists())
                    saved.append((evidence, produced.copy()))
            # A later attempt must never mutate an earlier archive.
            for evidence, produced in saved:
                for name, data in produced.items():
                    self.assertEqual((evidence / name).read_bytes(), data)

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
