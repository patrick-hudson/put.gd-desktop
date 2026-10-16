"""Host isolation checks; these do not prove a Windows desktop works."""
import importlib.util
from pathlib import Path
import shutil
import subprocess
from unittest.mock import patch, MagicMock
import sys
import tempfile
import unittest

LAB = Path(__file__).resolve().parents[2] / 'tools/lab'
sys.path.insert(0, str(LAB))
import overlay_cleanup
spec = importlib.util.spec_from_file_location('windows_lab', LAB / 'windows.py')
windows = importlib.util.module_from_spec(spec)
spec.loader.exec_module(windows)


@unittest.skipUnless(shutil.which('qemu-img') and shutil.which('qemu-io'), 'requires QEMU distro tools')
class WindowsIsolation(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        root_patch = patch.object(windows, 'ROOT', self.root)
        root_patch.start()
        self.addCleanup(root_patch.stop)
        lock_root = patch.object(overlay_cleanup, 'ROOT', self.root)
        lock_root.start()
        self.addCleanup(lock_root.stop)
        self.base = self.root / 'base'
        self.base.mkdir()
        windows.command('qemu-img', 'create', '-f', 'qcow2', self.base / 'disk.qcow2', '16M')
        (self.base / 'vars.fd').write_bytes(b'synthetic firmware')
        (self.base / 'tpm').mkdir()
        (self.base / 'tpm/state').write_bytes(b'synthetic TPM')
        windows.save(self.base / 'state.json', {'os': 'windows11', 'uuid': 'test', 'clean_shutdown': True})

    def test_independent_disk_firmware_and_tpm(self):
        windows.seal(self.base, 'synthetic host test only')
        expected = windows.load(self.base / 'manifest.json')['sha256']
        one, two = self.root / 'one', self.root / 'two'
        windows.clone(self.base, one)
        windows.clone(self.base, two)
        self.assertNotEqual(windows.load(one / 'state.json').get('run_id'),
                            windows.load(two / 'state.json').get('run_id'))
        self.assertEqual(windows.load(one / 'state.json')['uuid'], windows.load(two / 'state.json')['uuid'])
        windows.command('qemu-io', '-f', 'qcow2', '-c', 'write -P 0x5a 0 4096', one / 'disk.qcow2')
        (one / 'vars.fd').write_bytes(b'changed firmware')
        (one / 'tpm/state').write_bytes(b'changed TPM')
        windows.command('qemu-io', '-f', 'qcow2', '-c', 'read -P 0 0 4096', two / 'disk.qcow2')
        self.assertEqual((two / 'vars.fd').read_bytes(), b'synthetic firmware')
        self.assertEqual((two / 'tpm/state').read_bytes(), b'synthetic TPM')
        for name, sha in expected.items():
            self.assertEqual(windows.digest(self.base / name), sha)
        self.assertFalse(windows.load(one / 'state.json')['clean_shutdown'])

    def test_sealed_base_cannot_boot_or_clone_if_changed(self):
        windows.seal(self.base, 'synthetic host test only')
        with self.assertRaisesRegex(windows.LabError, 'Sealed bases cannot boot'):
            windows.launch(self.base, 30)
        (self.base / 'vars.fd').chmod(0o600)
        (self.base / 'vars.fd').write_bytes(b'corrupted')
        with self.assertRaisesRegex(windows.LabError, 'checksum mismatch'):
            windows.clone(self.base, self.root / 'bad')
        self.assertFalse((self.root / 'bad').exists())

    def test_unclean_base_cannot_be_sealed(self):
        windows.save(self.base / 'state.json', {'clean_shutdown': False})
        with self.assertRaisesRegex(windows.LabError, 'clean shutdown'):
            windows.seal(self.base, 'synthetic host test only')

    def test_sealed_shutdown_preserves_cloneability(self):
        windows.seal(self.base, 'synthetic host test only')
        with self.assertRaisesRegex(windows.LabError, 'Sealed bases'):
            windows.shutdown(self.base, force=True)
        windows.clone(self.base, self.root / 'still-valid')

    def test_cross_process_lifecycle_lock(self):
        script = "import sys; from pathlib import Path; sys.path.insert(0,sys.argv[1]); import windows, overlay_cleanup; windows.ROOT=Path(sys.argv[2]); overlay_cleanup.ROOT=Path(sys.argv[2]); windows.seal(Path(sys.argv[3]),'blocked')"
        with windows.bundle_lock(self.base):
            for operation in (lambda: windows.launch(self.base, 30), lambda: windows.shutdown(self.base), lambda: windows.clone(self.base, self.root / 'blocked-clone')):
                with self.assertRaisesRegex(windows.LabError, 'VM busy'):
                    operation()
            result = subprocess.run([sys.executable, '-c', script, str(LAB), str(self.root), str(self.base)], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('VM busy', result.stderr)
        self.assertFalse((self.base / 'manifest.json').exists())


class ShutdownTransitions(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.vm = self.root / 'vm'
        self.vm.mkdir()
        root_patch = patch.object(windows, 'ROOT', self.root)
        root_patch.start()
        self.addCleanup(root_patch.stop)
        lock_root = patch.object(overlay_cleanup, 'ROOT', self.root)
        lock_root.start()
        self.addCleanup(lock_root.stop)
        windows.save(self.vm / 'state.json', {'clean_shutdown': False})

    def test_clean_and_repeated_shutdown(self):
        qmp = MagicMock()
        qmp.execute.return_value = {'status': 'shutdown'}
        with patch.object(windows, 'QMP') as connect, patch.object(windows, 'owned_pid', side_effect=[123, None, None]), patch.object(windows, 'stop_tpm'):
            connect.return_value.__enter__.return_value = qmp
            windows.shutdown(self.vm)
        self.assertTrue(windows.load(self.vm / 'state.json')['clean_shutdown'])
        with patch.object(windows, 'owned_pid', return_value=None), patch.object(windows, 'stop_tpm'):
            windows.shutdown(self.vm)
        self.assertTrue(windows.load(self.vm / 'state.json')['clean_shutdown'])

    def test_force_does_not_require_qmp_and_cannot_seal(self):
        windows.save(self.vm / 'state.json', {'clean_shutdown': True})
        with patch.object(windows, 'QMP', side_effect=AssertionError('QMP unavailable')), patch.object(windows, 'owned_pid', side_effect=[123, None, None]), patch.object(windows.os, 'kill') as kill, patch.object(windows, 'stop_tpm'):
            windows.shutdown(self.vm, force=True)
        kill.assert_called_once_with(123, windows.signal.SIGTERM)
        self.assertFalse(windows.load(self.vm / 'state.json')['clean_shutdown'])

    def test_timeout_does_not_quit_or_record_clean(self):
        qmp = MagicMock()
        with patch.object(windows, 'QMP') as connect, patch.object(windows, 'owned_pid', return_value=123), patch.object(windows.time, 'monotonic', side_effect=[0, 181]), patch.object(windows, 'stop_tpm') as stop:
            connect.return_value.__enter__.return_value = qmp
            with self.assertRaisesRegex(windows.LabError, 'did not shut down'):
                windows.shutdown(self.vm)
        qmp.execute.assert_called_once_with('system_powerdown')
        stop.assert_not_called()
        self.assertFalse(windows.load(self.vm / 'state.json')['clean_shutdown'])

    def test_tpm_failure_does_not_record_clean(self):
        qmp = MagicMock()
        qmp.execute.return_value = {'status': 'shutdown'}
        with patch.object(windows, 'QMP') as connect, patch.object(windows, 'owned_pid', side_effect=[123, None, None]), patch.object(windows, 'stop_tpm', side_effect=windows.LabError('TPM did not stop')):
            connect.return_value.__enter__.return_value = qmp
            with self.assertRaisesRegex(windows.LabError, 'TPM did not stop'):
                windows.shutdown(self.vm)
        self.assertFalse(windows.load(self.vm / 'state.json')['clean_shutdown'])

    def test_qemu_exit_failure_does_not_record_clean(self):
        qmp = MagicMock()
        qmp.execute.return_value = {'status': 'shutdown'}
        with patch.object(windows, 'QMP') as connect, patch.object(windows, 'owned_pid', return_value=123), patch.object(windows.time, 'sleep'), patch.object(windows, 'stop_tpm') as stop:
            connect.return_value.__enter__.return_value = qmp
            with self.assertRaisesRegex(windows.LabError, 'QEMU has not exited'):
                windows.shutdown(self.vm)
        stop.assert_not_called()
        self.assertFalse(windows.load(self.vm / 'state.json')['clean_shutdown'])

    def test_state_save_failure_stops_started_tpm(self):
        windows.save(self.vm / 'state.json', {'uuid': 'test', 'os': 'windows11'})
        with patch.object(windows, 'stopped'), patch.object(windows.shutil, 'which', return_value='/usr/bin/test'), patch.object(windows.os, 'access', return_value=True), patch.object(windows.socket, 'socket'), patch.object(windows, 'command') as command, patch.object(windows, 'save', side_effect=OSError('write failure')), patch.object(windows, 'stop_tpm') as stop:
            with self.assertRaisesRegex(OSError, 'write failure'):
                windows.launch(self.vm, 30)
        self.assertEqual(command.call_args.args[0], 'swtpm')
        stop.assert_called_once_with(self.vm)

    def test_state_load_failure_starts_no_daemon(self):
        (self.vm / 'state.json').write_text('invalid json')
        with patch.object(windows, 'stopped'), patch.object(windows.shutil, 'which', return_value='/usr/bin/test'), patch.object(windows.os, 'access', return_value=True), patch.object(windows.socket, 'socket'), patch.object(windows, 'command') as command:
            with self.assertRaises(ValueError):
                windows.launch(self.vm, 30)
        command.assert_not_called()


if __name__ == '__main__':
    unittest.main()
