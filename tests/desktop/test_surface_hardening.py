#!/usr/bin/env python3
"""Surface authorization regressions using fakes and private local sockets only."""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from types import SimpleNamespace
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.lab import lab
from tools.lab.surface_driver.api import RecipeBlocked
from tools.lab.surface_driver.run import guard_target, main, resolve_boat_cli, verify_guest_endpoint
from tools.lab.surface_driver.target import Target


class LeaseSafety(unittest.TestCase):
    def target(self):
        target = Target.__new__(Target)
        target.physical = SimpleNamespace(ssh=Mock(return_value=json.dumps(
            {'ok': True, 'state': 'held', 'expiresInSeconds': 30})))
        target.run = Path('/tmp/sd-fake')
        target._lock = ('f44', 'owned')
        target._last_heartbeat = 100
        target._lock_valid_until = 130
        target.session = Mock(return_value='{"returncode":0,"stdout":"","stderr":""}')
        target.prefix = Mock(return_value='')
        return target

    def test_every_physical_execute_forces_heartbeat_and_rejects_revocation(self):
        for script in ('nohup putgd --background &', 'xdotool windowactivate 123', 'true'):
            with self.subTest(script=script):
                target = self.target()
                target.physical.ssh.side_effect = [target.physical.ssh.return_value,
                    '{"ok":false,"state":"held","expiresInSeconds":29}']
                with patch('tools.lab.surface_driver.target.time.clock_gettime', return_value=101):
                    target.execute(script)
                    with self.assertRaises(RecipeBlocked):
                        target.execute(script)
                self.assertEqual(target.physical.ssh.call_count, 2)
                target.session.assert_called_once()

    def test_suspend_ages_cached_lease_even_when_monotonic_does_not_advance(self):
        target = self.target()
        target.physical.ssh.return_value = '{"ok":false,"state":"expired","expiresInSeconds":0}'
        with patch('tools.lab.surface_driver.target.time.monotonic', return_value=101), \
             patch('tools.lab.surface_driver.target.time.clock_gettime', return_value=200) as clock:
            with self.assertRaises(RecipeBlocked):
                target.heartbeat()
        target.physical.ssh.assert_called_once()
        self.assertTrue(all(call.args == (time.CLOCK_BOOTTIME,) for call in clock.call_args_list))

    def test_valid_cache_and_suspend_during_heartbeat_round_trip(self):
        target = self.target()
        with patch('tools.lab.surface_driver.target.time.clock_gettime', return_value=101):
            target.heartbeat()
        target.physical.ssh.assert_not_called()
        with patch('tools.lab.surface_driver.target.time.clock_gettime', side_effect=[101, 140]):
            with self.assertRaises(RecipeBlocked):
                target.heartbeat(force=True)


