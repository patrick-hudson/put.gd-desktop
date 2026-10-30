"""Host contracts only: these tests never establish compositor acceptance."""
import importlib.util
import contextlib
import fcntl
import io
import json
import hashlib
import os
import shutil
import signal
import time
from types import SimpleNamespace
from pathlib import Path
import socket
import sys
import tempfile
import threading
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

    def test_guest_session_recovers_gnome_wayland_display_from_session_manager(self):
        # gnome-shell's own environment lacks WAYLAND_DISPLAY; without it the
        # resident falls back to its graphical route instead of the GNOME core.
        scripts = []
        with patch.object(lab, 'state', return_value={'guest_user': 'lab', 'profile': 'gnome-wayland'}), \
             patch.object(lab, 'ssh', side_effect=lambda _run, script, *a, **k: scripts.append(script) or ''):
            lab.guest_session('run', 'putgd-launcher --background\n')
        script = scripts[0]
        self.assertIn(lab.native_display_prefix(), script)
        self.assertLess(script.index('export XDG_RUNTIME_DIR=/run/user/'), script.index(lab.native_display_prefix()))
        self.assertLess(script.index(lab.native_display_prefix()), script.index('putgd-launcher --background'))

    def test_guest_session_takes_the_desktop_sessions_config_and_data_dirs(self):
        # #243: PanelTone reads XDG_CONFIG_DIRS (Plasma's ~/.config/kdedefaults)
        # and XDG_DATA_DIRS (the Plasma style's colors file); a resident started
        # through guest_session must see the desktop shell's values, not SSH's.
        import os, subprocess
        session = {'PATH': os.environ['PATH'], 'XDG_SESSION_TYPE': 'x11', 'DISPLAY': ':7',
                   'XDG_CONFIG_DIRS': '/home/lab/.config/kdedefaults:/etc/xdg',
                   'XDG_DATA_DIRS': '/usr/share/plasma-data:/usr/share'}
        scripts = []
        with patch.object(lab, 'state', return_value={'guest_user': 'lab', 'profile': 'plasma-x11'}), \
             patch.object(lab, 'load_profile', return_value={'session_process': 'plasmashell', 'qt_platform': 'xcb'}), \
             patch.object(lab, 'ssh', side_effect=lambda _run, script, *a, **k: scripts.append(script) or ''):
            lab.guest_session('run', 'printf "%s|%s|%s" "$XDG_CONFIG_DIRS" "$XDG_DATA_DIRS" "$DISPLAY"\n')
        for shell_values in (session, {key: value for key, value in session.items() if not key.startswith('XDG_') or key == 'XDG_SESSION_TYPE'}):
            with self.subTest(set=sorted(shell_values)), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                shell = subprocess.Popen(['sleep', '30'], env=shell_values)
                try:
                    # Stands in for pgrep, so no real desktop shell on this host is read.
                    (root / 'pgrep').write_text('#!/bin/sh\necho ' + str(shell.pid) + '\n')
                    (root / 'pgrep').chmod(0o700)
                    # The SSH login's own values, which the session's must replace.
                    result = subprocess.check_output(['sh', '-c', scripts[0]], text=True,
                                                     env={'PATH': str(root) + ':' + os.environ['PATH'], 'HOME': directory,
                                                          'XDG_CONFIG_DIRS': '/etc/xdg', 'XDG_DATA_DIRS': '/ssh/share'})
                finally:
                    shell.kill(); shell.wait()
                if 'XDG_CONFIG_DIRS' in shell_values:
                    self.assertEqual(result, '/home/lab/.config/kdedefaults:/etc/xdg|/usr/share/plasma-data:/usr/share|:7')
                else:
                    # Unset in the session: the login's values stay, as before.
                    self.assertEqual(result, '/etc/xdg|/ssh/share|:7')

    def test_guest_session_shows_appimage_entries_to_the_compositor(self):
        # #523: AppRun's host entries (KWin permission, icon entries named by the
        # windows' app IDs) go to XDG_DATA_HOME/applications. KWin reads the
        # session's data home, so only applications/ points there; put.gd's own
        # data stays in the private data home.
        import os, subprocess
        body = 'printf "%s|%s" "$XDG_DATA_HOME" "$(readlink "$XDG_DATA_HOME/applications")"\n'
        def script(install):
            scripts = []
            with patch.object(lab, 'state', return_value={'guest_user': 'lab', 'profile': 'neon-kde-wayland'}), \
                 patch.object(lab, 'load_profile', return_value={'session_process': 'plasmashell', 'qt_platform': 'wayland',
                                                                 'install': install}), \
                 patch.object(lab, 'ssh', side_effect=lambda _run, text, *a, **k: scripts.append(text) or ''):
                lab.guest_session('run', body)
            return scripts[0]
        def run(text, home, session):
            root = home / 'bin'
            root.mkdir(exist_ok=True)
            shell = subprocess.Popen(['sleep', '30'], env=dict(session, PATH=os.environ['PATH']))
            try:
                # Stands in for pgrep, so no real desktop shell on this host is read.
                (root / 'pgrep').write_text('#!/bin/sh\necho ' + str(shell.pid) + '\n')
                (root / 'pgrep').chmod(0o700)
                return subprocess.check_output(['sh', '-c', text], text=True,
                                               env={'PATH': str(root) + ':' + os.environ['PATH'], 'HOME': str(home)})
            finally:
                shell.kill(); shell.wait()
        appimage = script('appimage')
        for session_data in (None, 'custom-data'):
            with self.subTest(session_data=session_data), tempfile.TemporaryDirectory() as directory:
                home = Path(directory)
                session = {'XDG_SESSION_TYPE': 'wayland', 'WAYLAND_DISPLAY': '/nonexistent/wayland-0'}
                if session_data:
                    session['XDG_DATA_HOME'] = str(home / session_data)
                expected = home / (session_data or '.local/share') / 'applications'
                self.assertEqual(run(appimage, home, session), f'{home}/putgd-lab/data|{expected}')
                self.assertTrue(expected.is_dir())
                # Entries AppRun writes through the link land where KWin looks.
                (home / 'putgd-lab/data/applications/putgd-ui.desktop').write_text('[Desktop Entry]\n')
                self.assertTrue((expected / 'putgd-ui.desktop').is_file())
                # Every later session command keeps the same link.
                self.assertEqual(run(appimage, home, session), f'{home}/putgd-lab/data|{expected}')
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory)
            session = {'XDG_SESSION_TYPE': 'wayland', 'WAYLAND_DISPLAY': '/nonexistent/wayland-0'}
            # An overlay whose private applications/ already holds files keeps it.
            (home / 'putgd-lab/data/applications').mkdir(parents=True)
            (home / 'putgd-lab/data/applications/putgd-appimage-OLD.desktop').write_text('[Desktop Entry]\n')
            self.assertEqual(run(appimage, home, session), f'{home}/putgd-lab/data|')
            self.assertTrue((home / 'putgd-lab/data/applications/putgd-appimage-OLD.desktop').is_file())
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory)
            session = {'XDG_SESSION_TYPE': 'wayland', 'WAYLAND_DISPLAY': '/nonexistent/wayland-0'}
            # Package profiles install their entries system-wide; nothing is linked.
            self.assertNotIn('session_applications', script('deb'))
            self.assertEqual(run(script('deb'), home, session), f'{home}/putgd-lab/data|')
            self.assertFalse((home / '.local/share/applications').exists())

    def test_tablet_ticks_preserve_pixel_targets_for_native_rounding(self):
        # Actual KWin fullscreen probe: old ticks 15372/17421 delivered
        # 600.46875/680.5078125, so Qt selected 81 instead of 80 pixels.
        # The tablet range has 32768 ticks, including zero.
        for width, height in ((1280, 800), (1920, 1080), (3840, 2160)):
            for x, y in ((0, 0), (600, 307), (680, 367), (width-1, height-1)):
                with self.subTest(viewport=(width, height), target=(x, y)), \
                        patch.object(lab, 'input_events') as send:
                    lab.pointer('/unused', x, y, width, height, True)
                    events = send.call_args.args[1]
                    for event, target, extent in zip(events[:2], (x, y), (width, height)):
                        ticks = event['data']['value']
                        self.assertGreaterEqual(ticks, 0)
                        self.assertLessEqual(ticks, 32767)
                        # Wayland fixed-point positions quantize to 1/256 pixel;
                        # native consumers use either floor or Qt qRound.
                        raw_fixed = ticks * extent / 32768 * 256
                        for quantized in (int(raw_fixed), int(raw_fixed + .5)):
                            delivered = quantized / 256
                            self.assertEqual(int(delivered), target)
                            self.assertEqual(int(delivered + .5), target)
                    self.assertEqual(events[2], {'type': 'btn', 'data': {'button': 'left', 'down': True}})

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

    def test_install_timeout_options_and_reconciled_receipt(self):
        with tempfile.TemporaryDirectory() as directory:
            run = Path(directory)
            deb = run / 'synthetic.deb'
            deb.write_bytes(b'synthetic')
            lab.write_json(run / 'state.json', {'profile': 'cinnamon-x11'})
            report = {'ok': True, 'timedOut': True, 'ownedProcesses': [{'pid': 123, 'startTicks': 45}]}
            calls = []
            def ssh(_run, script, **kwargs):
                calls.append(script)
                return json.dumps(report) if 'result.json; then cat' in script else ''
            with patch.object(lab, 'ssh', side_effect=ssh), patch.object(lab, 'put'), \
                    patch.object(lab, 'get'), patch.object(lab, 'verify_installed_binaries', return_value={'/usr/bin/putgd': 'a' * 64}):
                result = lab.install(run, deb=deb, install_timeout=1200, install_recovery_timeout=300)
            self.assertTrue(result['timedOut'])
            self.assertEqual(lab.read_json(Path(result['installEvidence']) / 'result.json'), report)
            self.assertTrue(any('1200 300' in call for call in calls))
            self.assertEqual(sum(' launch-supervisor "$HOME/' in call for call in calls), 1)

    def test_install_known_safe_failures_release_only_this_jobs_lock(self):
        for stage in ('upload', 'collection', 'verification'):
            with self.subTest(stage=stage), tempfile.TemporaryDirectory() as directory:
                run = Path(directory)
                lab.write_json(run / 'state.json', {'profile': 'cinnamon-x11'})
                calls = []
                def ssh(_run, script, **kwargs):
                    calls.append(script)
                    return json.dumps({'ok': True, 'timedOut': False}) if 'result.json; then cat' in script else ''
                with patch.object(lab, 'ssh', side_effect=ssh), \
                        patch.object(lab, 'put', side_effect=lab.LabError('upload') if stage == 'upload' else None), \
                        patch.object(lab, 'get', side_effect=lab.LabError('collection') if stage == 'collection' else None), \
                        patch.object(lab, 'verify_installed_binaries', side_effect=lab.LabError('verification')):
                    with self.assertRaisesRegex(lab.LabError, stage):
                        lab.install_package(run, 'synthetic.deb')
                release = [c for c in calls if 'rmdir' in c]
                self.assertEqual(len(release), 1)
                self.assertIn('if [ "$(cat ~/putgd-lab/install-active/job)" = install-', release[0])
                if stage == 'upload':
                    self.assertFalse(any(' launch-supervisor "$HOME/' in c for c in calls))

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
            self.assertTrue(put.call_args_list[1].args[2].endswith('/package.deb'))
            stage = next(c for c in calls if 'Pre-Invoke' in c)
            self.assertEqual(stage.count('sha256sum -c -'), 3)
            self.assertIn('Dir::Etc::sourcelist=/dev/null', stage)
            self.assertIn('Dir::Etc::sourceparts=-', stage)
            self.assertIn('APT::Get::List-Cleanup=0', stage)
            self.assertIn('visudo -cf', stage)
            self.assertIn('exit 1', stage)
            self.assertNotIn('authorized_keys', stage)
            self.assertTrue(lab.read_json(Path(result['installEvidence']) / 'helper.json')['verified'])

    def test_install_fallback_is_printed_with_the_failed_prerequisite(self):
        with contextlib.redirect_stderr(io.StringIO()) as output:
            lab.report_staging_tracking({'tracking': 'session', 'fallbackReason':
                'passwordless sudo for systemd-run probe failed'}, 'supervisor')
        self.assertIn('supervisor uses session-only tracking', output.getvalue())
        self.assertIn('passwordless sudo for systemd-run probe failed', output.getvalue())

    def test_install_staging_lock_error_names_holder_without_running_hook(self):
        self.check_install_staging_lock_error(
            'E: Could not get lock /var/lib/apt/lists/lock. It is held by process 12345 (apt-get)',
            r'12345 \(apt-get\)')

    def test_install_staging_lock_error_without_holder_reports_unknown(self):
        self.check_install_staging_lock_error(
            'E: Could not get lock /var/lib/apt/lists/lock - open (11: Resource temporarily unavailable)',
            r'unknown \(unknown\)')

    def check_install_staging_lock_error(self, message, holder):
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory)
            remote = 'putgd-lab/install-test'
            (home / remote).mkdir(parents=True)
            source = home / remote / 'package_install.py'
            source.write_text('synthetic helper')
            sudo = home / 'sudo'
            sudo.write_text('#!/bin/sh\n'
                f'echo "{message}"\n'
                'echo "E: Unable to lock directory /var/lib/apt/lists/"\nexit 100\n')
            sudo.chmod(0o700)
            systemctl = home / 'systemctl'
            systemctl.write_text('#!/bin/sh\nexit 1\n')
            systemctl.chmod(0o700)
            def ssh(_run, script):
                result = lab.subprocess.run(['sh', '-s'], input=script, text=True, capture_output=True,
                    env={**os.environ, 'HOME': str(home), 'PATH': str(home) + ':' + os.environ['PATH']}, timeout=5)
                self.assertEqual(result.returncode, 0, result.stderr)
                return result.stdout
            with patch.object(lab, 'ssh', side_effect=ssh):
                with self.assertRaisesRegex(lab.LabError, f'apt lists lock.*holder PID {holder}.*staging.log'):
                    lab.stage_install_helper('unused', remote, '/nonexistent/helper', lab.hash_file(source))
            self.assertIn(message, (home / remote / 'staging.log').read_text())

    def test_install_staging_failure_releases_lock_without_launch_and_collects_log(self):
        with tempfile.TemporaryDirectory() as directory:
            run = Path(directory)
            lab.write_json(run / 'state.json', {'profile': 'cinnamon-x11'})
            def get(_run, remote, local, timeout):
                self.assertTrue(remote.endswith('/staging.log'))
                self.assertEqual(timeout, 10)
                Path(local).write_text('staging failure evidence')
            with patch.object(lab, 'ssh') as ssh, patch.object(lab, 'put'), \
                    patch.object(lab, 'get', side_effect=get), \
                    patch.object(lab, 'reconcile_staging', return_value={'finished': True, 'state': 'staging-finished'}), \
                    patch.object(lab, 'stage_install_helper', side_effect=lab.LabError('hash mismatch')):
                ssh.return_value = ''
                with self.assertRaisesRegex(lab.LabError, 'hash mismatch'):
                    lab.install_package(run, 'synthetic.deb')
                scripts = [c.args[1] for c in ssh.call_args_list]
                self.assertFalse(any(' launch-supervisor "$HOME/' in c for c in scripts))
                self.assertTrue(any('rmdir' in c for c in scripts))
            self.assertEqual(next(run.glob('install-*/staging.log')).read_text(), 'staging failure evidence')

    def test_install_staging_log_collection_is_best_effort(self):
        for error in (lab.LabError('missing log'), OSError('offline'), lab.subprocess.TimeoutExpired('scp', 10)):
            with self.subTest(error=type(error).__name__), tempfile.TemporaryDirectory() as directory:
                run = Path(directory)
                lab.write_json(run / 'state.json', {'profile': 'cinnamon-x11'})
                with patch.object(lab, 'ssh', return_value='') as ssh, patch.object(lab, 'put'), \
                        patch.object(lab, 'get', side_effect=error) as get, \
                        patch.object(lab, 'reconcile_staging', return_value={'finished': True, 'state': 'staging-finished'}), \
                    patch.object(lab, 'stage_install_helper', side_effect=lab.LabError('original staging failure')):
                    with self.assertRaisesRegex(lab.LabError, 'original staging failure'):
                        lab.install_package(run, 'synthetic.deb')
                    self.assertEqual(get.call_args.kwargs['timeout'], 10)
                    self.assertTrue(any('rmdir' in c.args[1] for c in ssh.call_args_list))

    def test_install_staging_log_interrupt_releases_lock_without_launch(self):
        for error in (KeyboardInterrupt(), SystemExit(2)):
            with self.subTest(error=type(error).__name__), tempfile.TemporaryDirectory() as directory:
                run = Path(directory)
                lab.write_json(run / 'state.json', {'profile': 'cinnamon-x11'})
                with patch.object(lab, 'ssh', return_value='') as ssh, patch.object(lab, 'put'), \
                        patch.object(lab, 'get', side_effect=error) as get, \
                        patch.object(lab, 'reconcile_staging', return_value={'finished': True, 'state': 'staging-finished'}), \
                    patch.object(lab, 'stage_install_helper', side_effect=lab.LabError('staging failure')):
                    with self.assertRaises(type(error)) as caught:
                        lab.install_package(run, 'synthetic.deb')
                    self.assertIs(caught.exception, error)
                    self.assertEqual(get.call_args.kwargs['timeout'], 10)
                    scripts = [call.args[1] for call in ssh.call_args_list]
                    releases = [script for script in scripts if 'rmdir' in script]
                    self.assertEqual(len(releases), 1)
                    self.assertIn('if [ "$(cat ~/putgd-lab/install-active/job)" = install-', releases[0])
                    self.assertFalse(any(' launch-supervisor "$HOME/' in script for script in scripts))

    def test_install_staging_log_interrupt_survives_release_failure(self):
        for interrupt in (KeyboardInterrupt(), SystemExit(2)):
            for failure in (lab.LabError('ssh failed'), OSError('offline'), lab.subprocess.TimeoutExpired('ssh', 60)):
                with self.subTest(interrupt=type(interrupt).__name__, failure=type(failure).__name__), \
                        tempfile.TemporaryDirectory() as directory:
                    run = Path(directory)
                    lab.write_json(run / 'state.json', {'profile': 'cinnamon-x11'})
                    scripts, warning = [], io.StringIO()
                    def ssh(_run, script):
                        scripts.append(script)
                        if 'rmdir' in script:
                            raise failure
                        return ''
                    with patch.object(lab, 'ssh', side_effect=ssh), patch.object(lab, 'put'), \
                            patch.object(lab, 'get', side_effect=interrupt), \
                            patch.object(lab, 'reconcile_staging', return_value={'finished': True, 'state': 'staging-finished'}), \
                    patch.object(lab, 'stage_install_helper', side_effect=lab.LabError('staging failure')), \
                            contextlib.redirect_stderr(warning):
                        with self.assertRaises(type(interrupt)) as caught:
                            lab.install_package(run, 'synthetic.deb')
                    self.assertIs(caught.exception, interrupt)
                    self.assertEqual(sum('rmdir' in script for script in scripts), 1)
                    self.assertFalse(any(' launch-supervisor "$HOME/' in script for script in scripts))
                    self.assertIn('unknown install-active state', warning.getvalue())

    def test_failed_staging_holds_lock_on_live_or_unreachable_guest(self):
        for transport in (lab.subprocess.TimeoutExpired('ssh', 60), KeyboardInterrupt(), SystemExit(2)):
            for outcome in ({'finished': False, 'state': 'staging-running'}, OSError('offline')):
                with self.subTest(transport=type(transport).__name__, outcome=outcome), tempfile.TemporaryDirectory() as directory:
                    run = Path(directory)
                    lab.write_json(run / 'state.json', {'profile': 'cinnamon-x11'})
                    warning = io.StringIO()
                    with patch.object(lab, 'ssh', return_value='') as ssh, patch.object(lab, 'put'), \
                            patch.object(lab, 'get'), patch.object(lab, 'stage_install_helper', side_effect=transport), \
                            patch.object(lab, 'reconcile_staging', **({'side_effect': outcome} if isinstance(outcome, Exception)
                                else {'return_value': outcome})), contextlib.redirect_stderr(warning):
                        with self.assertRaises(type(transport) if isinstance(transport, (KeyboardInterrupt, SystemExit)) else lab.LabError) as caught:
                            lab.install_package(run, 'synthetic.deb')
                        if isinstance(transport, (KeyboardInterrupt, SystemExit)):
                            self.assertIs(caught.exception, transport)
                            self.assertIn('unknown install-active state', warning.getvalue())
                        else:
                            self.assertIn('recover-staging', str(caught.exception))
                        self.assertFalse(any('rmdir' in c.args[1] for c in ssh.call_args_list))
                        self.assertTrue(next(run.glob('install-*/staging-failed.json')).is_file())

    def test_second_interrupt_and_unexpected_failure_during_staging_reconciliation_print_recovery(self):
        for error in (KeyboardInterrupt(), SystemExit(2), RuntimeError('unexpected failure')):
            with self.subTest(error=type(error).__name__), tempfile.TemporaryDirectory() as directory:
                run = Path(directory)
                lab.write_json(run / 'state.json', {'profile': 'cinnamon-x11'})
                warning = io.StringIO()
                with patch.object(lab, 'ssh', return_value='') as ssh, patch.object(lab, 'put'), \
                        patch.object(lab, 'get'), patch.object(lab, 'stage_install_helper', side_effect=KeyboardInterrupt()), \
                        patch.object(lab, 'reconcile_staging', side_effect=error), contextlib.redirect_stderr(warning):
                    with self.assertRaises(type(error) if isinstance(error, (KeyboardInterrupt, SystemExit)) else KeyboardInterrupt):
                        lab.install_package(run, 'synthetic.deb')
                job = next(run.glob('install-*')).name
                self.assertIn('unknown install-active state', warning.getvalue())
                self.assertIn(f'recover-staging --run {run} --job {job}', warning.getvalue())
                self.assertFalse(any('rmdir' in c.args[1] for c in ssh.call_args_list))

    def test_staging_lifetime_fence_finished_wait_exit_and_stopped(self):
        for finish in ('already-finished', 'exits', 'stopped', 'adopted'):
            with self.subTest(finish=finish), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                script = ('echo $$ > child.pid; touch ready; '
                          'while [ ! -e finish ]; do sleep .02; done')
                if finish == 'adopted':
                    script = 'sh -c ' + lab.shlex.quote(script) + ' & exit 0'
                process = lab.subprocess.Popen([sys.executable, str(lab.HERE / 'package_staging.py'),
                    'run', directory, script], start_new_session=True)
                child = None
                try:
                    deadline = time.monotonic() + 3
                    while not (root / 'ready').exists():
                        self.assertIsNone(process.poll())
                        self.assertLess(time.monotonic(), deadline)
                        time.sleep(.01)
                    child = int((root / 'child.pid').read_text())
                    identity = lab.read_json(root / 'staging-process.json')
                    self.assertEqual(identity['pid'], process.pid)
                    self.assertGreater(identity['startTicks'], 0)
                    # Execute the real reconciliation implementation in its own
                    # cwd without changing the test runner's process directory.
                    def reconcile(wait):
                        result = lab.subprocess.run([sys.executable, str(lab.HERE / 'package_staging.py'),
                            'reconcile', directory, str(wait)], capture_output=True, text=True, timeout=4, check=True)
                        return json.loads(result.stdout)
                    pending = reconcile(0)
                    self.assertFalse(pending['finished'])
                    self.assertEqual(pending['state'], 'staging-running')
                    self.assertTrue(pending['exactProcessAlive'])
                    self.assertEqual(pending['process'], identity)
                    if finish == 'already-finished':
                        (root / 'finish').touch()
                        process.wait(timeout=3)
                        report = reconcile(0)
                    else:
                        def release():
                            time.sleep(.1)
                            if finish == 'stopped':
                                os.kill(child, signal.SIGTERM)  # exact child started above
                            else:
                                (root / 'finish').touch()
                        worker = threading.Thread(target=release)
                        worker.start()
                        try:
                            report = reconcile(2)
                        finally:
                            worker.join(timeout=3)
                    self.assertTrue(report['finished'])
                    self.assertEqual(report['state'], 'staging-finished')
                    self.assertEqual(report['pid'], process.pid)
                    self.assertEqual(report['startTicks'], identity['startTicks'])
                    self.assertEqual(report['exitCode'] == 0, finish != 'stopped')
                finally:
                    (root / 'finish').touch()
                    process.wait(timeout=3)

    def test_retry_refused_while_real_staging_process_is_alive(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            job = 'install-' + 'b' * 32
            remote = root / 'putgd-lab' / job
            remote.mkdir(parents=True)
            active = root / 'putgd-lab/install-active'
            active.mkdir()
            (active / 'job').write_text(job)
            lab.write_json(root / 'state.json', {'profile': 'cinnamon-x11'})
            process = lab.subprocess.Popen([sys.executable, str(lab.HERE / 'package_staging.py'),
                'run', str(remote), 'touch ready; while [ ! -e finish ]; do sleep .02; done'])
            try:
                deadline = time.monotonic() + 3
                while not (remote / 'ready').exists():
                    self.assertIsNone(process.poll())
                    self.assertLess(time.monotonic(), deadline)
                    time.sleep(.01)
                def ssh(_run, script):
                    return lab.subprocess.run(['sh', '-s'], input=script, text=True, capture_output=True,
                        env={**os.environ, 'HOME': str(root)}, timeout=3, check=True).stdout
                with patch.object(lab, 'ssh', side_effect=ssh), patch.object(lab, 'put') as put:
                    with self.assertRaisesRegex(lab.LabError, 'retry refused.*staging'):
                        lab.install_package(root, 'synthetic.deb')
                    put.assert_not_called()
                self.assertIsNone(process.poll())
                self.assertEqual((active / 'job').read_text(), job)
                self.assertFalse(list(root.glob('install-*')))
            finally:
                (remote / 'finish').touch()
                process.wait(timeout=3)

    def test_staging_fence_cancels_delayed_launch_and_refuses_missing_terminal_receipt(self):
        for marker in (False, True):
            with self.subTest(marker=marker), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                if marker:
                    lab.write_json(root / 'staging-process.json', {'pid': 123, 'startTicks': 456})
                report = lab.subprocess.run([sys.executable, str(lab.HERE / 'package_staging.py'),
                    'reconcile', directory, '0'], capture_output=True, text=True, timeout=3, check=True)
                self.assertEqual(json.loads(report.stdout)['finished'], not marker)
                delayed = lab.subprocess.run([sys.executable, str(lab.HERE / 'package_staging.py'),
                    'run', directory, 'touch must-not-run'], timeout=3)
                self.assertEqual(delayed.returncode, 125)
                self.assertFalse((root / 'must-not-run').exists())

    def test_recover_staging_only_releases_confirmed_failed_staging(self):
        with tempfile.TemporaryDirectory() as directory:
            run = Path(directory)
            job = 'install-' + 'b' * 32
            (run / job).mkdir()
            lab.write_json(run / job / 'staging-failed.json', {'error': 'TimeoutExpired'})
            for finished in (False, True):
                with patch.object(lab, 'verify_owned_guest'), patch.object(lab, 'ssh', return_value='') as ssh, patch.object(lab, 'reconcile_staging',
                        return_value={'finished': finished, 'state': 'staging-finished' if finished else 'staging-running'}):
                    if finished:
                        self.assertTrue(lab.recover_staging(run, job)['finished'])
                    else:
                        with self.assertRaisesRegex(lab.LabError, 'remains held.*blocks retries'):
                            lab.recover_staging(run, job)
                    self.assertEqual(any('rmdir' in c.args[1] for c in ssh.call_args_list), finished)
                    for c in ssh.call_args_list:
                        self.assertIn(f'if [ -e ~/putgd-lab/{job}/progress.json ]', c.args[1])
                        self.assertIn('install-active/job', c.args[1])
                        self.assertNotIn('&&', c.args[1])  # set -e ignores a failed && left side.

    def test_abandoned_recovery_requires_free_flock_and_dead_or_reused_identity(self):
        # A real live process proves that PID reuse checks never signal it.
        process = lab.subprocess.Popen([sys.executable, '-c', 'import sys; sys.stdin.read()'],
                                       stdin=lab.subprocess.PIPE)
        self.addCleanup(lambda: process.wait(timeout=3))
        self.addCleanup(process.stdin.close)
        ticks = int(Path(f'/proc/{process.pid}/stat').read_text().rsplit(')', 1)[1].split()[19])
        dead = json.loads(lab.subprocess.run([sys.executable, '-c',
            'import json, os; from pathlib import Path; '
            'print(json.dumps({"pid": os.getpid(), "startTicks": '
            'int(Path("/proc/self/stat").read_text().rsplit(")", 1)[1].split()[19]), "sessionId": os.getpid()}))'],
            capture_output=True, text=True, check=True, timeout=3).stdout)
        original_reconcile = lab.reconcile_staging
        for case in ('flock-held', 'alive', 'terminal-but-alive', 'missing-identity', 'invalid-identity', 'unknown', 'dead', 'reused'):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as directory:
                run = Path(directory)
                job = 'install-' + 'b' * 32
                remote = run / 'putgd-lab' / job
                remote.mkdir(parents=True)
                active = run / 'putgd-lab/install-active'
                active.mkdir()
                (active / 'job').write_text(job)
                (run / job).mkdir()
                lab.write_json(run / job / 'staging-failed.json', {'error': 'TimeoutExpired'})
                identity = (dead if case in ('dead', 'flock-held') else
                            {'pid': process.pid, 'sessionId': process.pid, 'startTicks': ticks + (case == 'reused')})
                if case == 'invalid-identity':
                    identity = {'pid': -1, 'startTicks': ticks}
                if case == 'unknown':
                    identity = {'pid': process.pid, 'startTicks': 'unreadable'}
                if case == 'terminal-but-alive':
                    lab.write_json(remote / 'staging-result.json', {**identity, 'finished': True, 'exitCode': 0})
                if case != 'missing-identity':
                    lab.write_json(remote / 'staging-process.json', identity)
                def ssh(_run, script, **kwargs):
                    return lab.subprocess.run(['sh', '-s'], input=script, text=True, capture_output=True,
                        env={**os.environ, 'HOME': directory}, timeout=3, check=True).stdout
                with open(remote / 'staging.lock', 'a') as fence:
                    if case == 'flock-held':
                        fcntl.flock(fence, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    with patch.object(lab, 'verify_owned_guest'), patch.object(lab, 'ssh', side_effect=ssh), \
                            patch.object(lab, 'reconcile_staging', side_effect=lambda run, remote, **kw:
                                original_reconcile(run, remote, wait=0, **kw)):
                        if case in ('dead', 'reused'):
                            report = lab.recover_staging(run, job, abandoned=True)
                            self.assertEqual(report['state'], 'staging-abandoned')
                            self.assertFalse(report['exactProcessAlive'])
                            self.assertEqual(report['process'], identity)
                            self.assertFalse(active.exists())
                            self.assertEqual(lab.read_json(run / job / 'staging-recovery.json'), report)
                            delayed = lab.subprocess.run([sys.executable, str(lab.HERE / 'package_staging.py'),
                                'run', str(remote), 'touch must-not-run'], timeout=3)
                            self.assertEqual(delayed.returncode, 125)
                            self.assertFalse((remote / 'must-not-run').exists())
                        else:
                            with self.assertRaisesRegex(lab.LabError, 'remains held.*blocks retries'):
                                lab.recover_staging(run, job, abandoned=True)
                            self.assertEqual((active / 'job').read_text(), job)
                self.assertIsNone(process.poll())

    def test_abandoned_recovery_keeps_controller_ownership_and_supervisor_guards(self):
        for case in ('unowned', 'replacement-token', 'progress.json', 'result.json'):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as directory:
                run = Path(directory)
                job = 'install-' + 'b' * 32
                remote = run / 'putgd-lab' / job
                remote.mkdir(parents=True)
                active = run / 'putgd-lab/install-active'
                active.mkdir()
                held = 'install-' + 'c' * 32 if case == 'replacement-token' else job
                (active / 'job').write_text(held)
                (run / job).mkdir()
                lab.write_json(run / job / 'staging-failed.json', {'error': 'TimeoutExpired'})
                if case in ('progress.json', 'result.json'):
                    lab.write_json(remote / case, {'synthetic': True})
                def ssh(_run, script, **kwargs):
                    result = lab.subprocess.run(['sh', '-s'], input=script, text=True, capture_output=True,
                        env={**os.environ, 'HOME': directory}, timeout=3)
                    if result.returncode:
                        raise lab.LabError('guest guard refused recovery')
                    return result.stdout
                with patch.object(lab, 'verify_owned_guest', side_effect=(
                        lab.LabError('unowned overlay') if case == 'unowned' else None)), \
                        patch.object(lab, 'ssh', side_effect=ssh), patch.object(lab, 'reconcile_staging') as reconcile:
                    with self.assertRaises(lab.LabError):
                        lab.recover_staging(run, job, abandoned=True)
                    reconcile.assert_not_called()
                self.assertEqual((active / 'job').read_text(), held)

    def test_staging_reconciliation_without_pidfd_open_uses_proc_and_keeps_lock(self):
        identity = {'pid': os.getpid(), 'sessionId': os.getpid(), 'startTicks': int(
            Path('/proc/self/stat').read_text().rsplit(')', 1)[1].split()[19])}
        for reused in (False, True):
            with self.subTest(reused=reused), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                lab.write_json(root / 'staging-process.json', {
                    **identity, 'startTicks': identity['startTicks'] + reused})
                # Simulate the actual older guest API in a separate interpreter.
                script = ('import os, runpy; '
                          'os.__dict__.pop("pidfd_open", None); '
                          'runpy.run_path(' + repr(str(lab.HERE / 'package_staging.py')) + ', run_name="__main__")')
                for held in (False, True):
                    with self.subTest(held=held), open(root / 'staging.lock', 'a') as fence:
                        if held:
                            fcntl.flock(fence, fcntl.LOCK_EX | fcntl.LOCK_NB)
                        result = lab.subprocess.run([sys.executable, '-c', script, 'reconcile', directory, '0'],
                            capture_output=True, text=True, check=True, timeout=3)
                        report = json.loads(result.stdout)
                        self.assertFalse(report['finished'])
                        self.assertEqual(report['exactProcessAlive'], not reused)
                        self.assertEqual(report['state'], 'staging-running' if held else 'staging-outcome-unknown')
                        self.assertEqual(result.stderr, '')
                        if held:
                            with open(root / 'staging.lock', 'a') as probe:
                                with self.assertRaises(BlockingIOError):
                                    fcntl.flock(probe, fcntl.LOCK_EX | fcntl.LOCK_NB)

    def test_abandoned_recovery_refuses_unreadable_proc_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            lab.write_json(root / 'staging-process.json', {'pid': os.getpid(), 'sessionId': os.getpid(), 'startTicks': 1})
            # Only /proc reads fail; reading the saved job identity still works.
            script = ("import os, runpy; from pathlib import Path; from unittest.mock import patch; "
                      "os.__dict__.pop('pidfd_open', None); original = Path.read_text; "
                      "read = lambda p, *a, **kw: (_ for _ in ()).throw(PermissionError()) "
                      "if str(p).startswith('/proc/') else original(p, *a, **kw); "
                      "patch.object(Path, 'read_text', read).start(); "
                      "runpy.run_path(" + repr(str(lab.HERE / 'package_staging.py')) + ", run_name='__main__')")
            result = lab.subprocess.run([sys.executable, '-c', script, 'reconcile', directory, '0', '--abandoned'],
                capture_output=True, text=True, check=True, timeout=3)
            report = json.loads(result.stdout)
            self.assertFalse(report['finished'])
            self.assertIsNone(report['exactProcessAlive'])
            self.assertEqual(result.stderr, '')

    def test_recover_staging_cli_passes_abandoned_option(self):
        with patch('sys.argv', ['lab.py', 'recover-staging', '--run', '/tmp/run',
                               '--job', 'install-' + 'b' * 32, '--abandoned']), \
                patch.object(lab, 'recover_staging', return_value={'finished': True}) as recover, \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(lab.main(), 0)
        recover.assert_called_once_with('/tmp/run', 'install-' + 'b' * 32, abandoned=True)

    def test_concurrent_release_and_recovery_cannot_remove_replacement_lock(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            run = root / 'controller'
            run.mkdir()
            lab.write_json(run / 'state.json', {'profile': 'cinnamon-x11'})
            with patch.object(lab, 'ssh', return_value='') as ssh, patch.object(lab, 'put', side_effect=lab.LabError('upload')), \
                    patch.object(lab, 'get'):
                with self.assertRaisesRegex(lab.LabError, 'upload'):
                    lab.install_package(run, 'synthetic.deb')
            acquire = ssh.call_args_list[0].args[1]
            release = next(c.args[1] for c in ssh.call_args_list if 'rmdir' in c.args[1])
            job = next(run.glob('install-*')).name
            replacement = 'install-' + 'c' * 32
            acquire = acquire.replace(job, replacement)
            with patch.object(lab, 'verify_owned_guest'), patch.object(lab, 'ssh', return_value='') as ssh, \
                    patch.object(lab, 'reconcile_staging', return_value={'finished': True}):
                lab.recover_staging(run, job)
            recover = next(c.args[1] for c in ssh.call_args_list if 'rmdir' in c.args[1])
            binaries = root / 'bin'
            binaries.mkdir()
            wrapper = binaries / 'rm'
            wrapper.write_text('#!/bin/sh\nif [ "${BLOCK_RELEASE:-}" = 1 ]; then\n'
                'touch "$GATE/blocked"\nwhile [ ! -e "$GATE/release" ]; do sleep .01; done\nfi\n'
                'exec ' + lab.shlex.quote(shutil.which('rm')) + ' "$@"\n')
            wrapper.chmod(0o700)
            for number, (first, second) in enumerate(((release, recover), (recover, release), (recover, recover))):
                with self.subTest(number=number):
                    home = root / str(number)
                    active = home / 'putgd-lab/install-active'
                    active.mkdir(parents=True)
                    (active / 'job').write_text(job)
                    (home / 'putgd-lab' / job).mkdir()
                    env = {**os.environ, 'HOME': str(home), 'GATE': str(home),
                           'PATH': str(binaries) + ':' + os.environ['PATH']}
                    processes = []
                    def start(script, **extra):
                        process = lab.subprocess.Popen(['sh', '-s'], stdin=lab.subprocess.PIPE,
                            stdout=lab.subprocess.PIPE, stderr=lab.subprocess.PIPE, text=True, env={**env, **extra})
                        process.stdin.write(script)
                        process.stdin.close()
                        processes.append(process)
                        return process
                    def ready(name):
                        deadline = time.monotonic() + 3
                        while not (home / name).exists():
                            self.assertLess(time.monotonic(), deadline)
                            time.sleep(.01)
                    try:
                        a = start(first, BLOCK_RELEASE='1')
                        ready('blocked')  # A passed the old token check, before rm.
                        b = start('touch "$GATE/b-ready"\n' + second)
                        c = start('touch "$GATE/c-ready"\n' + acquire)
                        ready('b-ready')
                        ready('c-ready')
                        # Without the shared guard, B releases A's old token and
                        # C acquires a new job while A is paused before its rm.
                        with self.assertRaises(lab.subprocess.TimeoutExpired):
                            c.wait(timeout=.2)
                    finally:
                        (home / 'release').touch()
                        for process in processes:
                            process.wait(timeout=6)
                            process.stdout.close()
                            process.stderr.close()
                    self.assertEqual(a.returncode, 0)
                    self.assertEqual(c.returncode, 0)
                    self.assertEqual((active / 'job').read_text(), replacement)

    def test_install_refused_retry_reports_held_job_without_orphan_evidence(self):
        self.check_install_refused_retry('aborted', 'result.json')

    def test_install_refused_retry_with_only_progress_reports_running(self):
        self.check_install_refused_retry('running', 'progress.json')

    def test_install_refused_retry_without_reports_reports_staging(self):
        self.check_install_refused_retry('staging', None)

    def check_install_refused_retry(self, expected_state, report_name):
        with tempfile.TemporaryDirectory() as directory:
            run = Path(directory)
            lab.write_json(run / 'state.json', {'profile': 'cinnamon-x11'})
            job = 'install-' + 'b' * 32
            home = run / 'guest'
            active = home / 'putgd-lab/install-active'
            active.mkdir(parents=True)
            (active / 'job').write_text(job)
            result = home / 'putgd-lab' / job / 'result.json'
            result.parent.mkdir()
            report = {'aborted': True, 'ownedProcesses': [{'pid': pid, 'startTicks': 42} for pid in range(10000)]}
            if report_name:
                lab.write_json(result.with_name(report_name), report)
            def ssh(_run, script):
                output = lab.subprocess.run(['sh', '-s'], input=script, text=True, capture_output=True,
                    env={**os.environ, 'HOME': str(home)}, timeout=5)
                self.assertEqual(output.returncode, 0, output.stderr)
                held = json.loads(output.stdout)
                self.assertEqual(held['heldJob'], job)
                self.assertEqual(held['state'], expected_state)
                self.assertEqual(held['resultPath'], str(result))
                self.assertRegex(held['startedAt'], r'^\d{4}-\d\d-\d\dT')
                self.assertNotIn('ownedProcesses', output.stdout)
                if report_name:
                    self.assertEqual(lab.read_json(result.with_name(report_name)), report)
                self.assertEqual(result.exists(), report_name == 'result.json')
                return output.stdout
            with patch.object(lab, 'ssh', side_effect=ssh), patch.object(lab, 'put') as put:
                with self.assertRaisesRegex(lab.LabError, f'retry refused.*install-b.*{expected_state}.*startedAt.*result.json') as caught:
                    lab.install_package(run, 'synthetic.deb')
                self.assertLess(len(str(caught.exception)), 600)
                put.assert_not_called()
            self.assertFalse(list(run.glob('install-*')))
            self.assertEqual((active / 'job').read_text(), job)

    def test_install_supervisor_missing_progress_fails_fast_and_keeps_lock(self):
        with tempfile.TemporaryDirectory() as directory:
            run = Path(directory)
            lab.write_json(run / 'state.json', {'profile': 'cinnamon-x11'})
            calls = []
            def ssh(_run, script, **kwargs):
                calls.append(script)
                return json.dumps({'startupPending': True}) if 'result.json; then cat' in script else ''
            with patch.object(lab, 'ssh', side_effect=ssh), patch.object(lab, 'put'), \
                    patch.object(lab, 'get', side_effect=lab.LabError('missing supervisor.log')), \
                    patch.object(lab.time, 'monotonic', side_effect=[0, 0, 16]), patch.object(lab.time, 'sleep') as sleep:
                with self.assertRaisesRegex(lab.LabError, 'supervisor did not start.*no progress.json.*retains install-active'):
                    lab.install_package(run, 'synthetic.deb')
                sleep.assert_not_called()
            self.assertFalse(any('rmdir' in c for c in calls))

    def test_install_transport_timeout_reconciles_same_job_without_restart(self):
        with tempfile.TemporaryDirectory() as directory:
            run = Path(directory)
            deb = run / 'synthetic.deb'
            deb.write_bytes(b'synthetic')
            lab.write_json(run / 'state.json', {'profile': 'cinnamon-x11'})
            polls = 0
            calls = []
            def ssh(_run, script, **kwargs):
                nonlocal polls
                calls.append(script)
                if 'result.json; then cat' in script:
                    polls += 1
                    if polls == 1:
                        raise lab.subprocess.TimeoutExpired('ssh', 10)
                    return json.dumps({'ok': True, 'timedOut': True})
                return ''
            with patch.object(lab, 'ssh', side_effect=ssh), patch.object(lab, 'put'), \
                    patch.object(lab, 'get'), patch.object(lab.time, 'sleep'), \
                    patch.object(lab, 'verify_installed_binaries', return_value={}):
                lab.install_package(run, deb)
            self.assertEqual(polls, 2)
            self.assertEqual(sum(' launch-supervisor "$HOME/' in call for call in calls), 1)

    def test_install_lost_launch_reply_reconciles_without_starting_twice(self):
        with tempfile.TemporaryDirectory() as directory:
            run = Path(directory)
            deb = run / 'synthetic.deb'
            deb.write_bytes(b'synthetic')
            lab.write_json(run / 'state.json', {'profile': 'cinnamon-x11'})
            calls = []
            def ssh(_run, script, **kwargs):
                calls.append(script)
                if ' launch-supervisor "$HOME/' in script:
                    raise lab.subprocess.TimeoutExpired('ssh', 60)
                return json.dumps({'ok': True, 'timedOut': False}) if 'result.json; then cat' in script else ''
            with patch.object(lab, 'ssh', side_effect=ssh), patch.object(lab, 'put'), \
                    patch.object(lab, 'get'), patch.object(lab, 'verify_installed_binaries', return_value={}):
                lab.install_package(run, deb)
            self.assertEqual(sum(' launch-supervisor "$HOME/' in c for c in calls), 1)
            self.assertTrue(any('rmdir' in c for c in calls))

    def test_install_profile_timeouts_and_abort_block_blind_retry(self):
        with tempfile.TemporaryDirectory() as directory:
            run = Path(directory)
            lab.write_json(run / 'state.json', {'profile': 'cinnamon-x11'})
            calls = []
            def ssh(_run, script, **kwargs):
                calls.append(script)
                return json.dumps({'ok': False, 'aborted': True, 'dpkgStatus': {'stdout': 'half-configured'}}) if 'result.json; then cat' in script else ''
            with patch.object(lab, 'load_profile', return_value={'install_timeout': 42, 'install_recovery_timeout': 21}), \
                    patch.object(lab, 'ssh', side_effect=ssh), patch.object(lab, 'put'), patch.object(lab, 'get'), \
                    patch.object(lab, 'verify_installed_binaries') as verify:
                with self.assertRaisesRegex(lab.LabError, 'dpkg state.*blocks retries'):
                    lab.install_package(run, 'synthetic.deb')
                verify.assert_not_called()
            self.assertTrue(any('42 21' in call for call in calls))
            self.assertFalse(any('rmdir' in call for call in calls))
            self.assertEqual(sum(' launch-supervisor "$HOME/' in call for call in calls), 1)

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

    def test_appimage_profiles_put_shims_on_path_and_package_profiles_do_not(self):
        for profile, expected in (('aurora-kde-wayland', True), ('kde-wayland', False)):
            scripts = []
            with patch.object(lab, 'state', return_value={'guest_user': 'lab', 'profile': profile}), \
                 patch.object(lab, 'ssh', side_effect=lambda _run, script, *a, **k: scripts.append(script) or ''):
                lab.guest_session('run', 'putgd-cli capabilities {}\n')
            self.assertEqual('putgd-lab/bin:$PATH' in scripts[0], expected, profile)

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

    def test_multi_head_launch_adds_one_output_tablet_and_vnc_listener_per_head(self):
        profile = lab.load_profile('cinnamon-x11')
        single = lab.qemu_command(profile, Path('/tmp/run'), Path('/tmp/run/disk.qcow2'), 22001, 60, 'kvm')
        self.assertEqual(single, lab.qemu_command(profile, Path('/tmp/run'), Path('/tmp/run/disk.qcow2'),
                                                  22001, 60, 'kvm', None, 1))
        self.assertEqual(single.count('-vnc'), 1)
        self.assertIn('127.0.0.1:60', single)
        command = lab.qemu_command(profile, Path('/tmp/run'), Path('/tmp/run/disk.qcow2'), 22001, 60, 'kvm', None, 4)
        self.assertIn('virtio-vga,id=video0,max_outputs=4', command)
        listeners = [command[index + 1] for index, value in enumerate(command) if value == '-vnc']
        self.assertEqual(listeners, [f'127.0.0.1:{60 + head},id=head{head},display=video0,head={head}' for head in range(4)])
        tablets = [value for value in command if value.startswith('usb-tablet')]
        # The unbound tablet keeps QMP input without a head working.
        self.assertEqual(tablets, ['usb-tablet'] + [
            f'usb-tablet,id=tablet{head},display=video0,head={head},serial=putgd-head{head}' for head in range(4)])
        self.assertEqual([port['port'] for port in lab.head_ports(60, 4)], [5960, 5961, 5962, 5963])

    def test_head_plan_validates_counts_layouts_and_video_devices(self):
        profile = lab.load_profile('kde-wayland')
        self.assertEqual(lab.head_plan(profile), (1, None))
        self.assertEqual(lab.head_plan(profile, 3), (3, None))
        count, layout = lab.head_plan(profile, layout='2560x1440+0+0,1920x1080-1920+0')
        self.assertEqual(count, 2)
        self.assertEqual(layout[1], {'width': 1920, 'height': 1080, 'x': -1920, 'y': 0})
        self.assertEqual(lab.format_layout(layout), '2560x1440+0+0,1920x1080-1920+0')
        self.assertEqual(lab.head_plan({**profile, 'head_layout': ['1280x800+0+0', '1280x800+1380+0']}), (2, [
            {'width': 1280, 'height': 800, 'x': 0, 'y': 0}, {'width': 1280, 'height': 800, 'x': 1380, 'y': 0}]))
        for heads, spec, message in ((5, None, 'Heads must be 1 to 4'), (0, None, 'Heads must be 1 to 4'),
                                     (3, '1280x800+0+0,1280x800+1280+0', 'one entry per head'),
                                     (None, '1280x800+0+0,1280x800+640+0', 'overlap'),
                                     (None, '1280x800+0+0,100x100+1280+0', 'between'),
                                     (None, '1280x800@0,0', 'look like')):
            with self.subTest(heads=heads, spec=spec):
                with self.assertRaisesRegex(lab.LabError, message):
                    lab.head_plan(profile, heads, spec)
        with self.assertRaisesRegex(lab.LabError, 'virtio-gpu'):
            lab.head_plan({**profile, 'video': 'qxl-vga'}, 2)
        self.assertEqual(lab.head_plan({**profile, 'video': 'qxl-vga'}, 1), (1, None))

    def test_head_size_request_uses_set_desktop_size_and_checks_qemu_status(self):
        def serve(status):
            server = socket.socket()
            server.bind(('127.0.0.1', 0))
            server.listen(1)
            received = []
            def peer():
                connection, _ = server.accept()
                with connection:
                    def read(count):
                        data = b''
                        while len(data) < count:
                            data += connection.recv(count - len(data))
                        return data
                    connection.sendall(b'RFB 003.008\n')
                    read(12)
                    connection.sendall(b'\x01\x01')
                    read(1)
                    connection.sendall(b'\0\0\0\0')
                    read(1)
                    connection.sendall(lab.struct.pack('>HH16xI', 640, 480, 4) + b'QEMU')
                    read(20)  # SetPixelFormat
                    read(8)   # raw-only SetEncodings from the shared RFB client
                    received.append(read(12))
                    received.append(read(24))
                    # An unrelated server-initiated resize first, then the answer.
                    for reason, answer in ((0, 0), (1, status)):
                        connection.sendall(lab.struct.pack('>BxHHHHHi', 0, 1, reason, answer, 1920, 1080, -308) +
                                           b'\x01\0\0\0' + bytes(16))
            thread = threading.Thread(target=peer)
            thread.start()
            return server, thread, received
        server, thread, received = serve(4)
        self.assertEqual(lab.request_head_size(server.getsockname()[1], 1920, 1080, timeout=3)['status'], 4)
        thread.join(3)
        server.close()
        self.assertEqual(received[0], lab.struct.pack('>BxHii', 2, 2, 0, -308))
        self.assertEqual(received[1][:8], lab.struct.pack('>BxHHBx', 251, 1920, 1080, 1))
        self.assertEqual(lab.struct.unpack('>IHHHHI', received[1][8:]), (0, 0, 0, 1920, 1080, 0))
        server, thread, _ = serve(3)
        with self.assertRaisesRegex(lab.LabError, 'refused head size'):
            lab.enable_heads(server.getsockname()[1] - 5900, 1)
        thread.join(3)
        server.close()

    def test_multi_head_launch_records_every_head_port_in_run_state(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            golden = root / 'golden.qcow2'
            golden.write_bytes(b'sealed test identity')
            trusted = root / 'known_hosts'
            trusted.write_text('[127.0.0.1]:22260 ssh-ed25519 AAAA\n')
            run = root / 'runs' / 'cinnamon-heads'
            args = SimpleNamespace(profile='cinnamon-x11', golden=str(golden), run=str(run), acceleration='kvm',
                                   ssh_port=22260, vnc_display=60, known_hosts=str(trusted),
                                   bootstrap_key=str(root / 'identity'))
            launched = []
            def fake_command(arguments, **kwargs):
                if arguments[0] == 'qemu-system-x86_64':
                    launched.append(arguments)
                    (run / 'qemu.pid').write_text('4242\n')
                    return SimpleNamespace(returncode=0, stderr='')
                if arguments[0] == 'qemu-img':
                    Path(arguments[-1]).write_bytes(b'overlay')
                elif arguments[0] == 'ssh-keygen':
                    Path(arguments[-1]).write_bytes(b'private')
                return SimpleNamespace(returncode=0, stderr='', stdout='')
            identity = golden.stat()
            layout = lab.parse_layout('1920x1080+0+0,1280x1024-1280+0')
            with patch.object(lab, 'command', side_effect=fake_command):
                info = lab._launch_machine(args, lab.load_profile('cinnamon-x11'), golden, run, 'f' * 64,
                                           (identity.st_dev, identity.st_ino, identity.st_size, identity.st_mtime_ns),
                                           None, 2, layout)
            recorded = json.loads((run / 'state.json').read_text())
            self.assertEqual(recorded['heads'], info['heads'])
            self.assertEqual([head['display'] for head in recorded['heads']], ['127.0.0.1:5960', '127.0.0.1:5961'])
            self.assertEqual(recorded['head_layout'], '1920x1080+0+0,1280x1024-1280+0')
            self.assertIn('virtio-vga,id=video0,max_outputs=2', launched[0])

    def test_outputs_must_be_real_heads_in_the_requested_arrangement(self):
        query = '''Screen 0: minimum 320 x 200, current 3200 x 1080, maximum 8192 x 8192
Virtual-1 connected primary 1920x1080+1280+0 (normal left inverted right x axis y axis) 487mm x 274mm
   1920x1080     75.00*+  60.00
   1280x1024     60.02
Virtual-2 connected 1280x1024+0+0 (normal left inverted right x axis y axis) 325mm x 260mm
   1280x1024     75.00*+
Virtual-3 disconnected (normal left inverted right x axis y axis)
'''
        monitors = '''Monitors: 2
 0: +*Virtual-1 1920/487x1080/274+1280+0  Virtual-1
 1: +Virtual-2 1280/325x1024/260+0+0  Virtual-2
'''
        observed = lab.parse_xrandr(query, monitors)
        self.assertEqual([output['name'] for output in observed['outputs']], ['Virtual-1', 'Virtual-2', 'Virtual-3'])
        self.assertEqual(observed['outputs'][0]['modes'], ['1920x1080', '1280x1024'])
        layout = lab.parse_layout('1920x1080+0+0,1280x1024-1280+0')
        # X11 places the desktop at 0,0: the arrangement, sizes and primary head are what must match.
        self.assertTrue(lab.verify_outputs(observed, 2, layout)['ok'])
        self.assertFalse(lab.verify_outputs(observed, 2, lab.parse_layout('1920x1080+0+0,1280x1024+1920+0'))['ok'])
        self.assertFalse(lab.verify_outputs(observed, 3)['ok'])
        demoted = lab.parse_xrandr(query.replace(' primary', ''), monitors)
        self.assertIn('head 0 is not the primary output', lab.verify_outputs(demoted, 2, layout)['errors'])
        # One output split into two RandR monitors (xrandr --setmonitor) is not two heads.
        split_query = query.replace('Virtual-2 connected 1280x1024+0+0', 'Virtual-2 connected')
        split = lab.parse_xrandr(split_query, '''Monitors: 2
 0: +*left 960/243x1080/274+0+0  Virtual-1
 1: +right 960/244x1080/274+960+0  none
''')
        result = lab.verify_outputs(split, 2)
        self.assertFalse(result['ok'])
        self.assertTrue(any('RandR monitors' in error or 'enabled' in error for error in result['errors']))

    def test_kscreen_outputs_follow_connector_order_and_priority(self):
        document = {'outputs': [
            {'id': 2, 'name': 'Virtual-2', 'connected': True, 'enabled': True, 'priority': 2,
             'pos': {'x': -1280, 'y': 0}, 'size': {'width': 1280, 'height': 1024},
             'modes': [{'id': '7', 'name': '1280x1024@75'}]},
            {'id': 1, 'name': 'Virtual-1', 'connected': True, 'enabled': True, 'priority': 1,
             'pos': {'x': 0, 'y': 0}, 'size': {'width': 1920, 'height': 1080},
             'modes': [{'id': '3', 'name': '1920x1080@75'}, {'id': '9', 'name': '1920x1080@50'}]}]}
        observed = lab.parse_kscreen(document)
        self.assertEqual([output['name'] for output in observed['outputs']], ['Virtual-1', 'Virtual-2'])
        self.assertEqual(observed['outputs'][1]['modes'], {'1280x1024': '7'})
        self.assertEqual(observed['outputs'][0]['modes'], {'1920x1080': '3'})
        result = lab.verify_outputs(observed, 2, lab.parse_layout('1920x1080+0+0,1280x1024-1280+0'))
        self.assertTrue(result['ok'], result['errors'])
        self.assertEqual(result['outputs'][1]['x'], -1280)

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

    def test_scaled_fixture_origin_comes_from_quadrant_boundaries(self):
        def render(width, height, placements, edge=True):
            # Area-sampled like a smooth scale, with the editor's 1 px image edge.
            pixels = bytearray(bytes([60, 60, 60]) * (width * height))
            for ox, oy, scale in placements:
                for y in range(int(oy) - 1, int(oy + 320 * scale) + 2):
                    for x in range(int(ox) - 1, int(ox + 640 * scale) + 2):
                        samples = []
                        for sx, sy in ((.25, .25), (.75, .25), (.25, .75), (.75, .75)):
                            u, v = (x + sx - ox) / scale, (y + sy - oy) / scale
                            if 0 <= u < 640 and 0 <= v < 320:
                                samples.append(lab.FIXTURE_RGB[(v >= 160) * 2 + (u >= 320)])
                        if not samples:
                            continue
                        colour = [sum(c[i] for c in samples) // len(samples) if len(set(samples)) > 1
                                  else samples[0][i] for i in range(3)]
                        if len(samples) < 4 or (edge and (abs(x - ox) < 1 or abs(y - oy) < 1 or
                                                         abs(x + 1 - ox - 640 * scale) < 1 or abs(y + 1 - oy - 320 * scale) < 1)):
                            colour = [(c + 128) // 2 for c in colour]
                        offset = (y * width + x) * 3
                        pixels[offset:offset + 3] = bytes(colour)
            return f'P6\n{width} {height}\n255\n'.encode() + bytes(pixels)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'editor.ppm'
            for ox, oy, scale in ((37.3, 50.6, .9), (20, 30, 1.0), (101.5, 12.25, .5)):
                with self.subTest(scale=scale):
                    path.write_bytes(render(700, 400, [(ox, oy, scale)]))
                    found = lab.locate_scaled_fixture_paint(path, scale)
                    self.assertIsNotNone(found)
                    self.assertAlmostEqual(found['x'], ox, delta=.6)
                    self.assertAlmostEqual(found['y'], oy, delta=.6)
                    self.assertEqual((found['width'], found['height']), (640 * scale, 320 * scale))
            # The exact locator cannot see an image whose outer pixels carry an edge.
            path.write_bytes(render(700, 400, [(20, 30, 1.0)]))
            self.assertIsNone(lab.locate_fixture_paint(path))
            # A complete top half above a single bottom row is not the fixture.
            width, height = 700, 400
            truncated = bytearray(render(width, height, [(20, 30, .9)]))
            header = len(f'P6\n{width} {height}\n255\n'.encode())
            for y in range(30 + 147, height):
                truncated[header + y * width * 3:header + (y + 1) * width * 3] = bytes([60, 60, 60]) * width
            path.write_bytes(bytes(truncated))
            self.assertIsNone(lab.locate_scaled_fixture_paint(path, .9))
            path.write_bytes(render(760, 400, [(5, 10, .5), (400, 60, .5)]))
            self.assertIsNone(lab.locate_scaled_fixture_paint(path, .5))
            self.assertIsNone(lab.locate_scaled_fixture_paint(path, .9))

    def test_window_frame_oracle_follows_observed_kwin_session_not_profile_name(self):
        # Physical Debian 13 Plasma 6.3 (profile kde-wayland-physical) captured
        # a 642x449 server-decorated frame with paint at (1,28). Native Qt
        # Wayland frameMargins were zero, so a name-gated oracle expected the
        # bare 640x420 client area and rejected correct frame-inclusive pixels.
        kde = lab.load_profile('kde-wayland')
        physical = dict(kde, name='kde-wayland-physical', guest_user='patrick')
        original_profile = lab.load_profile
        original_compare = lab.compare_ppm
        paint = {'x': 640, 'y': 321, 'width': 640, 'height': 320}
        colors = ([220, 30, 40], [20, 180, 70], [35, 70, 220], [235, 190, 20])
        for profile, desktop, margins, kwin_frame in (
                ('kde-wayland', 'KDE', {'left': 0, 'top': 0, 'right': 0, 'bottom': 0},
                 {'x': 640.0, 'y': 293.0, 'width': 640.0, 'height': 448.0}),
                ('kde-wayland-physical', 'KDE', {'left': 0, 'top': 0, 'right': 0, 'bottom': 0},
                 {'x': 639.0, 'y': 293.0, 'width': 642.0, 'height': 449.0}),
                ('gnome-wayland', 'ubuntu:GNOME', {'left': 11, 'top': 48, 'right': 11, 'bottom': 12}, None)):
            with self.subTest(profile=profile), tempfile.TemporaryDirectory() as directory:
                run = Path(directory)
                lab.write_json(run / 'state.json', {'schemaVersion': 1, 'profile': profile,
                    'acceleration': 'physical', 'guest_user': 'fixture', 'id': 'synthetic'})
                ready = {'session': 'wayland', 'qt_platform': 'wayland', 'desktop': desktop,
                    'qt_version': '6.8.2', 'decorationMargins': margins,
                    'display_layout': [{'x': 0, 'y': 0, 'width': 1920, 'height': 1080}],
                    'paint': {'x': 0, 'y': 0, 'width': 640, 'height': 320},
                    'frame': {'x': 0, 'y': 0, 'width': 640, 'height': 420}}
                frame = {key: round(value) for key, value in kwin_frame.items()} if kwin_frame else {
                    'x': paint['x'] - margins['left'], 'y': paint['y'] - margins['top'],
                    'width': 640 + margins['left'] + margins['right'],
                    'height': 420 + margins['top'] + margins['bottom']}
                # Decoration grey, white client area, fixture paint at its real offset.
                pixels = bytearray(bytes([200, 200, 200]) * frame['width'] * frame['height'])
                left, top = paint['x'] - frame['x'], paint['y'] - frame['y']
                for row in range(420):
                    offset = ((top + row) * frame['width'] + left) * 3
                    line = bytes([255, 255, 255]) * 640
                    if row < 320:
                        quadrants = colors[:2] if row < 160 else colors[2:]
                        line = bytes(quadrants[0]) * 320 + bytes(quadrants[1]) * 320
                    pixels[offset:offset + 640 * 3] = line
                capture = f'P6\n{frame["width"]} {frame["height"]}\n255\n'.encode() + pixels
                saved = b'synthetic saved window artifact'
                operation = {'id': 'window-gesture', 'state': 'completed', 'copyStatus': 'copied', 'items': [
                    {'local': {'status': 'saved', 'path': '/synthetic/window.png'},
                     'artifact': {'id': 'window-gesture', 'sha256': hashlib.sha256(saved).hexdigest(),
                                  'bytes': len(saved)}, 'uploadStatus': 'local_only'}]}
                recent_calls = 0
                scripts = []
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
                    return {}
                def fake_get(_run, remote, target):
                    target = Path(target)
                    if target.name == 'ready.json':
                        lab.write_json(target, ready)
                    elif remote == '/synthetic/window.png':
                        target.write_bytes(saved)
                    elif target.suffix == '.ppm':
                        target.write_bytes(capture)
                    else:
                        lab.write_json(target, {})
                def fake_session(_run, body):
                    scripts.append(body)
                    if 'cat ~/putgd-lab/window-oracle.json' not in body:
                        return ''
                    values = {'caption': 'put.gd independent synthetic paint / paste fixture', **kwin_frame}
                    return json.dumps({'type': 'a{sv}', 'data': [
                        {key: {'type': 's' if key == 'caption' else 'd', 'data': value}
                         for key, value in values.items()}]})
                def fake_screenshot(_run, name='witness', head=0):
                    path = run / f'{name}-head{head}.ppm'
                    path.write_bytes(b'synthetic witness')
                    return {'path': str(path), 'sha256': lab.hash_file(path)}
                def compare(path, expected):
                    if Path(path).name in ('artifact.ppm', 'paste.ppm'):
                        return original_compare(path, expected)
                    return {'ok': True}
                args = SimpleNamespace(run=str(run), scenario='window', clipboard_client='qt',
                    settings=None, history=False, timeout=3, keep=True)
                with contextlib.ExitStack() as stack:
                    for name, replacement in (
                            ('cli', fake_cli), ('get', fake_get), ('screenshot', fake_screenshot),
                            ('guest_session', fake_session), ('verify_running_app', lambda *_args: {'pid': 42}),
                            ('compare_ppm', compare), ('locate_fixture_paint', lambda _path: dict(paint)),
                            ('load_profile', lambda name: physical if name == physical['name'] else original_profile(name)),
                            ('pointer', lambda *_args: None), ('key', lambda *_args: None),
                            ('collect', lambda *_args: None)):
                        stack.enter_context(patch.object(lab, name, side_effect=replacement))
                    stack.enter_context(patch.object(lab.time, 'sleep'))
                    result = lab.fixture_run(args)
                self.assertTrue(result['ok'], result)
                parameters = lab.read_json(run / 'parameters.json')
                self.assertEqual((parameters['expectedArtifact']['width'], parameters['expectedArtifact']['height']),
                                 (frame['width'], frame['height']))
                self.assertEqual(parameters['expectedArtifact']['areas'][0]['x'], left)
                self.assertEqual(parameters['expectedArtifact']['areas'][0]['y'], top)
                asked_kwin = any('queryWindowInfo' in script for script in scripts)
                self.assertEqual(asked_kwin, kwin_frame is not None)
                if kwin_frame:
                    self.assertEqual(result['independentWindowFrame']['oracle'], 'pre-action KWin queryWindowInfo')

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

    def test_neon_profile_is_a_bios_appimage_plasma_wayland_guest_with_a_locked_iso(self):
        lock = json.loads((ROOT / 'tools/lab/image-lock.json').read_text())
        for name, sha256 in (('kde-neon-user', 'b1b3bd85136a358713b73155cc662170337bed6d702c65488266a40fec842e3a'),
                             ('kde-neon-unstable', 'e81d627d27a61b0f3d106998d4377b6e3677b1cea6725e24123889b7e01dc6eb')):
            self.assertEqual(lock['images'][name]['sha256'], sha256)
            self.assertEqual(lock['images'][name]['signingFingerprint'], 'B74EA2162376765BEAC3AE4345F4C354638D1F29')
        profile = lab.load_profile('neon-kde-wayland')
        self.assertEqual((profile['desktop'], profile['session'], profile['session_process']), ('KDE', 'wayland', 'plasmashell'))
        self.assertEqual(profile['image'], 'kde-neon')
        self.assertIsNone(lab.uefi_firmware(profile))
        # The Ubuntu 24.04 DEB is built on Qt 6.4.2 without the LayerShellQt region overlay.
        self.assertEqual(lab.install_format(profile), 'appimage')
        self.assertEqual(profile['fixture_hotkeys'], lab.load_profile('kde-wayland')['fixture_hotkeys'])


if __name__ == '__main__':
    unittest.main()
