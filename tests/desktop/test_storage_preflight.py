"""Host-only capacity contracts; no guest or desktop acceptance is implied."""
import importlib.util
import json
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('storage_preflight', Path(__file__).resolve().parents[2] / 'tools/lab/storage_preflight.py')
preflight = importlib.util.module_from_spec(spec)
spec.loader.exec_module(preflight)


class CapacityContracts(unittest.TestCase):
    def test_host_reserves_space_for_each_guest_build_and_checks_inodes(self):
        self.assertFalse(preflight.host_capacity_passes(10.1 * 2**30, 2000, 10, 3, 2))
        self.assertTrue(preflight.host_capacity_passes(16 * 2**30, 2000, 10, 3, 2))
        self.assertFalse(preflight.host_capacity_passes(20 * 2**30, 900, 10, 0, 2))

    def test_reserved_space_inodes_and_home_build_estimate(self):
        data = {name: {'availableBytes': 2 * 2**30, 'totalBytes': 32 * 2**30,
                       'availableInodes': 2048} for name in ('home', 'tmp')}
        self.assertTrue(preflight.capacity_passes(data, 2, 0))
        self.assertFalse(preflight.capacity_passes(data, 2, 1))
        data['home']['availableBytes'] += 2**30
        self.assertTrue(preflight.capacity_passes(data, 2, 1))
        data['tmp']['availableInodes'] = 1024
        self.assertFalse(preflight.capacity_passes(data, 2, 1))

    def test_private_key_and_fail_closed_numeric_response(self):
        with tempfile.TemporaryDirectory() as folder:
            run = Path(folder)
            (run/'state.json').write_text(json.dumps({'known_hosts': '/unused', 'ssh_port': 1234,
                                                     'guest_user': 'lab'}))
            key = run/'identity'
            key.write_text('synthetic test input')
            key.chmod(0o600)
            with patch.object(preflight.subprocess, 'run') as command:
                command.return_value = SimpleNamespace(returncode=0, stdout='{}')
                with self.assertRaises(ValueError): preflight.measure_guest(run)
                command.return_value = SimpleNamespace(returncode=1, stdout='')
                with self.assertRaises(ValueError): preflight.measure_guest(run)
                key.chmod(0o644)
                before = command.call_count
                with self.assertRaises(ValueError): preflight.measure_guest(run)
                self.assertEqual(command.call_count, before)


if __name__ == '__main__':
    unittest.main()
