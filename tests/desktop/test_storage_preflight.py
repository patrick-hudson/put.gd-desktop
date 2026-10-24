"""Host-only capacity contracts; no guest or desktop acceptance is implied."""
import importlib.util
import contextlib
import copy
import io
import json
import os
from pathlib import Path
import subprocess
import sys
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('storage_preflight', Path(__file__).resolve().parents[2] / 'tools/lab/storage_preflight.py')
preflight = importlib.util.module_from_spec(spec)
spec.loader.exec_module(preflight)


def measurement(kind='ext2/ext3', magic='ef53', inodes=2048, available_gib=20):
    return {'path': '/measured', 'device': 1, 'filesystemType': kind, 'filesystemMagic': magic,
            'identificationErrorType': None,
            'statvfs': {'f_bsize': 4096, 'f_frsize': 4096, 'f_blocks': 32 * 2**18,
                        'f_bfree': int(available_gib * 2**18), 'f_bavail': int(available_gib * 2**18),
                        'f_files': 4096, 'f_ffree': inodes, 'f_favail': inodes}}


def dynamic_measurement(available_gib=20):
    value = measurement('btrfs', '9123683e', 0, available_gib)
    value['statvfs']['f_files'] = 0
    return value


class CapacityContracts(unittest.TestCase):
    def test_host_reserves_space_for_each_guest_build_and_checks_inodes(self):
        for raw in (measurement(available_gib=16), dynamic_measurement(16)):
            data = preflight.classify_filesystem(raw)
            self.assertTrue(preflight.host_capacity_passes(data, 10, 3, 2))
            raw['statvfs']['f_bavail'] -= 1
            data = preflight.classify_filesystem(raw)
            self.assertFalse(preflight.host_capacity_passes(data, 10, 3, 2))

    def test_reported_inode_boundary_and_exhaustion(self):
        for kind, magic in (('ext2/ext3', 'ef53'), ('tmpfs', '1021994'), ('xfs', '58465342')):
            for inodes in (0, 1024, 1025):
                with self.subTest(kind=kind, inodes=inodes):
                    value = preflight.classify_filesystem(measurement(kind, magic, inodes))
                    self.assertEqual(value['inodeAccounting'], 'reported')
                    self.assertEqual(value['availableInodes'], inodes)
                    self.assertEqual(preflight.host_capacity_passes(value, 10, 0, 1), inodes > 1024)

    def test_verified_btrfs_zeroes_mean_dynamic_not_a_fabricated_count(self):
        raw = dynamic_measurement()
        value = preflight.classify_filesystem(raw)
        self.assertEqual(value['statvfs'], raw['statvfs'])
        self.assertEqual(value['inodeAccounting'], 'dynamic')
        self.assertIsNone(value['availableInodes'])
        self.assertTrue(preflight.host_capacity_passes(value, 10, 0, 1))
        for available in (0, 9):
            value = preflight.classify_filesystem(dynamic_measurement(available))
            self.assertFalse(preflight.host_capacity_passes(value, 10, 0, 1))

    def test_unknown_or_inconsistent_identification_never_authorizes_zeroes(self):
        for kind, magic in ((None, None), ('unknown', 'deadbeef'), ('btrfs', 'ef53'),
                            ('ext2/ext3', '9123683e'), ('btrfs', None)):
            for raw in (dynamic_measurement(), measurement()):
                with self.subTest(kind=kind, magic=magic, raw=raw):
                    raw.update(filesystemType=kind, filesystemMagic=magic)
                    value = preflight.classify_filesystem(raw)
                    self.assertEqual(value['inodeAccounting'], 'unknown')
                    self.assertFalse(preflight.host_capacity_passes(value, 10, 0, 1))

    def test_negative_malformed_and_inconsistent_metrics_are_rejected(self):
        for field in measurement()['statvfs']:
            for invalid in (-1, True, '0', None, 1.5):
                with self.subTest(field=field, invalid=invalid):
                    raw = measurement()
                    raw['statvfs'][field] = invalid
                    with self.assertRaises(ValueError): preflight.classify_filesystem(raw)
        for changes in ({'f_frsize': 0}, {'f_bsize': 0}, {'f_blocks': 0},
                        {'f_bfree': 0}, {'f_ffree': 0}, {'f_files': 0}):
            raw = measurement()
            raw['statvfs'].update(changes)
            with self.assertRaises(ValueError): preflight.classify_filesystem(raw)
        for field in ('f_files', 'f_ffree', 'f_favail'):
            raw = dynamic_measurement()
            raw['statvfs'][field] = 1
            with self.assertRaises(ValueError): preflight.classify_filesystem(raw)
        for field in ('statvfs', 'filesystemMagic', 'device'):
            raw = measurement()
            del raw[field]
            with self.assertRaises(ValueError): preflight.classify_filesystem(raw)

    def test_reserved_space_inodes_and_home_build_estimate(self):
        for factory in (measurement, dynamic_measurement):
            data = {name: preflight.classify_filesystem(factory(available_gib=2))
                    for name in ('home', 'tmp')}
            self.assertTrue(preflight.capacity_passes(data, 2, 0))
            self.assertFalse(preflight.capacity_passes(data, 2, 1))
            data['home'] = preflight.classify_filesystem(factory(available_gib=3))
            self.assertTrue(preflight.capacity_passes(data, 2, 1))
            data['tmp'] = preflight.classify_filesystem(measurement(inodes=1024))
            self.assertFalse(preflight.capacity_passes(data, 2, 1))

    def test_actual_probe_resolves_symlink_and_identifies_its_filesystem(self):
        with tempfile.TemporaryDirectory() as folder:
            link = Path(folder) / 'link'
            link.symlink_to(folder)
            raw = preflight.measure_filesystem(link)
            self.assertEqual(raw['path'], str(Path(folder).resolve()))
            self.assertEqual(raw['device'], Path(folder).stat().st_dev)
            expected = subprocess.check_output(['stat', '-f', '--format=%T:%t', '--', folder], text=True).strip()
            self.assertEqual((raw['filesystemType'], raw['filesystemMagic']), tuple(expected.split(':')))
            self.assertEqual(raw['statvfs']['f_frsize'], os.statvfs(folder).f_frsize)

    def test_host_identification_failures_keep_raw_metrics_and_private_failed_receipt(self):
        failures = [(subprocess.TimeoutExpired(['stat', 'protected argument'], 5,
                                               output='protected output', stderr='protected stderr'), 'TimeoutExpired'),
                    (FileNotFoundError('protected path'), 'FileNotFoundError'),
                    (SimpleNamespace(returncode=1, stdout='protected output', stderr='protected stderr'), 'CommandFailed')]
        malformed = ('protected:opaque', 'protected malformed output', 'btrfs:9123683e\nprotected',
                     'protected\nbtrfs:9123683e', 'btrfs:9123683e:protected',
                     'protected space:ef53', 'protected\x00:ef53', 'protected\r:ef53',
                     'protected' * 10 + ':ef53', 'protected:123456789', 'protected:0x123',
                     'protected:EF53', ':ef53', 'protected:', 'btrfs:9123683e\n\n', None)
        failures += [(SimpleNamespace(returncode=0, stdout=value, stderr='protected stderr'), 'InvalidResponse')
                     for value in malformed]
        for failure, expected_error in failures:
            with self.subTest(failure=type(failure).__name__), tempfile.TemporaryDirectory() as folder:
                output = Path(folder) / 'failed.json'
                guests = {name: preflight.classify_filesystem(measurement()) for name in ('home', 'tmp')}
                options = {'side_effect': failure} if isinstance(failure, Exception) else {'return_value': failure}
                old_mask = os.umask(0o077)
                try:
                    with patch.object(preflight.subprocess, 'run', **options) as command, \
                            patch.object(preflight, 'measure_guest', return_value=guests), \
                            patch.object(sys, 'argv', ['storage_preflight.py', '--run', folder,
                                                     '--host-path', folder, '--output', str(output)]), \
                            contextlib.redirect_stdout(io.StringIO()):
                        self.assertEqual(preflight.main(), 2)
                    receipt = json.loads(output.read_text())
                    self.assertFalse(receipt['hostPassed'])
                    self.assertFalse(receipt['passed'])
                    self.assertEqual(receipt['hostErrorType'], expected_error)
                    host = receipt['hostFilesystem']
                    self.assertEqual(host['inodeAccounting'], 'unknown')
                    self.assertIsNone(host['availableInodes'])
                    self.assertIsNone(host['filesystemType'])
                    self.assertIsNone(host['filesystemMagic'])
                    self.assertEqual(host['device'], Path(folder).stat().st_dev)
                    self.assertEqual(host['statvfs']['f_files'], os.statvfs(folder).f_files)
                    self.assertGreater(host['availableBytes'], 0)
                    self.assertNotIn('protected', output.read_text())
                    self.assertEqual(output.stat().st_mode & 0o777, 0o600)
                    descriptor = command.call_args.kwargs['pass_fds'][0]
                    with self.assertRaises(OSError): os.fstat(descriptor)
                finally:
                    os.umask(old_mask)

    def test_valid_identification_forms_are_retained_without_broadening_support(self):
        for name, magic in (('btrfs', '9123683e'), ('ext2/ext3', 'ef53'),
                            ('tmpfs', '1021994'), ('xfs', '58465342'), ('futurefs', 'deadbeef')):
            for ending in ('', '\n'):
                with tempfile.TemporaryDirectory() as folder, \
                        patch.object(preflight.subprocess, 'run', return_value=SimpleNamespace(
                            returncode=0, stdout=name + ':' + magic + ending)):
                    value = preflight.measure_filesystem(folder)
                    self.assertEqual(value['filesystemType'], name)
                    self.assertEqual(value['filesystemMagic'], magic)
                    self.assertIsNone(value['identificationErrorType'])
                    if name == 'futurefs':
                        unknown = preflight.classify_filesystem(value)
                        self.assertEqual(unknown['inodeAccounting'], 'unknown')
                        self.assertFalse(unknown['inodeCheckPassed'])

    def test_unavailable_or_malformed_host_measurement_still_writes_failure(self):
        for failure in (OSError('protected path'), ValueError('protected metrics')):
            with tempfile.TemporaryDirectory() as folder:
                output = Path(folder) / 'failed.json'
                old_mask = os.umask(0o077)
                try:
                    with patch.object(preflight, 'measure_filesystem', side_effect=failure), \
                            patch.object(preflight, 'measure_guest') as guest, \
                            patch.object(sys, 'argv', ['storage_preflight.py', '--run', folder,
                                                     '--host-path', folder, '--output', str(output)]), \
                            contextlib.redirect_stdout(io.StringIO()):
                        self.assertEqual(preflight.main(), 2)
                    receipt = json.loads(output.read_text())
                    self.assertEqual(receipt['hostErrorType'], type(failure).__name__)
                    self.assertFalse(receipt['passed'])
                    self.assertIsNone(receipt['hostFilesystem'])
                    self.assertIsNone(receipt['hostAvailableBytes'])
                    self.assertIsNone(receipt['hostAvailableInodes'])
                    self.assertFalse(receipt['guests'][0]['passed'])
                    self.assertNotIn('protected', output.read_text())
                    guest.assert_not_called()
                finally:
                    os.umask(old_mask)

    def test_shared_guest_probe_is_self_contained(self):
        with tempfile.TemporaryDirectory() as folder:
            run = self.make_run(folder)
            with patch.object(preflight.subprocess, 'run') as command:
                command.return_value = SimpleNamespace(returncode=0, stdout=json.dumps(
                    {name: measurement() for name in ('home', 'tmp')}))
                preflight.measure_guest(run)
                script = command.call_args.kwargs['input']
            # The exact transmitted probe runs without importing the controller.
            result = subprocess.run([sys.executable, '-'], input=script, text=True, capture_output=True, check=True)
            data = json.loads(result.stdout)
            self.assertEqual(set(data), {'home', 'tmp'})
            self.assertEqual(data['home']['path'], str(Path.home().resolve()))
            self.assertIn('filesystemMagic', data['tmp'])

    def make_run(self, folder):
        run = Path(folder)
        (run/'state.json').write_text(json.dumps({'known_hosts': '/unused', 'ssh_port': 1234,
                                                 'guest_user': 'lab'}))
        key = run/'identity'
        key.write_text('synthetic test input')
        key.chmod(0o600)
        return run

    def test_private_key_and_fail_closed_numeric_response(self):
        with tempfile.TemporaryDirectory() as folder:
            run = self.make_run(folder)
            key = run/'identity'
            with patch.object(preflight.subprocess, 'run') as command:
                for response in ({}, {'home': measurement()}, {'home': measurement(), 'tmp': {}},
                                 {'home': measurement(inodes=-1), 'tmp': measurement()},
                                 {'home': measurement(), 'tmp': measurement(), 'extra': measurement()}):
                    command.return_value = SimpleNamespace(returncode=0, stdout=json.dumps(response))
                    with self.assertRaises(ValueError): preflight.measure_guest(run)
                command.return_value = SimpleNamespace(returncode=1, stdout='', stderr='protected output')
                with self.assertRaises(ValueError) as error: preflight.measure_guest(run)
                self.assertNotIn('protected output', str(error.exception))
                key.chmod(0o644)
                before = command.call_count
                with self.assertRaises(ValueError): preflight.measure_guest(run)
                self.assertEqual(command.call_count, before)

    def test_cli_receipt_device_guard_and_exclusive_private_output(self):
        with tempfile.TemporaryDirectory() as folder:
            run = Path(folder)
            (run / 'disk.qcow2').touch()
            host = dynamic_measurement()
            host['device'] = run.stat().st_dev
            guests = {name: preflight.classify_filesystem(measurement()) for name in ('home', 'tmp')}
            for same_device in (True, False):
                output = run / ('same.json' if same_device else 'different.json')
                raw = copy.deepcopy(host)
                if not same_device: raw['device'] += 1
                with patch.object(preflight, 'measure_filesystem', return_value=raw), \
                        patch.object(preflight, 'measure_guest', return_value=guests) as guest, \
                        patch.object(sys, 'argv', ['storage_preflight.py', '--run', folder,
                                                 '--host-path', folder, '--output', str(output)]), \
                        contextlib.redirect_stdout(io.StringIO()):
                    old_mask = os.umask(0o077)
                    try:
                        self.assertEqual(preflight.main(), 0 if same_device else 2)
                        receipt = json.loads(output.read_text())
                        self.assertEqual(receipt['schemaVersion'], 2)
                        self.assertEqual(receipt['passed'], same_device)
                        self.assertEqual(receipt['hostFilesystem']['statvfs'], host['statvfs'])
                        self.assertIsNone(receipt['hostAvailableInodes'])
                        self.assertTrue(receipt['readOnly'])
                        self.assertEqual(guest.call_count, int(same_device))
                        self.assertEqual(output.stat().st_mode & 0o777, 0o600)
                        with self.assertRaises(FileExistsError): preflight.main()
                    finally:
                        os.umask(old_mask)

    def test_cli_rejects_lower_reserves_and_invalid_budgets_before_measurement(self):
        for flag, value in (('--host-reserve-gib', '9'), ('--guest-reserve-gib', '1'),
                            ('--expected-build-gib', '-1'), ('--expected-build-gib', 'nan'),
                            ('--host-reserve-gib', 'inf')):
            with patch.object(sys, 'argv', ['storage_preflight.py', '--run', '/unused',
                                             '--output', '/unused', flag, value]), \
                    patch.object(preflight, 'measure_filesystem') as probe, \
                    contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as error: preflight.main()
                self.assertEqual(error.exception.code, 2)
                probe.assert_not_called()


if __name__ == '__main__':
    unittest.main()
