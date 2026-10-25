"""Builder-only empty disposable-mount and failure-bundle regression checks.

Run serially with nice -n 10. Never fills storage or opens a display. Every mount
is on a fresh private temporary path; fixture cleanup verifies exact identity.
"""
import hashlib
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
from reproducer_mount import cleanup_mount, inspect_mount, SOURCE

ROOT = pathlib.Path(__file__).resolve().parent
PRODUCT = pathlib.Path('/home/patrick/work/i254/build/dev')
RECEIPTS = []

class MountSafety(unittest.TestCase):
    def test_parent_and_foreign_mounts_are_untouched(self):
        with tempfile.TemporaryDirectory(prefix='i254-m-', dir='/tmp') as directory:
            mount = pathlib.Path(directory)/'mount'; mount.mkdir()
            parent_device = mount.stat().st_dev
            receipt = cleanup_mount(mount, True)
            self.assertIsNone(receipt['observed'])
            self.assertIsNone(receipt['unmountExit'])
            self.assertFalse(receipt['errors'])
            self.assertEqual(mount.stat().st_dev, parent_device)
            RECEIPTS.append({'case': 'no-exact-mount-parent-untouched', 'receipt': receipt})
            # Tiny empty fixture, foreign to the reproducer. No filling at all.
            try:
                self.mount_fixture(mount, 'i254-review-foreign')
                marker = mount/'sentinel'; marker.write_text('synthetic fixture')
                receipt = cleanup_mount(mount, True)
                self.assertTrue(receipt['errors'])
                self.assertIsNone(receipt['unmountExit'])
                self.assertTrue(mount.is_mount())
                self.assertEqual(marker.read_text(), 'synthetic fixture')
                RECEIPTS.append({'case': 'foreign-source-refused', 'receipt': receipt})
            finally:
                self.remove_fixture(mount, 'i254-review-foreign')

    def test_known_device_and_acquisition_guard(self):
        with tempfile.TemporaryDirectory(prefix='i254-m-', dir='/tmp') as directory:
            mount = pathlib.Path(directory)/'mount'; mount.mkdir()
            try:
                self.mount_fixture(mount, SOURCE)
                device = mount.stat().st_dev
                for name, attempted, known in [('no-attempt', False, None), ('changed-device', True, device+1)]:
                    receipt = cleanup_mount(mount, attempted, known)
                    self.assertTrue(receipt['errors'])
                    self.assertIsNone(receipt['unmountExit'])
                    self.assertTrue(mount.is_mount())
                    RECEIPTS.append({'case': name, 'receipt': receipt})
                receipt = cleanup_mount(mount, True, device)
                self.assertFalse(receipt['errors'])
                self.assertEqual(receipt['unmountExit'], 0)
                self.assertFalse(mount.is_mount())
                RECEIPTS.append({'case': 'matching-device-cleaned', 'receipt': receipt})
            finally:
                self.remove_fixture(mount, SOURCE)

    def test_target_type_mismatches_cannot_invoke_umount(self):
        with tempfile.TemporaryDirectory(prefix='i254-m-', dir='/tmp') as directory:
            mount = pathlib.Path(directory)
            for field, value in [('target', '/'), ('fstype', 'ext4')]:
                observed = {'target': str(mount), 'source': SOURCE, 'fstype': 'tmpfs', 'device': 123}
                observed[field] = value
                with patch('reproducer_mount.inspect_mount', return_value=observed), patch('reproducer_mount.subprocess.run') as command:
                    receipt = cleanup_mount(mount, True)
                    self.assertTrue(receipt['errors'])
                    command.assert_not_called()
                    RECEIPTS.append({'case': field+'-mismatch-refused', 'receipt': receipt})

    def test_invalid_args_do_not_create_run(self):
        with tempfile.TemporaryDirectory(prefix='i254-a-', dir='/tmp') as directory:
            run = pathlib.Path(directory)/'never-created'
            result = subprocess.run([sys.executable, str(ROOT/'reproducer.py'), '--build', str(PRODUCT),
                                     '--run', str(run), '--fail-at', 'after-stop'], capture_output=True, text=True)
            self.assertEqual(result.returncode, 2)
            self.assertIn('requires --exhaust', result.stderr)
            self.assertFalse(run.exists())
            RECEIPTS.append({'case': 'invalid-args-before-run', 'exit': result.returncode, 'runCreated': run.exists()})

    def test_failure_bundles_and_uncertain_acquisition(self):
        # Retain these private failure bundles beside the test, outside /tmp.
        # The short actual run paths keep the production socket preflight valid.
        for name, build, extra, expected_exit in [
                ('missing', '/tmp/i254-absent-binaries', [], 1),
                ('version', str(ROOT/'bad-version'), [], 1),
                ('interrupt', str(PRODUCT), ['--fail-at', 'after-mount-before-flag'], 130)]:
            if name == 'version':
                fixture = ROOT/'bad-version'; fixture.mkdir()
                binary = fixture/'putgd-cli'; binary.write_text('#!/bin/sh\nexit 42\n'); binary.chmod(0o700)
            run = pathlib.Path(tempfile.mkdtemp(prefix='i254-i-', dir='/tmp'))/'r'
            result = subprocess.run([sys.executable, str(ROOT/'reproducer.py'), '--build', build,
                                     '--run', str(run), *extra], capture_output=True, text=True, timeout=40)
            (ROOT/(name+'.stdout')).write_text(result.stdout)
            (ROOT/(name+'.stderr')).write_text(result.stderr)
            self.assertEqual(result.returncode, expected_exit, result.stderr)
            report = json.loads((run/'result.json').read_text())
            self.assertFalse(report['passed'])
            self.assertTrue(report['cleanup']['passed'])
            for dependency in ('reproducer.py', 'reproducer_checks.py', 'reproducer_mount.py'):
                self.assertEqual((run/dependency).read_bytes(), (ROOT/dependency).read_bytes())
            imported = subprocess.run([sys.executable, '-c', 'import reproducer_checks, reproducer_mount'], cwd=run, capture_output=True)
            self.assertEqual(imported.returncode, 0)
            if name == 'interrupt':
                self.assertFalse(report['cleanup']['mountFlag'])
                self.assertEqual(report['interruptedSignal'], 2)
                discovered = report['cleanup']['mountDiscovery']
                self.assertEqual(discovered['observed']['target'], str(run/'data/putgd/recording'))
                self.assertEqual(discovered['observed']['source'], SOURCE)
                self.assertEqual(discovered['observed']['fstype'], 'tmpfs')
                self.assertEqual(discovered['unmountExit'], 0)
                self.assertFalse(discovered['mountRemaining'])
            RECEIPTS.append({'case': name, 'run': str(run), 'exit': result.returncode, 'report': report})

    def mount_fixture(self, mount, source):
        subprocess.run(['sudo', '-n', 'mount', '-t', 'tmpfs', '-o',
                        f'size=16M,mode=0700,uid={os.getuid()},gid={os.getgid()},nosuid,nodev', source, str(mount)], check=True)
        observed = inspect_mount(mount)
        self.assertEqual(observed['target'], str(mount))
        self.assertEqual(observed['source'], source)
        self.assertEqual(observed['fstype'], 'tmpfs')

    def remove_fixture(self, mount, source):
        current = inspect_mount(mount)
        if current is None:
            return
        assert current['target'] == str(mount) and current['source'] == source and current['fstype'] == 'tmpfs', current
        subprocess.run(['sudo', '-n', 'umount', str(mount)], check=True)

if __name__ == '__main__':
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(MountSafety)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    summary = {'passed': result.wasSuccessful(), 'testsRun': result.testsRun, 'receipts': RECEIPTS,
               'sourceHashes': {name: hashlib.sha256((ROOT/name).read_bytes()).hexdigest() for name in
                                ('reproducer.py', 'reproducer_checks.py', 'reproducer_mount.py', 'test_reproducer_mount.py')}}
    (ROOT/'mount-safety-results.json').write_text(json.dumps(summary, indent=2))
    sys.exit(0 if result.wasSuccessful() else 1)
