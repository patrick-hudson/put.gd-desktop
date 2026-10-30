"""Cleanup contracts; tiny real qcow2 chains, no guest boot or service access."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('overlay_cleanup', ROOT / 'tools/lab/overlay_cleanup.py')
cleanup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cleanup)


class ProcessInventory(unittest.TestCase):
    """No VM/tools/root needed; all opaque process cases must use evidence."""
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.disk = self.root / 'disk.qcow2'
        self.disk.write_bytes(b'fixture')
        self.proc = self.root / 'proc'
        (self.proc / 'self').mkdir(parents=True)
        (self.proc / 'self/status').write_text('NSpid: 123\n')
        (self.proc / 'mounts').write_text('proc /proc proc rw,hidepid=0 0 0\n')
        self.process = self.proc / '123'
        (self.process / 'fd').mkdir(parents=True)
        (self.process / 'stat').write_text('123 (with spaces) ' + ' '.join(['0'] * 20))
        (self.process / 'comm').write_text('python3\n')
        (self.process / 'cmdline').write_bytes(b'python3\0')
        (self.process / 'maps').write_text('')
        self.proc_patch = patch.object(cleanup, '_PROC', self.proc)
        self.proc_patch.start()
        self.addCleanup(self.proc_patch.stop)

    def probe(self):
        cleanup._check_processes(self.root, self.disk)

    def test_visible_builder_path_does_not_invoke_sudo(self):
        with patch.object(cleanup, '_privileged_process_inventory') as privileged:
            self.probe()
            privileged.assert_not_called()

    def test_foreign_and_root_holders_are_never_exempt(self):
        original = Path.stat
        for uid in (0, os.getuid() + 1):
            def foreign(path, *args, **kwargs):
                value = original(path, *args, **kwargs)
                if path == self.process:
                    fields = list(value)
                    fields[4] = uid
                    return os.stat_result(fields)
                return value
            with self.subTest(uid=uid), patch.object(Path, 'stat', foreign):
                (self.process / 'fd/3').symlink_to(self.disk)
                with self.assertRaisesRegex(cleanup.CleanupError, 'overlay open'):
                    self.probe()
                (self.process / 'fd/3').unlink()

    def test_unknown_foreign_or_root_fds_require_sudo_and_refuse_without_it(self):
        original = Path.stat
        for uid in (0, os.getuid() + 1):
            def opaque(path, *args, **kwargs):
                if path == self.process / 'fd/0':
                    raise PermissionError('unknown descriptor')
                value = original(path, *args, **kwargs)
                if path == self.process:
                    fields = list(value)
                    fields[4] = uid
                    return os.stat_result(fields)
                return value
            (self.process / 'fd/0').touch()
            with self.subTest(uid=uid), patch.object(Path, 'stat', opaque):
                with patch.object(cleanup, '_privileged_process_inventory') as privileged:
                    self.probe()
                    privileged.assert_called_once()
                with patch.object(cleanup, '_privileged_process_inventory',
                                  side_effect=cleanup.CleanupError('incomplete')):
                    with self.assertRaisesRegex(cleanup.CleanupError, 'incomplete'):
                        self.probe()
            (self.process / 'fd/0').unlink()

    def test_hidepid_and_nested_pid_namespace_never_try_sudo(self):
        with patch.object(cleanup, '_privileged_process_inventory') as privileged:
            for option in ('1', '2', '4', 'invisible', 'ptraceable'):
                (self.proc / 'mounts').write_text(f'proc /proc proc rw,hidepid={option} 0 0\n')
                with self.subTest(option=option), self.assertRaisesRegex(cleanup.CleanupError, 'Hidden'):
                    self.probe()
            (self.proc / 'mounts').write_text('proc /proc proc rw 0 0\n')
            (self.proc / 'self/status').write_text('NSpid: 123 1\n')
            with self.assertRaisesRegex(cleanup.CleanupError, 'namespace'):
                self.probe()
            privileged.assert_not_called()

    def test_privileged_probe_is_isolated_read_only_and_accepts_only_clear(self):
        info = self.disk.stat()
        for code, stdout, stderr in ((0, 'clear\n', ''), (1, 'clear\n', ''),
                                      (0, '', ''), (0, 'blocked\n', ''),
                                      (0, 'unavailable\n', ''), (0, 'clear\n', 'warning')):
            with self.subTest(code=code, stdout=stdout, stderr=stderr):
                with patch.object(cleanup.subprocess, 'run',
                                  return_value=subprocess.CompletedProcess([], code, stdout, stderr)) as run:
                    if (code, stdout, stderr) == (0, 'clear\n', ''):
                        cleanup._privileged_process_inventory(self.root, info.st_dev, info.st_ino, -1)
                    else:
                        with self.assertRaises(cleanup.CleanupError):
                            cleanup._privileged_process_inventory(self.root, info.st_dev, info.st_ino, -1)
                    args = run.call_args.args[0]
                    self.assertEqual(args[:8], ['sudo', '-n', '--', '/usr/bin/python3', '-I', '-B', '-c', args[7]])
                    self.assertEqual(args[7], cleanup._PRIVILEGED_PROBE)
                    self.assertEqual(run.call_args.kwargs['timeout'], 15)
                    # Run the exact transmitted program against synthetic /proc.
                    # No import of checkout code occurs in the elevated program.
                    source = args[7].replace('Path("/proc")', 'Path(' + repr(str(self.proc)) + ')')
                result = subprocess.run([sys.executable, '-I', '-c', source, str(self.root),
                                         str(info.st_dev), str(info.st_ino), '-1'],
                                        capture_output=True, text=True, timeout=5)
                self.assertEqual((result.returncode, result.stdout, result.stderr), (0, 'clear\n', ''))

    def test_privileged_probe_is_captured_at_import_and_names_holders(self):
        # A checkout switched mid-run must not change the program that runs as root.
        with patch.object(cleanup.inspect, 'getsource', return_value='raise SystemExit(9)\n'), \
             patch.object(cleanup.subprocess, 'run',
                          return_value=subprocess.CompletedProcess([], 0, 'clear\n', '')) as run:
            cleanup._privileged_process_inventory(self.root, 1, 2, -1)
        self.assertNotIn('SystemExit(9)', run.call_args.args[0][7])
        for stdout, message in (('blocked\n', 'found a process using the overlay'),
                                ('unavailable\n', 'unavailable or incomplete')):
            with self.subTest(stdout=stdout), \
                 patch.object(cleanup.subprocess, 'run',
                              return_value=subprocess.CompletedProcess([], 0, stdout, '')):
                with self.assertRaisesRegex(cleanup.CleanupError, message):
                    cleanup._privileged_process_inventory(self.root, 1, 2, -1)

    def test_privileged_program_refuses_held_hidden_and_unknown_inventory(self):
        with patch.object(cleanup.subprocess, 'run',
                          return_value=subprocess.CompletedProcess([], 0, 'clear\n', '')) as run:
            cleanup._privileged_process_inventory(self.root, 1, 2, -1)
            source = run.call_args.args[0][7].replace(
                'Path("/proc")', 'Path(' + repr(str(self.proc)) + ')')
        info = self.disk.stat()
        for case in ('held', 'mapped', 'hidepid', 'unknown-fds', 'malformed-identity'):
            (self.proc / 'mounts').write_text('proc /proc proc rw 0 0\n')
            program = source
            if case == 'held':
                (self.process / 'fd/3').symlink_to(self.disk)
            elif case == 'mapped':
                (self.process / 'maps').write_text(
                    f'1000-2000 r--p 0 {os.major(info.st_dev):x}:{os.minor(info.st_dev):x} {info.st_ino} /disk\n')
            elif case == 'hidepid':
                (self.proc / 'mounts').write_text('proc /proc proc rw,hidepid=2 0 0\n')
            elif case == 'unknown-fds':
                program = program.replace("for descriptor in (directory / 'fd').iterdir():",
                                          "raise PermissionError('unknown root FDs')\n            for descriptor in (directory / 'fd').iterdir():")
            else:
                (self.process / 'stat').write_text('malformed')
            result = subprocess.run([sys.executable, '-I', '-c', program, str(self.root),
                                     str(info.st_dev), str(info.st_ino), '-1'],
                                    capture_output=True, text=True, timeout=5)
            with self.subTest(case=case):
                self.assertEqual(result.returncode, 0)
                self.assertEqual(result.stdout, 'unavailable\n' if case == 'unknown-fds' else 'blocked\n')
                self.assertEqual(result.stderr, '')
            (self.process / 'fd/3').unlink(missing_ok=True)
            (self.process / 'maps').write_text('')

    def test_live_pid_missing_inventory_and_changed_identity_refuse(self):
        (self.process / 'maps').unlink()
        with self.assertRaisesRegex(cleanup.CleanupError, 'live PID'):
            self.probe()
        (self.process / 'maps').write_text('')
        original = Path.read_text
        count = 0
        def changing(path, *args, **kwargs):
            nonlocal count
            if path == self.process / 'stat':
                count += 1
                return '123 (fixture) ' + ' '.join(['0'] * 19 + [str(count)])
            return original(path, *args, **kwargs)
        with patch.object(Path, 'read_text', changing):
            with self.assertRaisesRegex(cleanup.CleanupError, 'identity changed'):
                self.probe()

    def test_sudo_missing_or_timed_out_refuses(self):
        for error in (FileNotFoundError(), subprocess.TimeoutExpired('sudo', 15)):
            with self.subTest(error=error), patch.object(cleanup.subprocess, 'run', side_effect=error):
                with self.assertRaisesRegex(cleanup.CleanupError, 'sudo inventory unavailable'):
                    cleanup._privileged_process_inventory(self.root, 1, 2, -1)


@unittest.skipUnless(shutil.which('qemu-img') and shutil.which('qemu-io'), 'distro QEMU disk utilities required')
class OverlayCleanup(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='overlay-cleanup-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.root_patch = patch.object(cleanup, 'ROOT', self.root)
        self.root_patch.start()
        self.addCleanup(self.root_patch.stop)
        self.process_probe = cleanup._check_processes
        self.process_patch = patch.object(cleanup, '_check_processes')
        self.process_patch.start()
        self.addCleanup(self.process_patch.stop)
        self.base = self.root / 'work/lab-images/golden/disk.qcow2'
        self.base.parent.mkdir(parents=True)
        self.qemu('create', '-f', 'qcow2', self.base, '1M')
        self.run = self.root / 'work/lab-runs/example'
        self.run.mkdir(parents=True)
        self.qemu('create', '-f', 'qcow2', '-F', 'qcow2', '-b', self.base, self.run / 'disk.qcow2')
        self.write(self.run / 'state.json', {'schemaVersion': 1, 'id': 'fixture-run',
            'overlay': str(self.run / 'disk.qcow2'), 'golden': str(self.base), 'profile': 'gnome-wayland'})
        self.evidence()

    def qemu(self, *args):
        subprocess.run(['qemu-img', *map(str, args)], check=True, capture_output=True)

    def write(self, path, value):
        path.write_text(json.dumps(value) + '\n')

    def evidence(self, ok=True):
        evidence = self.run / 'tests/final'
        evidence.mkdir(parents=True, exist_ok=True)
        result = {'testId': 'final', 'ok': ok, 'profile': 'gnome-wayland',
                  'scenario': 'clipboard-independent-paste', 'evidenceDirectory': 'tests/final'}
        self.write(self.run / 'results.json', result)
        self.write(evidence / 'results.json', result)
        (evidence / 'frame.ppm').write_bytes(b'preserved original frame')
        self.write(evidence / 'manifest.json', {'schemaVersion': 1, 'testId': 'final',
            'profile': 'gnome-wayland', 'scenario': 'clipboard', 'files': {
                name: hashlib.sha256((evidence / name).read_bytes()).hexdigest()
                for name in ('results.json', 'frame.ppm')}})
        (self.run / 'test-events.jsonl').write_text('\n'.join(json.dumps(event) for event in [
            {'event': 'started', 'testId': 'final', 'profile': 'gnome-wayland', 'scenario': 'clipboard'},
            {'event': 'finished', 'testId': 'final', 'profile': 'gnome-wayland', 'scenario': 'clipboard',
             'ok': ok, 'collectionIncomplete': False, 'evidenceDirectory': 'tests/final'}]) + '\n')

    def test_cleanup_preserves_every_other_byte_and_is_idempotent(self):
        (self.run / 'identity').write_bytes(b'private key fixture, never exported')
        before = {path: path.read_bytes() for path in self.run.rglob('*')
                  if path.is_file() and path.name != 'disk.qcow2'}
        receipt = cleanup.cleanup(self.run)
        self.assertEqual(receipt['status'], 'completed')
        self.assertFalse((self.run / 'disk.qcow2').exists())
        self.assertEqual(before, {path: path.read_bytes() for path in before})
        self.assertEqual(cleanup.cleanup(self.run), receipt)
        self.assertTrue(self.base.exists())

    def test_uefi_run_retires_only_its_disk_and_keeps_private_nvram(self):
        # UEFI runs carry a private OVMF variables copy, itself a qcow2 image
        # without a backing file; it is firmware state, not a disposable disk.
        nvram = self.run / 'vars.qcow2'
        self.qemu('create', '-f', 'qcow2', nvram, '540K')
        state = json.loads((self.run / 'state.json').read_text())
        state['firmware'] = {'type': 'uefi', 'format': 'qcow2', 'vars': str(nvram),
                             'code': '/usr/share/edk2/ovmf/OVMF_CODE_4M.qcow2'}
        self.write(self.run / 'state.json', state)
        before = nvram.read_bytes()
        receipt = cleanup.cleanup(self.run)
        self.assertEqual(receipt['status'], 'completed')
        self.assertFalse((self.run / 'disk.qcow2').exists())
        self.assertEqual(nvram.read_bytes(), before)
        self.assertTrue(self.base.exists())

    def test_dry_run_does_not_write_or_delete(self):
        before = set(self.run.rglob('*'))
        self.assertEqual(cleanup.cleanup(self.run, dry_run=True)['status'], 'dry-run')
        self.assertEqual(before, set(self.run.rglob('*')))

    def test_corrupt_evidence_unfinished_journal_and_failed_result_refuse(self):
        for failure in ('hash', 'unfinished', 'failed', 'wrong-result'):
            with self.subTest(failure=failure):
                self.evidence()
                if failure == 'hash':
                    (self.run / 'tests/final/frame.ppm').write_bytes(b'changed')
                elif failure == 'unfinished':
                    with (self.run / 'test-events.jsonl').open('a') as stream:
                        stream.write(json.dumps({'event': 'started', 'testId': 'next'}) + '\n')
                elif failure == 'failed':
                    self.evidence(ok=False)
                else:
                    self.write(self.run / 'results.json', {'testId': 'another', 'ok': True})
                with self.assertRaises(cleanup.CleanupError):
                    cleanup.cleanup(self.run)
                self.assertTrue((self.run / 'disk.qcow2').exists())

    def test_later_pass_does_not_retire_unreviewed_failure_guest(self):
        journal = self.run / 'test-events.jsonl'
        journal.write_text(json.dumps({'event': 'finished', 'testId': 'earlier', 'ok': False}) + '\n' + journal.read_text())
        with self.assertRaisesRegex(cleanup.CleanupError, 'Prior failed'):
            cleanup.cleanup(self.run)
        self.assertTrue((self.run / 'disk.qcow2').exists())

    def test_later_pass_keeps_incomplete_or_unfinished_history(self):
        for earlier in ({'event': 'finished', 'testId': 'earlier', 'ok': True, 'collectionIncomplete': True},
                        {'event': 'started', 'testId': 'unfinished'},
                        {'event': 'finished', 'testId': 'legacy', 'ok': True}):
            with self.subTest(earlier=earlier):
                self.evidence()
                journal = self.run / 'test-events.jsonl'
                journal.write_text(json.dumps(earlier) + '\n' + journal.read_text())
                with self.assertRaises(cleanup.CleanupError):
                    cleanup.cleanup(self.run)
                self.assertTrue((self.run / 'disk.qcow2').exists())

    def test_actual_child_backing_chain_refuses_even_with_alias(self):
        other = self.root / 'work/windows/runs/dependent'
        other.mkdir(parents=True)
        alias = self.root / 'overlay-alias.qcow2'
        alias.symlink_to(self.run / 'disk.qcow2')
        self.qemu('create', '-f', 'qcow2', '-F', 'qcow2', '-b', alias, other / 'disk.qcow2')
        with self.assertRaisesRegex(cleanup.CleanupError, 'depend'):
            cleanup.cleanup(self.run)
        self.assertTrue((self.run / 'disk.qcow2').exists())

    def test_scratch_checkout_run_needs_an_explicit_validated_images_root(self):
        # A lab.py copy outside the main tree keeps its runs beside itself,
        # while the golden stays in the main tree's work/lab-images.
        main = self.root / 'main'
        golden = main / 'work/lab-images/golden/disk.qcow2'
        golden.parent.mkdir(parents=True)
        self.qemu('create', '-f', 'qcow2', golden, '1M')
        (self.run / 'disk.qcow2').unlink()
        self.qemu('create', '-f', 'qcow2', '-F', 'qcow2', '-b', golden, self.run / 'disk.qcow2')
        state = json.loads((self.run / 'state.json').read_text())
        self.write(self.run / 'state.json', {**state, 'golden': str(golden)})
        with self.assertRaisesRegex(cleanup.CleanupError, 'outside its canonical'):
            cleanup.cleanup(self.run, dry_run=True)
        alias = self.root / 'images-alias'
        alias.symlink_to(main / 'work/lab-images')
        wrong = main / 'work/other-images'
        wrong.mkdir()
        for root, message in (('main/work/lab-images', 'absolute'), (wrong, 'work/lab-images'),
                              (alias, 'Symlink'), (main / 'work/lab-images/golden', 'work/lab-images')):
            with self.subTest(root=root):
                with self.assertRaisesRegex(cleanup.CleanupError, message):
                    cleanup.cleanup(self.run, dry_run=True, images_root=root)
        # The main tree's runs are inventoried too: a child there keeps the overlay.
        child = main / 'work/lab-runs/dependent/disk.qcow2'
        child.parent.mkdir(parents=True)
        self.qemu('create', '-f', 'qcow2', '-F', 'qcow2', '-b', self.run / 'disk.qcow2', child)
        with self.assertRaisesRegex(cleanup.CleanupError, 'depend'):
            cleanup.cleanup(self.run, images_root=main / 'work/lab-images')
        self.assertTrue((self.run / 'disk.qcow2').exists())
        child.unlink()
        receipt = cleanup.cleanup(self.run, images_root=main / 'work/lab-images')
        self.assertEqual(receipt['status'], 'completed')
        self.assertEqual(receipt['base'], str(golden))
        self.assertFalse((self.run / 'disk.qcow2').exists())
        self.assertTrue(golden.exists())

    def test_backing_dependency_with_img_extension_is_not_missed(self):
        child = self.base.parent / 'dependent.img'
        self.qemu('create', '-f', 'qcow2', '-F', 'qcow2', '-b', self.run / 'disk.qcow2', child)
        with self.assertRaisesRegex(cleanup.CleanupError, 'depend'):
            cleanup.cleanup(self.run)
        self.assertTrue((self.run / 'disk.qcow2').exists())

    def test_native_image_lock_prevents_competing_writer(self):
        with cleanup._image_lock(self.run / 'disk.qcow2'):
            result = subprocess.run(['qemu-img', 'check', str(self.run / 'disk.qcow2')], capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            with self.assertRaises(cleanup.CleanupError):
                cleanup.cleanup(self.run)
        self.assertTrue((self.run / 'disk.qcow2').exists())

    def test_incomplete_dependency_inventory_refuses_retirement(self):
        def unreadable(_root, **kwargs):
            kwargs['onerror'](PermissionError('synthetic inaccessible image directory'))
            return iter(())
        with patch.object(cleanup.os, 'walk', side_effect=unreadable):
            with self.assertRaisesRegex(cleanup.CleanupError, 'inventory'):
                cleanup.cleanup(self.run)
        self.assertTrue((self.run / 'disk.qcow2').exists())

    def test_shared_lock_blocks_linux_and_windows_lifecycle_processes(self):
        script = ('import sys; from pathlib import Path; sys.path.insert(0,sys.argv[1]); '
                  'import lab, windows, overlay_cleanup; overlay_cleanup.ROOT=Path(sys.argv[2]); '
                  '(lab.reset if sys.argv[3]=="linux" else windows.shutdown)(Path(sys.argv[4]))')
        with cleanup.lifecycle_lock():
            for engine in ('linux', 'windows'):
                result = subprocess.run([sys.executable, '-c', script, str(ROOT / 'tools/lab'),
                    str(self.root), engine, str(self.run)], capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('Another VM lifecycle operation', result.stderr)
        self.assertTrue((self.run / 'disk.qcow2').exists())

    def test_opaque_process_always_requires_complete_privileged_inventory(self):
        # Includes same-UID nondumpable applications; the native writer lock
        # cannot rule out an ordinary read-only descriptor or mapping.
        proc = self.root / 'opaque-proc'
        (proc / 'self').mkdir(parents=True)
        (proc / 'self/status').write_text('NSpid:\t123\n')
        (proc / 'mounts').write_text('proc /proc proc rw 0 0\n')
        process = proc / '123'
        (process / 'fd').mkdir(parents=True)
        (process / 'stat').write_text('123 (fixture) ' + ' '.join(['0'] * 20))
        (process / 'comm').write_text('gpg-agent\n')
        (process / 'cmdline').write_bytes(b'gpg-agent\0')
        read_text = Path.read_text
        def protected(path, *args, **kwargs):
            if path == process / 'maps':
                raise PermissionError('synthetic nondumpable process')
            return read_text(path, *args, **kwargs)
        with patch.object(cleanup, '_PROC', proc), patch.object(Path, 'read_text', protected):
            with patch.object(cleanup, '_privileged_process_inventory') as privileged:
                with cleanup._image_lock(self.run / 'disk.qcow2') as lock:
                    self.process_probe(self.run, self.run / 'disk.qcow2', lock_process=lock)
                privileged.assert_called_once()
            with patch.object(cleanup, '_privileged_process_inventory',
                              side_effect=cleanup.CleanupError('inventory unavailable')):
                with cleanup._image_lock(self.run / 'disk.qcow2') as lock:
                    with self.assertRaisesRegex(cleanup.CleanupError, 'unavailable'):
                        self.process_probe(self.run, self.run / 'disk.qcow2', lock_process=lock)

    def test_legacy_outside_run_requires_stopped_relocation_before_cleanup(self):
        outside = self.root / 'legacy-run'
        self.run.rename(outside)
        with self.assertRaisesRegex(cleanup.CleanupError, 'canonical.*stop and relocate.*docs/vm-lab.md'):
            cleanup.cleanup(outside)
        self.assertTrue((outside / 'disk.qcow2').exists())
        # Relocating the stopped bundle back restores its recorded paths. A
        # different canonical destination also requires updating state/evidence.
        outside.rename(self.run)
        cleanup.cleanup(self.run)
        self.assertFalse((self.run / 'disk.qcow2').exists())

    def test_symlink_evidence_state_or_run_and_multilink_disk_refuse(self):
        original = self.run / 'tests/final/frame.ppm'
        retained = self.root / 'frame'
        original.replace(retained)
        original.symlink_to(retained)
        with self.assertRaises(cleanup.CleanupError):
            cleanup.cleanup(self.run)
        original.unlink()
        retained.replace(original)
        alias = self.root / 'alias'
        alias.symlink_to(self.run, target_is_directory=True)
        with self.assertRaises(cleanup.CleanupError):
            cleanup.cleanup(alias)
        state = self.run / 'state.json'
        state.replace(self.root / 'state')
        state.symlink_to(self.root / 'state')
        with self.assertRaises(cleanup.CleanupError):
            cleanup.cleanup(self.run)
        state.unlink()
        (self.root / 'state').replace(state)
        (self.root / 'hardlink').hardlink_to(self.run / 'disk.qcow2')
        with self.assertRaises(cleanup.CleanupError):
            cleanup.cleanup(self.run)

    def test_running_probe_and_mismatched_base_refuse(self):
        with patch.object(cleanup, '_check_processes', side_effect=cleanup.CleanupError('running')):
            with self.assertRaisesRegex(cleanup.CleanupError, 'running'):
                cleanup.cleanup(self.run)
        state = json.loads((self.run / 'state.json').read_text())
        wrong = self.base.parent / 'wrong.qcow2'
        self.qemu('create', '-f', 'qcow2', wrong, '1M')
        state['golden'] = str(wrong)
        self.write(self.run / 'state.json', state)
        with self.assertRaisesRegex(cleanup.CleanupError, 'backing file'):
            cleanup.cleanup(self.run)

    def test_failed_custom_run_needs_hashed_explicit_completion(self):
        self.evidence(ok=False)
        evidence = self.root / 'work/evidence/custom'
        evidence.mkdir(parents=True)
        proof = {'run': str(self.run), 'runId': 'fixture-run', 'completed': True, 'outcome': 'failed'}
        self.write(evidence / 'completion.json', proof)
        self.write(evidence / 'manifest.json', {'schemaVersion': 1,
            'kind': 'vm-overlay-cleanup-evidence', **proof, 'files': {
                'completion.json': hashlib.sha256((evidence / 'completion.json').read_bytes()).hexdigest()}})
        self.assertEqual(cleanup.cleanup(self.run, evidence_manifest=evidence / 'manifest.json')['status'], 'completed')

    def test_prepared_receipt_recovers_after_unlink_without_rewriting_evidence(self):
        save = cleanup._save
        def interrupt_completion(path, value):
            if value['status'] == 'completed':
                raise OSError('simulated interruption after unlink')
            return save(path, value)
        with patch.object(cleanup, '_save', side_effect=interrupt_completion):
            with self.assertRaises(cleanup.CleanupError):
                cleanup.cleanup(self.run)
        self.assertFalse((self.run / 'disk.qcow2').exists())
        self.assertEqual(json.loads((self.run / 'overlay-cleanup.json').read_text())['status'], 'prepared')
        self.assertEqual(cleanup.cleanup(self.run)['status'], 'completed')
        self.qemu('create', '-f', 'qcow2', '-F', 'qcow2', '-b', self.base, self.run / 'disk.qcow2')
        with self.assertRaises(cleanup.CleanupError):
            cleanup.cleanup(self.run)
        self.assertTrue((self.run / 'disk.qcow2').exists())

    def test_process_probe_refuses_open_mapped_or_running_disk_and_hidden_inventory(self):
        proc = self.root / 'proc'
        (proc / 'self').mkdir(parents=True)
        (proc / 'self/status').write_text('NSpid:\t123\n')
        (proc / 'mounts').write_text('proc /proc proc rw 0 0\n')
        process = proc / '123'
        (process / 'fd').mkdir(parents=True)
        (process / 'stat').write_text('123 (fixture) ' + ' '.join(['0'] * 20))
        (process / 'cmdline').write_bytes(b'python3\0')
        (process / 'comm').write_text('python3\n')
        (process / 'maps').write_text('')
        with patch.object(cleanup, '_PROC', proc):
            (process / 'fd/3').symlink_to(self.run / 'disk.qcow2')
            with self.assertRaisesRegex(cleanup.CleanupError, 'open'):
                self.process_probe(self.run, self.run / 'disk.qcow2')
            (process / 'fd/3').unlink()
            identity = (self.run / 'disk.qcow2').stat()
            (process / 'maps').write_text(f'1000-2000 r--p 0 {os.major(identity.st_dev):x}:{os.minor(identity.st_dev):x} {identity.st_ino} /disk\n')
            with self.assertRaisesRegex(cleanup.CleanupError, 'maps'):
                self.process_probe(self.run, self.run / 'disk.qcow2')
            (process / 'maps').write_text('')
            (process / 'cmdline').write_bytes(b'qemu-system-x86_64\0-qmp\0unix:' + str(self.run / 'qmp.sock').encode() + b'\0')
            with self.assertRaisesRegex(cleanup.CleanupError, 'QEMU'):
                self.process_probe(self.run, self.run / 'disk.qcow2')
            (process / 'cmdline').write_bytes(b'python3\0')
            (proc / 'mounts').write_text('proc /proc proc rw,hidepid=2 0 0\n')
            with self.assertRaisesRegex(cleanup.CleanupError, 'Hidden'):
                self.process_probe(self.run, self.run / 'disk.qcow2')

    def test_windows_bundle_base_and_explicit_completion_preserve_firmware_tpm(self):
        base = self.root / 'work/windows/bases/fixture'
        base.mkdir(parents=True)
        self.qemu('create', '-f', 'qcow2', base / 'disk.qcow2', '1M')
        self.write(base / 'manifest.json', {'sha256': {}})
        run = self.root / 'work/windows/runs/fixture'
        run.mkdir(parents=True)
        self.qemu('create', '-f', 'qcow2', '-F', 'qcow2', '-b', base / 'disk.qcow2', run / 'disk.qcow2')
        self.write(run / 'state.json', {'uuid': 'hardware-fixture', 'run_id': 'windows-fixture',
                                      'base': str(base), 'clean_shutdown': True})
        (run / 'vars.fd').write_bytes(b'firmware')
        (run / 'tpm').mkdir()
        (run / 'tpm/state').write_bytes(b'TPM state')
        evidence = run / 'evidence'
        evidence.mkdir()
        proof = {'run': str(run), 'runId': 'windows-fixture', 'completed': True, 'outcome': 'passed'}
        self.write(evidence / 'completion.json', proof)
        self.write(evidence / 'manifest.json', {'schemaVersion': 1,
            'kind': 'vm-overlay-cleanup-evidence', **proof, 'files': {
                'completion.json': hashlib.sha256((evidence / 'completion.json').read_bytes()).hexdigest()}})
        self.assertEqual(cleanup.cleanup(run, evidence_manifest=evidence / 'manifest.json')['status'], 'completed')
        self.assertEqual((run / 'vars.fd').read_bytes(), b'firmware')
        self.assertEqual((run / 'tpm/state').read_bytes(), b'TPM state')


if __name__ == '__main__':
    unittest.main()
