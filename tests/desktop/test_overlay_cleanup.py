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

    def test_opaque_unrelated_process_requires_native_lock_but_opaque_vm_refuses(self):
        proc = self.root / 'opaque-proc'
        (proc / 'self').mkdir(parents=True)
        (proc / 'self/status').write_text('NSpid:\t123\n')
        (proc / 'mounts').write_text('proc /proc proc rw 0 0\n')
        process = proc / '123'
        (process / 'fd').mkdir(parents=True)
        (process / 'comm').write_text('gpg-agent\n')
        (process / 'cmdline').write_bytes(b'gpg-agent\0')
        read_text = Path.read_text
        def protected(path, *args, **kwargs):
            if path == process / 'maps':
                raise PermissionError('synthetic nondumpable process')
            return read_text(path, *args, **kwargs)
        with patch.object(cleanup, '_PROC', proc), patch.object(Path, 'read_text', protected):
            with self.assertRaisesRegex(cleanup.CleanupError, 'not fully visible'):
                self.process_probe(self.run, self.run / 'disk.qcow2')
            with cleanup._image_lock(self.run / 'disk.qcow2') as lock:
                self.process_probe(self.run, self.run / 'disk.qcow2', lock_process=lock)
                (process / 'comm').write_text('qemu-system-x86\n')
                (process / 'cmdline').write_bytes(b'qemu-system-x86_64\0')
                with self.assertRaisesRegex(cleanup.CleanupError, 'not fully visible'):
                    self.process_probe(self.run, self.run / 'disk.qcow2', lock_process=lock)

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