class GuestPeerSafety(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='sd-qmp-')
        self.addCleanup(self.temp.cleanup)
        self.run = Path(self.temp.name)
        self.path = self.run / 'qmp.sock'
        # This synthetic listener stands in for QEMU only in the ownership fake.
        # SO_PEERCRED and /proc checks use the real, exact child identity.
        code = ('import socket,sys; s=socket.socket(socket.AF_UNIX); '
                's.bind(sys.argv[1]); s.listen(16); print("ready",flush=True); sys.stdin.readline()')
        self.process = subprocess.Popen([sys.executable, '-c', code, str(self.path),
            '-qmp', 'unix:'+str(self.path)+',server=on,wait=off'],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        self.addCleanup(self.stop)
        self.assertEqual(self.process.stdout.readline().strip(), 'ready')
        self.target = Target.__new__(Target)
        self.target.run = self.run
        self.target.physical = None
        self.target._lock = None
        self.target.info = {}
        self.args = SimpleNamespace(placement='guest')
        self.command = (Path('/proc') / str(self.process.pid) / 'cmdline').read_bytes()
        with patch.object(lab, 'verify_owned_guest', return_value={'pid': self.process.pid}):
            guard_target(self.args, self.target)

    def stop(self):
        if not self.process.stdin.closed:
            self.process.stdin.close()
        self.process.wait(timeout=5)
        self.process.stdout.close()

    def test_cached_pid_and_command_line_are_rechecked(self):
        verify_guest_endpoint(self.target, self.process.pid, self.command)
        with self.assertRaises(RecipeBlocked):
            verify_guest_endpoint(self.target, self.process.pid, self.command+b'changed')
        with self.assertRaises(RecipeBlocked):
            verify_guest_endpoint(self.target, os.getpid(), self.command)
        self.stop()
        with self.assertRaises(RecipeBlocked):
            self.target.key(['t'])

    def test_socket_swapped_mid_run_blocks_every_guest_input(self):
        with patch.object(lab, 'key') as key:
            self.target.key(['t'])
            key.assert_called_once_with(self.run, ['t'])
        self.path.unlink()
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as replacement:
            replacement.bind(str(self.path))
            replacement.listen(16)
            with patch.object(lab, 'key') as key, patch.object(lab, 'pointer') as pointer, \
                 patch.object(lab, 'input_events') as events:
                for method, args in (('key', (['t'],)), ('pointer', (1, 1, 10, 10)),
                                     ('events', ([],)), ('key_state', (['shift'], True))):
                    with self.subTest(method=method), self.assertRaises(RecipeBlocked):
                        getattr(self.target, method)(*args)
                for command in (key, pointer, events):
                    command.assert_not_called()

    def test_missing_and_symlink_socket_are_rejected(self):
        self.path.unlink()
        with self.assertRaises(RecipeBlocked):
            self.target.key(['t'])
        self.path.symlink_to(self.run / 'other.sock')
        with self.assertRaises(RecipeBlocked):
            self.target.key(['t'])


class BoatCliSafety(unittest.TestCase):
    def test_resolves_once_before_path_changes_and_uses_absolute_cli_for_both_proofs(self):
        with tempfile.TemporaryDirectory(prefix='sd-boat-') as td:
            root = Path(td)
            executable = root / 'real-boat'
            executable.write_text('#!/bin/sh\nexit 0\n')
            executable.chmod(0o700)
            (root / 'boat').symlink_to(executable)
            with patch.dict(os.environ, PATH=str(root)):
                pinned = resolve_boat_cli()
            self.assertEqual(pinned, str(executable))
            target = SimpleNamespace(run=root, physical=SimpleNamespace(
                config={'host': '127.0.0.1', 'user': 'user'}, ssh=Mock(return_value='boot')))
            args = SimpleNamespace(host='boat', placement='boat', boat_id='bx_test', boat_cli=pinned)
            responses = [SimpleNamespace(stdout=json.dumps(v)) for v in (
                {'sandbox': {'id': 'bx_test', 'hydrated': True}}, {'exitCode': 0, 'stdout': 'boot'})]
            with patch.dict(os.environ, PATH='/untrusted'), \
                 patch('tools.lab.surface_driver.run.subprocess.run', side_effect=responses) as command, \
                 patch.object(Path, 'read_text', return_value='boot'), \
                 patch('tools.lab.surface_driver.run.guard_physical_endpoints'):
                guard_target(args, target)
            self.assertEqual([c.args[0][0] for c in command.call_args_list], [pinned, pinned])

    def test_startup_resolves_once_and_records_cli_even_in_blocked_receipt(self):
        with tempfile.TemporaryDirectory(prefix='sd-boat-') as td:
            root = Path(td)
            args = ['--desktop', 'mint-cinnamon-x11', '--host', 'boat', '--placement', 'boat',
                    '--boat-id', 'bx_test', '--run', str(root), '--package', 'deb',
                    '--destination', 'local', '--recipes', 'core_demo', '--catalog', str(root),
                    '--build', 'abcdef', '--out', str(root/'out'), '--coverage', str(root/'coverage.json')]
            target = Mock()
            target.close.return_value = []
            with patch('tools.lab.surface_driver.run.resolve_boat_cli', return_value='/trusted/boat') as resolve, \
                 patch('tools.lab.surface_driver.run.Target', return_value=target), \
                 patch('tools.lab.surface_driver.run.guard_target', side_effect=RecipeBlocked('test stop')):
                self.assertEqual(main(args), 2)
            resolve.assert_called_once_with()
            receipt = json.loads((root/'out/receipt.json').read_text())
            self.assertEqual(receipt['environment']['placement_details']['boat_cli'], '/trusted/boat')

    def test_missing_cli_fails_closed(self):
        with patch('tools.lab.surface_driver.run.shutil.which', return_value=None):
            with self.assertRaises(RecipeBlocked):
                resolve_boat_cli()

    def test_owned_guests_do_not_require_an_unused_boat_cli(self):
        with tempfile.TemporaryDirectory(prefix='sd-boat-') as td:
            root = Path(td)
            target = SimpleNamespace(physical=None, close=lambda: [])
            args = ['--desktop', 'mint-cinnamon-x11', '--host', 'boat', '--placement', 'boat',
                    '--boat-id', 'bx_test', '--run', str(root), '--package', 'source',
                    '--destination', 'local', '--recipes', 'core_demo', '--catalog', str(root),
                    '--build', 'abcdef', '--out', str(root/'out'), '--coverage', str(root/'coverage.json')]
            with patch('tools.lab.surface_driver.run.resolve_boat_cli', side_effect=AssertionError('unused CLI')) as resolve, \
                 patch('tools.lab.surface_driver.run.Target', return_value=target), \
                 patch('tools.lab.surface_driver.run.guard_target', side_effect=RecipeBlocked('test stop')):
                self.assertEqual(main(args), 2)
            resolve.assert_not_called()


if __name__ == '__main__':
    unittest.main()
