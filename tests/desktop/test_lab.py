"""Host contracts only: these tests never establish compositor acceptance."""
import importlib.util
import contextlib
import io
import json
import hashlib
import shutil
from types import SimpleNamespace
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('putgd_lab', ROOT / 'tools/lab/lab.py')
lab = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lab)


class LabContracts(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='lab-lock-')
        self.addCleanup(temporary.cleanup)
        lock_root = patch.object(lab.overlay_cleanup, 'ROOT', Path(temporary.name))
        lock_root.start()
        self.addCleanup(lock_root.stop)
        # These tests supply synthetic QMP/session fixtures. The real endpoint,
        # policy and fail-closed checks have their own guest-preflight suite.
        preflight = patch.object(lab, 'guest_preflight', return_value={'ok': True, 'synthetic': True})
        preflight.start()
        self.addCleanup(preflight.stop)

    def test_failed_drag_witness_releases_button_and_cannot_archive_stale_image(self):
        # Exercise the production attempt handler, not a copied gesture.
        for failed_stage, cleanup_failure in (('press', None), ('move', None), ('witness', None),
                                               ('witness', 'cancel'), ('witness', 'release'),
                                               ('endpoint-release', None), ('witness', 'observation')):
            with self.subTest(failed_stage=failed_stage, cleanup_failure=cleanup_failure), tempfile.TemporaryDirectory() as directory:
                run = Path(directory)
                lab.write_json(run / 'state.json', {'profile': 'cinnamon-x11',
                    'acceleration': 'kvm', 'guest_user': 'fixture', 'id': 'synthetic'})
                (run / 'drag-head0.ppm').write_bytes(b'previous attempt drag')
                ready = {'session': 'x11', 'qt_platform': 'xcb', 'desktop': 'X-Cinnamon',
                    'qt_version': '6.4.2', 'display_layout': [
                        {'x': 0, 'y': 0, 'width': 800, 'height': 600}],
                    'paint': {'x': 10, 'y': 20, 'width': 640, 'height': 320}}
                events = []
                keys = []
                trace = []
                recent_calls = 0
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
                        if cleanup_failure == 'observation' and recent_calls > 1:
                            raise lab.LabError('synthetic outcome observation failure')
                        return {'operations': [] if recent_calls == 1 else [
                            {'id': 'failed-gesture', 'state': 'cancelled', 'copyStatus': 'suppressed'}]}
                    return {}
                def fake_pointer(*args):
                    events.append(args[1:])
                    trace.append(('pointer', args[1:]))
                    if failed_stage == 'press' and args[1:3] == (290, 150) and args[-1] is True:
                        raise lab.LabError('synthetic drag press failure')
                    if cleanup_failure == 'release' and args[1:3] != (40, 50) and args[-1] is False:
                        raise lab.LabError('synthetic cleanup release failure')
                    if failed_stage == 'endpoint-release' and args[1:3] == (370, 210) and args[-1] is False:
                        raise lab.LabError('synthetic drag endpoint-release failure')
                    if failed_stage == 'move' and len(args) == 5:
                        raise lab.LabError('synthetic drag move failure')
                def fake_key(_run, pressed):
                    keys.append(pressed)
                    trace.append(('key', pressed))
                    if cleanup_failure == 'cancel' and pressed == ['esc']:
                        raise lab.LabError('synthetic cleanup cancel failure')
                def fake_screenshot(_run, name='witness', head=0):
                    if name == 'drag' and failed_stage != 'endpoint-release':
                        raise lab.LabError('synthetic drag witness failure')
                    path = run / f'{name}-head{head}.ppm'
                    path.write_bytes(b'current attempt:' + name.encode())
                    return {'path': str(path), 'sha256': lab.hash_file(path)}
                args = SimpleNamespace(run=str(run), scenario='region', clipboard_client='qt',
                    settings=None, history=False, timeout=3, keep=failed_stage != 'press')
                disk = run / 'disk.qcow2'
                disk.write_bytes(b'retain failed guest for investigation')
                with contextlib.ExitStack() as stack:
                    for name, replacement in (
                            ('cli', fake_cli), ('get', lambda _run, _remote, target: lab.write_json(target, ready)),
                            ('screenshot', fake_screenshot), ('pointer', fake_pointer),
                            ('guest_session', lambda *_args: ''), ('verify_running_app', lambda *_args: {'pid': 42}),
                            ('compare_ppm', lambda *_args: {'ok': True}),
                            ('key', fake_key), ('collect', lambda *_args: None)):
                        stack.enter_context(patch.object(lab, name, side_effect=replacement))
                    stack.enter_context(patch.object(lab.time, 'sleep'))
                    stack.enter_context(patch.object(lab, 'reset', side_effect=lab.LabError('busy lifecycle')))
                    cleanup = stack.enter_context(patch.object(lab, 'cleanup_overlay'))
                    result = lab.fixture_run(args)
                    cleanup.assert_not_called()
                self.assertTrue(disk.exists())
                self.assertFalse(result['ok'])
                self.assertEqual(result['error'], 'synthetic drag ' + failed_stage + ' failure')
                self.assertTrue(result['dragAbortRequested'])
                self.assertEqual(keys[-1], ['esc'])
                self.assertEqual(trace[-2:], [('key', ['esc']),
                    ('pointer', (290, 150, 800, 600, False))])
                if cleanup_failure == 'observation':
                    self.assertTrue(result['dragOperationObservationIncomplete'])
                    self.assertNotIn('operationsAfterFailedDrag', result)
                else:
                    self.assertEqual(result['operationsAfterFailedDrag'], [
                        {'id': 'failed-gesture', 'state': 'cancelled', 'copyStatus': 'suppressed'}])
                if cleanup_failure in ('cancel', 'release'):
                    self.assertEqual(result['dragCleanupErrors'], [{'stage': cleanup_failure,
                        'error': 'synthetic cleanup ' + cleanup_failure + ' failure'}])
                else:
                    self.assertNotIn('dragCleanupErrors', result)
                with self.subTest(check='release'):
                    self.assertEqual(events[-1], (290, 150, 800, 600, False))
                with self.subTest(check='stale witness'):
                    if failed_stage == 'endpoint-release':
                        self.assertEqual((run / 'drag-head0.ppm').read_bytes(), b'current attempt:drag')
                    else:
                        self.assertFalse((run / 'drag-head0.ppm').exists())
                evidence = run / result['evidenceDirectory']
                manifest = lab.read_json(evidence / 'manifest.json')['files']
                if failed_stage == 'endpoint-release':
                    self.assertEqual(manifest['drag-head0.ppm'],
                        hashlib.sha256(b'current attempt:drag').hexdigest())
                else:
                    self.assertNotIn('drag-head0.ppm', manifest)
                self.assertEqual((evidence / 'final-head0.ppm').read_bytes(), b'current attempt:final')

    def test_failed_install_cannot_reuse_previous_package_receipt(self):
        with tempfile.TemporaryDirectory() as directory:
            receipt = Path(directory) / 'package.json'
            for failed_stage in ('upload', 'verification'):
                with self.subTest(failed_stage=failed_stage):
                    receipt.write_text(json.dumps({'installedBinarySha256': {
                        '/usr/bin/putgd': 'a' * 64}}))
                    (Path(directory) / 'state.json').write_text(json.dumps({'profile': 'cinnamon-x11'}))
                    with patch('sys.argv', ['lab.py', 'install', '--run', directory,
                                           '--deb', '/tmp/synthetic-new-package.deb']), \
                            patch.object(lab, 'put', side_effect=lab.LabError('upload failed')
                                         if failed_stage == 'upload' else None), \
                            patch.object(lab, 'ssh', return_value=json.dumps({'ok': True, 'timedOut': False})), \
                            patch.object(lab, 'get'), \
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

    def test_launch_rejects_noncanonical_run_before_any_allocation(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for relative in ('outside', 'work/lab-runs/nested/run', 'work/lab-runs'):
                args = SimpleNamespace(run=str(root / relative))
                with self.subTest(relative=relative), patch.object(lab.overlay_cleanup, 'ROOT', root), \
                        patch.object(lab, 'command') as command, \
                        patch.object(lab, '_launch_machine') as allocate:
                    with self.assertRaisesRegex(lab.LabError, 'canonical.*107-byte'):
                        lab.launch(args)
                    command.assert_not_called()
                    allocate.assert_not_called()
                    self.assertEqual(list(root.iterdir()), [])

    def test_launch_rejects_overlong_utf8_sockets_before_allocation(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            run = root / 'work/lab-runs' / ('é' * 45)
            with patch.object(lab.overlay_cleanup, 'ROOT', root), patch.object(lab, '_launch_machine') as allocate:
                with self.assertRaisesRegex(lab.LabError, 'socket paths exceed.*107-byte'):
                    lab.launch(SimpleNamespace(run=str(run)))
                allocate.assert_not_called()
            self.assertEqual(list(root.iterdir()), [])

    def test_launch_rejects_symlinked_canonical_path(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'outside').mkdir()
            (root / 'work').mkdir()
            (root / 'work/lab-runs').symlink_to(root / 'outside')
            with patch.object(lab.overlay_cleanup, 'ROOT', root), patch.object(lab, '_launch_machine') as allocate:
                with self.assertRaisesRegex(lab.LabError, '[Ss]ymlink'):
                    lab.launch(SimpleNamespace(run=str(root / 'work/lab-runs/c1')))
                allocate.assert_not_called()
            self.assertEqual(list((root / 'outside').iterdir()), [])

    def test_install_rpm_is_rejected_before_guest_work(self):
        with patch.object(lab, 'put') as put, patch.object(lab, 'ssh') as ssh:
            with self.assertRaisesRegex(lab.LabError, 'requires a .deb.*RPM'):
                lab.install_package('unused', 'synthetic.rpm')
            put.assert_not_called()
            ssh.assert_not_called()

    def test_install_stages_and_verifies_helper_before_package(self):
        with tempfile.TemporaryDirectory() as directory:
            run = Path(directory)
            lab.write_json(run / 'state.json', {'profile': 'cinnamon-x11'})
            calls = []
            def ssh(_run, script, **kwargs):
                calls.append(script)
                return json.dumps({'ok': True, 'timedOut': False}) if 'result.json; then cat' in script else ''
            with patch.object(lab, 'ssh', side_effect=ssh), patch.object(lab, 'put') as put, \
                    patch.object(lab, 'get'), patch.object(lab, 'verify_installed_binaries', return_value={}), \
                    patch.object(lab, 'hash_file', return_value='a' * 64):
                result = lab.install_package(run, 'synthetic.deb')
            self.assertEqual(Path(put.call_args_list[0].args[1]).name, 'package_install.py')
            self.assertEqual(Path(put.call_args_list[1].args[1]).name, 'package_scope.py')
            self.assertTrue(put.call_args_list[2].args[2].endswith('/package.deb'))
            stage = next(c for c in calls if 'Pre-Invoke' in c)
            # Helper and scope launcher: uploaded copy, installed copy, final check.
            self.assertEqual(stage.count('sha256sum -c -'), 6)
            grant = next(line for line in stage.splitlines() if 'NOPASSWD' in line)
            self.assertIn('NOPASSWD: /usr/local/lib/putgd-lab/package_install.py, '
                          '/usr/local/lib/putgd-lab/package_scope.py\\n', grant)
            self.assertNotIn('systemd-run', grant)
            self.assertIn('Dir::Etc::sourcelist=/dev/null', stage)
            self.assertIn('Dir::Etc::sourceparts=-', stage)
            self.assertIn('APT::Get::List-Cleanup=0', stage)
            self.assertIn('visudo -cf', stage)
            self.assertIn('exit 1', stage)
            self.assertNotIn('authorized_keys', stage)
            self.assertTrue(lab.read_json(Path(result['installEvidence']) / 'helper.json')['verified'])

    def test_launch_rejects_wrong_host_key_endpoint_before_creating_run(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            golden = root / 'golden.qcow2'
            golden.write_bytes(b'sealed test identity')
            trusted = root / 'known_hosts'
            trusted.write_text('[127.0.0.1]:22220 ssh-ed25519 AAAA\n')
            run = root / 'work/lab-runs/new-run'
            args = SimpleNamespace(profile='cinnamon-x11', golden=str(golden),
                run=str(run), acceleration='tcg', ssh_port=22431,
                known_hosts=str(trusted), bootstrap_key=str(root / 'identity'))
            original_command = lab.command
            def read_only_command(arguments, **kwargs):
                socket_check = (arguments[0] == sys.executable and
                                Path(arguments[1]).name == 'check_socket_paths.py')
                if not socket_check and (arguments[0] != 'ssh-keygen' or '-F' not in arguments):
                    self.fail('Launch mutated resources before rejecting an unpinned endpoint')
                return original_command(arguments, **kwargs)
            with patch.object(lab.overlay_cleanup, 'ROOT', root), patch.object(lab, 'command', side_effect=read_only_command):
                with self.assertRaisesRegex(lab.LabError, r'22431.*known.hosts'):
                    lab.launch(args)
            self.assertFalse(run.exists())
            self.assertEqual(golden.read_bytes(), b'sealed test identity')

    def test_launch_accepts_plain_and_hashed_pinned_endpoints(self):
        for port, endpoint in ((22, '127.0.0.1'), (22431, '[127.0.0.1]:22431')):
            for hashed in (False, True):
                with self.subTest(port=port, hashed=hashed), tempfile.TemporaryDirectory() as directory:
                    root = Path(directory)
                    golden = root / 'golden.qcow2'
                    golden.write_bytes(b'sealed test identity')
                    trusted = root / 'known_hosts'
                    trusted.write_text(endpoint + ' ssh-ed25519 AAAA\n')
                    if hashed:
                        lab.command(['ssh-keygen', '-H', '-f', trusted])
                    run = root / 'work/lab-runs/existing-run'
                    run.mkdir(parents=True)
                    args = SimpleNamespace(profile='cinnamon-x11', golden=str(golden),
                        run=str(run), acceleration='tcg', ssh_port=port,
                        known_hosts=str(trusted), bootstrap_key=str(root / 'identity'))
                    # Passing preflight reaches the existing-run guard, before any allocation.
                    with patch.object(lab.overlay_cleanup, 'ROOT', root), self.assertRaises(FileExistsError):
                        lab.launch(args)
                    self.assertEqual(list(run.iterdir()), [])

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

    def test_bios_profiles_keep_their_qemu_command_and_uefi_adds_private_pflash(self):
        for name in ('cinnamon-x11', 'gnome-wayland', 'kde-wayland', 'fedora-gnome-wayland'):
            profile = lab.load_profile(name)
            self.assertIsNone(lab.uefi_firmware(profile))
            command = lab.qemu_command(profile, Path('/tmp/run'), Path('/tmp/run/disk.qcow2'), 22001, 9, 'kvm')
            self.assertNotIn('pflash', ' '.join(command))
            self.assertEqual(command, lab.qemu_command(profile, Path('/tmp/run'), Path('/tmp/run/disk.qcow2'),
                                                       22001, 9, 'kvm', None))
        aurora = lab.load_profile('aurora-kde-wayland')
        self.assertEqual(aurora['firmware'], 'uefi')
        self.assertEqual(aurora['install'], 'appimage')
        firmware = {'type': 'uefi', 'code': '/usr/share/edk2/ovmf/OVMF_CODE_4M.qcow2', 'format': 'qcow2',
                    'vars_template': '/usr/share/edk2/ovmf/OVMF_VARS_4M.qcow2', 'vars': '/tmp/run/vars.qcow2'}
        command = lab.qemu_command(aurora, Path('/tmp/run'), Path('/tmp/run/disk.qcow2'), 22426, 26, 'kvm', firmware)
        self.assertIn('if=pflash,format=qcow2,readonly=on,file=/usr/share/edk2/ovmf/OVMF_CODE_4M.qcow2', command)
        self.assertIn('if=pflash,format=qcow2,file=/tmp/run/vars.qcow2', command)
        # The shared code image is never opened writable; only the run's copy is.
        self.assertNotIn('if=pflash,format=qcow2,file=/usr/share/edk2/ovmf/OVMF_VARS_4M.qcow2', command)
        self.assertIn('-daemonize', command)
        with patch.object(lab, 'UEFI_FIRMWARE', ()):
            with self.assertRaisesRegex(lab.LabError, 'UEFI firmware unavailable'):
                lab.uefi_firmware(aurora)
        with self.assertRaisesRegex(lab.LabError, 'firmware'):
            lab.uefi_firmware({**aurora, 'firmware': 'coreboot'})

    def test_uefi_launch_records_a_private_nvram_copy_in_run_state(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            golden = root / 'golden.qcow2'
            golden.write_bytes(b'sealed test identity')
            code, template = root / 'OVMF_CODE_4M.qcow2', root / 'OVMF_VARS_4M.qcow2'
            code.write_bytes(b'shared firmware code')
            template.write_bytes(b'pristine nvram template')
            trusted = root / 'known_hosts'
            trusted.write_text('[127.0.0.1]:22426 ssh-ed25519 AAAA\n')
            run = root / 'runs' / 'aurora-test'
            args = SimpleNamespace(profile='aurora-kde-wayland', golden=str(golden), run=str(run),
                acceleration='kvm', ssh_port=22426, vnc_display=26, known_hosts=str(trusted),
                bootstrap_key=str(root / 'identity'))
            launched = []
            original = lab.command
            def fake_command(arguments, **kwargs):
                if arguments[0] == 'qemu-system-x86_64':
                    launched.append(arguments)
                    (run / 'qemu.pid').write_text('4242\n')
                    return SimpleNamespace(returncode=0, stderr='')
                if arguments[0] == 'qemu-img':
                    Path(arguments[-1]).write_bytes(b'overlay')
                    return SimpleNamespace(returncode=0, stderr='', stdout='')
                if arguments[0] == 'ssh-keygen' and '-t' in arguments:
                    Path(arguments[-1]).write_bytes(b'private')
                    Path(str(arguments[-1]) + '.pub').write_text('ssh-ed25519 AAAA test\n')
                    return SimpleNamespace(returncode=0, stderr='', stdout='')
                return original(arguments, **kwargs)
            firmware = {'type': 'uefi', 'code': str(code), 'vars_template': str(template), 'format': 'qcow2'}
            golden_stat = golden.stat()
            identity = (golden_stat.st_dev, golden_stat.st_ino, golden_stat.st_size, golden_stat.st_mtime_ns)
            with patch.object(lab, 'command', side_effect=fake_command):
                info = lab._launch_machine(args, lab.load_profile('aurora-kde-wayland'), golden, run,
                                           'f' * 64, identity, firmware)
            vars_copy = run / 'vars.qcow2'
            self.assertEqual(vars_copy.read_bytes(), b'pristine nvram template')
            self.assertEqual(template.read_bytes(), b'pristine nvram template')
            self.assertEqual(vars_copy.stat().st_mode & 0o777, 0o600)
            recorded = json.loads((run / 'state.json').read_text())['firmware']
            self.assertEqual(recorded['vars'], str(vars_copy))
            self.assertEqual(recorded['code'], str(code))
            self.assertEqual(recorded['code_sha256'], hashlib.sha256(b'shared firmware code').hexdigest())
            self.assertEqual(recorded['vars_template_sha256'], hashlib.sha256(b'pristine nvram template').hexdigest())
            self.assertEqual(info['firmware'], recorded)
            joined = ' '.join(launched[0])
            self.assertIn(f'if=pflash,format=qcow2,readonly=on,file={code}', joined)
            self.assertIn(f'if=pflash,format=qcow2,file={vars_copy}', joined)

    def test_install_dispatches_by_profile_and_appimage_receipt_feeds_running_app_check(self):
        with tempfile.TemporaryDirectory() as directory:
            run = Path(directory)
            (run / 'state.json').write_text(json.dumps({'profile': 'aurora-kde-wayland'}))
            with patch.object(lab, 'put'), patch.object(lab, 'ssh'), patch.object(lab, 'install_package') as package:
                with self.assertRaisesRegex(lab.LabError, 'AppImage'):
                    lab.install(run, deb='/tmp/synthetic.deb')
                package.assert_not_called()
            (run / 'state.json').write_text(json.dumps({'profile': 'kde-wayland'}))
            with patch.object(lab, 'install_appimage') as appimage:
                with self.assertRaisesRegex(lab.LabError, 'distro package'):
                    lab.install(run, appimage='/tmp/putgd-x86_64.AppImage')
                appimage.assert_not_called()
            self.assertFalse((run / 'package.json').exists())
            # The AppImage path copies to the guest home, verifies bytes, extracts
            # the bundled binaries and records the identity it reports.
            (run / 'state.json').write_text(json.dumps({'profile': 'aurora-kde-wayland'}))
            image = run / 'putgd-0.1.1-dev.419+gtest-x86_64.AppImage'
            image.write_bytes(b'synthetic appimage bytes')
            inner = hashlib.sha256(b'inner resident').hexdigest()
            report = {'home': '/var/home/lab', 'version': {'version': '0.1.1-dev.419+gtest'},
                      'binaries': {'/var/home/lab/putgd-lab/appimage/squashfs-root/usr/bin/putgd': inner,
                                   '/var/home/lab/putgd-lab/appimage/squashfs-root/usr/bin/putgd-cli': 'b' * 64}}
            uploads, scripts = [], []
            with patch.object(lab, 'put', side_effect=lambda _run, local, remote: uploads.append((local, remote))), \
                 patch.object(lab, 'ssh', side_effect=lambda _run, script, **k: scripts.append(script) or json.dumps(report)):
                value = lab.install(run, appimage=str(image))
            self.assertEqual(value['format'], 'appimage')
            self.assertEqual(uploads, [(str(image), image.name)])
            script = scripts[0]
            self.assertIn(hashlib.sha256(b'synthetic appimage bytes').hexdigest(), script)
            self.assertIn('--appimage-extract', script)
            self.assertIn('--version-json', script)
            self.assertIn('putgd-lab/bin/putgd-launcher', script)
            self.assertIn('putgd-lab/bin/putgd-cli', script)
            self.assertIn('putgd-lab/bin/putgd-record', script)
            receipt = json.loads((run / 'package.json').read_text())
            self.assertEqual(receipt['installedBinarySha256'], {'/usr/bin/putgd': inner, '/usr/bin/putgd-cli': 'b' * 64})
            self.assertEqual(receipt['installedPath'], '/var/home/lab/' + image.name)
            self.assertEqual(receipt['version']['version'], '0.1.1-dev.419+gtest')
            with patch.object(lab, 'ssh', return_value=json.dumps([{'pid': 7, 'startTicks': 1, 'sha256': inner}])):
                self.assertEqual(lab.verify_running_app(run)['sha256'], inner)
            # A guest copy with different bytes fails before any receipt exists.
            with patch.object(lab, 'put'), patch.object(lab, 'ssh', side_effect=lab.LabError('ssh failed (exit 65)')):
                with self.assertRaises(lab.LabError):
                    lab.install(run, appimage=str(image))
            self.assertFalse((run / 'package.json').exists())

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

    @unittest.skipUnless(shutil.which('qemu-img') and shutil.which('qemu-io'), 'requires QEMU disk utilities')
    def test_attempt_bundles_retain_all_current_witness_bytes_and_hashes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            run = root / 'work/lab-runs/scenario'
            run.mkdir(parents=True)
            golden = root / 'work/lab-images/golden.qcow2'
            golden.parent.mkdir(parents=True)
            lab.command(['qemu-img', 'create', '-q', '-f', 'qcow2', golden, '1M'])
            lab.write_json(run / 'state.json', {'schemaVersion': 1, 'profile': 'cinnamon-x11',
                'acceleration': 'kvm', 'guest_user': 'fixture', 'id': 'synthetic',
                'overlay': str(run / 'disk.qcow2'), 'golden': str(golden)})
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
                        timeout=3, keep=not (sequence == 2 and number == 2))
                    disk = run / 'disk.qcow2'
                    disk.unlink(missing_ok=True)
                    lab.command(['qemu-img', 'create', '-q', '-f', 'qcow2', '-F', 'qcow2', '-b', golden, disk])
                    stopped = []
                    original_cleanup = lab.cleanup_overlay
                    def clean_completed_disk(_run, **_kwargs):
                        self.assertEqual(stopped, [run])
                        last_event = json.loads((run / 'test-events.jsonl').read_text().splitlines()[-1])
                        self.assertEqual(last_event['event'], 'finished')
                        bundle = run / last_event['evidenceDirectory']
                        for name, digest in lab.read_json(bundle / 'manifest.json')['files'].items():
                            self.assertEqual(hashlib.sha256((bundle / name).read_bytes()).hexdigest(), digest)
                        return original_cleanup(_run, **_kwargs)
                    with contextlib.ExitStack() as stack:
                        for name, replacement in (
                                ('cli', fake_cli), ('get', fake_get), ('screenshot', fake_screenshot),
                                ('guest_session', lambda *_args: ''), ('verify_running_app', lambda *_args: {'pid': 42}),
                                ('compare_ppm', fake_visibility), ('pointer', lambda *_args: None),
                                ('key', lambda *_args: None), ('collect', lambda *_args: None)):
                            stack.enter_context(patch.object(lab, name, side_effect=replacement))
                        stack.enter_context(patch.object(lab.time, 'sleep'))
                        stack.enter_context(patch.object(lab, 'reset', side_effect=lambda path: stopped.append(path)))
                        stack.enter_context(patch.object(lab, 'cleanup_overlay', side_effect=clean_completed_disk))
                        stack.enter_context(patch.object(lab.overlay_cleanup, 'ROOT', root))
                        stack.enter_context(patch.object(lab.overlay_cleanup, '_check_processes'))
                        stack.enter_context(patch.object(lab.overlay_cleanup, '_check_chains'))
                        stack.enter_context(patch('urllib.request.urlopen', return_value=io.BytesIO(payload)))
                        result = lab.fixture_run(args)
                    self.assertTrue(result['ok'], result)
                    self.assertEqual(disk.exists(), args.keep)
                    if not args.keep:
                        self.assertEqual(lab.read_json(run / 'overlay-cleanup.json')['status'], 'completed')
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


if __name__ == '__main__':
    unittest.main()
